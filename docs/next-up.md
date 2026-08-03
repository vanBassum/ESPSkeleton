# Next up

Rewritten as work lands — this is the ordered view, not a log. The detail lives in
`docs/backlog/`; this file only says what to do next and what is waiting on a decision.

Last updated 2026-08-03, after the command framework and remote-access work.

## Do these first (small, finish the cheap half of the buffer plan)

Detail: [`backlog/2026-08-03-no-dynamic-buffers.md`](backlog/2026-08-03-no-dynamic-buffers.md)

1. **WiFi scan's per-scan array.** `WiFiInterface::ScanNetworks` does
   `new wifi_ap_record_t[count]` on every scan. The caller already caps results, so a
   fixed array sized by that cap removes the allocation. Ten minutes.

2. **The 4 KB upload buffer.** `UpdateManager`'s `HeapBuf` in `partition write` and
   `partition read`. Two routes, pick one:
   - a fixed scratch buffer owned by the transport and lent through `CommandContext`
     (which now exists and is the natural home), or
   - zero-copy: have `Session` expose its current chunk so the handler writes straight
     from the transport's inbound buffer to flash.

   The second is better and is **not** blocked by anything — the JSON envelope already
   ends at a newline and the reader already leaves the stream at the body. (I previously
   claimed this needed the console format; that was wrong.)

3. **`help`.** Also not blocked, for the same kind of reason: the describe-mode outputter
   is just another `ArgReader`, so re-dispatching a command with it works over JSON
   exactly as it would over anything else. Registry walk gives categories and names for
   free; the outputter gives each command's arguments. Categories make it browsable.

## Waiting on a decision, not on code

4. **The relay's per-frame `malloc`** — one allocate/free per inbound WebSocket frame,
   hundreds per upload, which is the shape that fragments a heap. The obvious fixed-size
   replacement is queue depth 16 × 4 KB ≈ 65 KB against 109 KB free heap, so it trades
   against upload throughput, chunk size, or PSRAM. Changing it without deciding that
   only moves the problem.

5. **The relay gate watchdog** — measure idleness, not session length. Decide together
   with the device's own `RECV_TIMEOUT_MS`, since the two interact.
   Detail: [`backlog/2026-08-02-relay-gate-watchdog-upload.md`](backlog/2026-08-02-relay-gate-watchdog-upload.md)

6. **Device→server credential.** The worst gap: anything that guesses the MAC-derived
   device id does not impersonate the device, it **evicts** it. TLS, server-side login
   and persistence sit behind the same question — does this ever face a public network?

## Security, independent of all the above

7. **`settings list` returns passwords in plaintext.** Both `wifi.password` and
   `web.password`, to anyone who reaches the socket while no web password is set — which
   is the default. The relay widens that past the LAN. Needs a decision on whether
   secret-ish settings are write-only over the wire, masked, or gated.

## Open smaller items

- `ConsoleManager` holds a single broadcast callback, which is what forces the
  `WebServerManager` → `RelayManager` edge. Bas wanted to think about what "central"
  should mean before changing it.
- `RelayManager` includes `WebServerManager.h` to borrow the `Authenticator` — the last
  sideways edge between two transports.
- `UpdateManager` now owns only the `partition` category; the name has drifted from the
  contents.
- [`backlog/2026-08-03-command-worker-task.md`](backlog/2026-08-03-command-worker-task.md)
  — the stack-tax and private-API halves that survived the multiplexing rejection. Cost,
  not correctness.

## Parked

The console request format. Motivation reduced to hand-typability, which the frontend
covers; the pull contract keeps the flip cheap whenever it is wanted. Unknown-argument
enforcement is parked with it, deliberately.
See `docs/reasoning/2026-08-03-16h30-the-console-format-is-parked.md`.

## Not for here

Today's structural work flows one way to the KC1245 fork (`lib/protocol`, the pull
contract, two-word routes, auth-as-commands). Nothing needs backporting *from* it —
every shared commit was walked on 2026-08-03.
