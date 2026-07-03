# Stream-Based Commands — Design

**Date:** 2026-07-03
**Status:** Approved in discussion (this doc records it)

## Goal

CommandManager becomes the device's single API surface. Every entrance —
WebSocket today; HTTP, serial, relay server later — is a dumb pipe that
carries opaque payload bytes to and from named commands. Everything the
device can do is a command; nothing is specific to an entrance.

## Core decision: streams are the contract, JSON is the dialect

The handler signature drops its JSON coupling:

```cpp
void Handler(Stream& in, Stream& out);   // was (const char* json, JsonWriter& resp)
```

- **Contract** (`Stream`): the lowest common denominator. Anything can be
  faked with a bounded memory stream, but a buffer can never become
  limitless. Bulk transfers (firmware images, partition dumps) move
  bytes without base64 and without whole-payload RAM.
- **Dialect** (JSON): handlers with structured data construct JSON
  adapters on line one of their body. Bulk handlers never mention JSON.
  Nothing below the handler knows which choice it made. If a command
  ever wants CBOR or plain text, only that handler changes.

There is exactly ONE handler shape. No dual-signature compatibility
layer; all existing handlers are migrated in this rework.

## Components

### CommandEntry v2 (`CommandManager/CommandEntry.h`)

Unchanged intrusive registry (chain links, `inline static` owner tables,
ctx stamping, FATAL-on-destruction), new handler type:

```cpp
struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, Stream& in, Stream& out);
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;
};
```

Trampoline: same owner-deduction pattern as today (`CommandOwner`
specializations for non-const and const members; `if constexpr` branch
for free/static functions), argument list `(Stream&, Stream&)`.

`CommandManager::Execute(const char* name, Stream& in, Stream& out)`
returns bool (command found). Find under lock, handler outside lock —
unchanged.

### New lib adapters

- **`MemoryStream`** (`lib/common/MemoryStream.h`): read-only `Stream`
  view over an existing byte range (`buf`, `len`, advancing `pos`).
  `write()` returns 0; `available()` reports remaining. Used by
  transports to present received frames as streams. Deliberately NOT
  folded into BufferStream: BufferStream's concept is chunked I/O
  buffering (e.g. relieving SPI of byte-per-byte reads); MemoryStream
  is "a stream backed by memory". Different things, different names.
- **`JsonReader`** (`lib/json/JsonReader.h`): buffered, not streaming.
  Template capacity with internal buffer: `JsonReader<1024> req(in);`
  consumes `in` at construction (truncating past capacity, logging like
  BufferStream does), then serves typed getters wrapping the existing
  JsonHelpers: `GetString(key, out, maxLen) -> bool`,
  `GetInt(key, default)`, `GetBool(key, default)`. A streaming parser
  can replace the internals later; handlers only see getters.
- **`JsonObject` / `JsonArray`** (`lib/json/JsonScope.h`): RAII scope
  writers — the early-return fix at EVERY nesting level, not just the
  outer braces. A scope writes its opener at construction and its
  closer at destruction; C++ reverse-declaration destruction order
  closes inner scopes first, so JSON is well-formed on every return
  path:

  ```cpp
  JsonObject root(out);                 // { … } via RAII
  root.field("firmware", ...);
  JsonObject nested = root.object("xx"); // "xx":{ … }
  nested.field("y", 1);
  ```                                    // } } on scope exit

  `field(key, value)` for leaves; `object(key)` / `array(key)` return
  child scopes (guaranteed copy elision — no copies, one destructor).
  The one misuse RAII cannot prevent — writing to a parent while a
  child scope is open, or two live siblings — is caught by a
  `childOpen` flag + assert: dies loudly on first run. Copy/assignment
  deleted. Existing naked `beginObject/endObject` JsonWriter callers
  (MQTT discovery, PublishState) may migrate opportunistically; not
  part of this rework.

`BufferStream` (write-side compose-in-RAM) and `JsonWriter` (writes to
any `Stream`) already exist and are unchanged.

### Handler shapes (the two extremes)

```cpp
// JSON dialect — typical structured command:
void SystemManager::Cmd_Info(Stream& in, Stream& out)
{
    JsonObject resp(out);                // {  ... } via RAII
    resp.field("firmware", ...);
}

// Raw — bulk bytes, JSON never mentioned:
void UpdateManager::Cmd_UpdateWrite(Stream& in, Stream& out)
{
    char buf[1024];
    while (size_t n = in.read(buf, sizeof(buf), timeout))
        if (!WriteChunk(buf, n)) { /* error reply; return */ }
    out.write("{\"ok\":true}", 11);
}
```

### WebSocket transport (WebSocketHandler)

Text frames, envelope as today on the request side:
`{"id": N, "type": "<command>", ...params}`. Dispatch:

- `in`  = `MemoryStream` over the received frame bytes (the handler's
  JsonReader sees the whole message; envelope fields are harmless).
- `out` = `BufferStream` over the existing 4 KB response buffer.

Response envelope changes from flat to **nested by concatenation**:
the transport writes `{"id":N,"payload":`, then the handler's bytes,
then `}`. Unknown command: `{"id":N,"error":"<type>"}` (no payload).
This works because a JSON-dialect reply is exactly one complete JSON
object. Raw/bulk replies are NOT supported over WS (bulk goes over
HTTP); broadcasts (console log lines) are unchanged.

Frontend (`backend.ts`): `req.resolve(msg.payload)` instead of
`resolve(msg)`; error path unchanged. Pages are untouched — they
already consume the resolved object's fields.

### HTTP transport (WebServerManager) — phase 2

One route replaces all bespoke API routes: **`POST /api/command?type=<name>`**.

- `in`  = stream over `httpd_req_recv` (bounded by Content-Length).
- `out` = stream over `httpd_resp_send_chunk` — true streaming.
- Response body IS the handler's reply, no JSON envelope (HTTP status
  and the request itself are the envelope). Content-Type:
  `application/octet-stream`; clients that expect JSON parse it.
- CORS handling as today (headers + OPTIONS preflight on this route).
- POST only. Browser downloads use `fetch` + blob save, not `<a href>`.

### UpdateManager goes pure — phase 2

`HandleUploadApp` / `HandleUploadWww` / `HandleDownloadPartition` and
the `/api/upload/*`, `/api/download` routes are DELETED from
WebServerManager (its only remaining manager dependency: CommandManager
+ ConsoleManager broadcast wiring). The Begin/Write/Finalize state
machine stays; UpdateManager's command set becomes:

| Command             | Dialect | Payload                                  |
| ------------------- | ------- | ---------------------------------------- |
| `updateStatus`      | JSON    | as today                                 |
| `partitions`        | JSON    | as today                                 |
| `updateBegin`       | JSON    | `{"target":"app"\|"www"}` → opens session |
| `updateWrite`       | raw     | image bytes → active session, chunked    |
| `updateEnd`         | JSON    | finalize, report result                  |
| `downloadPartition` | mixed   | JSON request `{"partition":label}`, raw partition bytes out |
| `updateFromUrl`     | JSON    | `{"url":...}` — device pulls the image itself (esp_http_client → Begin/Write/Finalize). Fast path. |

Frontend firmware page: upload switches to `updateBegin` (WS) →
`POST /api/command?type=updateWrite` with the file as body (XHR
progress works as today) → `updateEnd` (WS). Download switches to
`fetch("/api/command?type=downloadPartition", {method:"POST", body:...})`.

## Implementation phasing (separate plans)

1. **Plan 1 — plumbing + migration**: MemoryStream, JsonReader,
   JsonResponse, CommandEntry v2, trampoline, Execute signature,
   WebSocketHandler adaptation, migrate ALL existing handlers
   (System/Settings/Network/Console/Update as-is), `backend.ts`
   payload envelope. Device behaves exactly as before.
2. **Plan 2 — pure UpdateManager**: `/api/command` route, new update
   command set incl. `updateFromUrl`, delete upload/download routes,
   frontend firmware page rework.

Serial transport and the relay server are enabled by this design but
out of scope (see `docs/backlog/remote-access.md`).

## Error handling

- Wrong handler signature in a table → no `CommandOwner` specialization
  → compile error at the table line (as today).
- Registry misuse (re-register, dying entry) → FATAL at boot (as today).
- Unknown command → transport-level error reply (WS: `"error"` field;
  HTTP: 404).
- Handler-level errors → payload fields (`{"ok":false,"error":...}`),
  the handler's own business (as today).
- JsonReader overflow (payload > capacity) → truncation + error log,
  getters fail soft (missing keys) — same policy as BufferStream.

## Testing

No automated tests (repo convention). Verification per plan:
build + flash, then: settings page load/edit/save, wifi scan, console
stream, info/ping, firmware page (status + partitions), and in plan 2
a full app OTA + www update + partition download through the new path,
plus `updateFromUrl` against a local HTTP server.
