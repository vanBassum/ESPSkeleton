# Stream-based commands (NEXT UP)

CommandManager becomes the device's single API surface; every entrance
(WebSocket, HTTP, serial, future relay) is a dumb pipe. The handler
signature drops its JSON coupling:

```cpp
void Handler(Stream& in, Stream& out);   // was (const char* json, JsonWriter& resp)
```

Streams are the **contract** (lowest common denominator — anything can be
faked with a bounded memory stream, but a buffer can never become
limitless). JSON is the **dialect** handlers opt into by constructing
adapters on line one. Bulk commands (firmware bytes) never mention JSON.

Spec: `docs/superpowers/specs/2026-07-03-stream-commands-design.md`
(The exploratory sketch `ideas/stream-commands-example.h` was removed
once the spec captured it; see git history if needed.)

Includes: UpdateManager goes pure (HTTP upload/download routes deleted,
replaced by `updateFromUrl` / `updateBegin`/`Write`/`End` /
`downloadPartition` commands), new `JsonReader` / `MemoryStream` /
`JsonResponse` lib adapters, single `POST /api/command` HTTP route.
