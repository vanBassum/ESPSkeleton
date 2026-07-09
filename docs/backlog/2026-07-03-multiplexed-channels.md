# Multiplexed command channels (transport-level sessions/chunking)

**Update 2026-07-09 — the framing shipped; this is now the concurrency half.**
Step 1 of the session-mux transport
(`docs/superpowers/specs/2026-07-09-session-mux-transport-design.md`) already put
the session id on every chunk and enforces single-in-flight synchronously. What
remains here is concurrency: a slot table + a worker task so several sessions run
at once — which is also what removes the private-API `httpd_ws_get_frame_type`
wart (see the cleanup note below).

Idea (Bas, 2026-07-03): the WS protocol (and later the relay pipe)
should do chunking and sessions itself — frames tagged with a channel
id, many logical streams interleaved over one connection. Then upload,
download, commands, and live logs all run in parallel over one socket,
and no application has to build its own chunking. This is the pattern
HTTP/2 / SSH channels / yamux use.

Fits the stream-commands contract exactly: one channel = one command
invocation; the handler's `(Stream& in, Stream& out)` are fed/ drained
frame-by-frame by the transport. Registry and handlers unchanged.

Related idea (Bas, 2026-07-03, parked): CommandManager owns a worker
task + work queue — entrances hand over (entry, in, out) and return.
Fixes the "every entrance task needs stack for the heaviest command"
tax (one worker sized for the worst case instead of N transports), and
is the natural place for "while one command waits on stream data, run
the next" — cooperative scheduling. Same prerequisite as below.

**Prerequisite that makes or breaks it:** handler execution must move
off the transport task (worker task/pool). Multiplexed framing without
concurrent execution just queues channels behind the running handler —
during a 12 s flash write nothing else would run anyway. Budget:
per-channel buffers + backpressure + task stacks on 320 KB RAM.

**When:** as the core of the relay transport design (see
`2026-07-03-remote-access.md` — this subsumes its open "envelope request ids"
point). Not worth retrofitting today's WS first: only the update flow
needed chunking and it lives in one frontend function; browsers get
parallelism free via extra HTTP connections.

The update session (updateBegin/Write/End) collapses into a single
streamed command once this lands — settled 2026-07-03: the session is
scaffolding around the transport, NOT domain architecture. Bas's
argument, conceded: sector erase happens lazily during writes
(OTA_WITH_SEQUENTIAL_WRITES), ordering within one stream is free, and
finalize can run at end-of-stream. Writes only need cross-request
state because the single-threaded server forces the image into
multiple HTTP requests. Delete begin/end when one long stream can be
served without starving the server.

**Cleanup owed here (added 2026-07-09):** WS inbound streaming
(`2026-07-09-ws-inbound-streaming.md`) ships *before* this, so it drains
body frames synchronously on the httpd task — which the public WS API
can't do (one frame per handler invocation). To make it work it
forward-declares and calls the **non-public** `httpd_ws_get_frame_type`
(declared in ESP-IDF's private `esp_httpd_priv.h`). That is a deliberate
wart, accepted only because a future IDF removing the symbol fails as a
clean linker error, not silent breakage. When the CommandManager worker
task lands with these channels, the httpd task goes back to one-frame-
per-invocation feeding a queue, and this private call must be removed.
