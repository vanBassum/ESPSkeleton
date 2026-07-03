#pragma once

// ══════════════════════════════════════════════════════════════
// MINIMAL EXAMPLE — commands over streams. Not built; plumbing only.
//
// Decided 2026-07-03:
//   - CommandManager is the device's single API surface. Every
//     entrance (WebSocket, HTTP, serial, ...) is a dumb pipe.
//   - ONE handler signature: (Stream& in, Stream& out). Streams are
//     the CONTRACT — the lowest common denominator; anything can be
//     faked with a memory stream (bounded), but a buffer can never
//     become limitless.
//   - JSON is the default DIALECT, not the contract. A handler with
//     structured data constructs the JSON adapters on line one; a
//     bulk handler (firmware bytes) never mentions JSON. Nothing
//     below the handler knows which choice it made. If a command
//     ever wants CBOR/plain text, only that handler changes.
//   - UpdateManager goes PURE: its HTTP routes disappear; upload/
//     download become commands. Fast path = updateFromUrl (device
//     pulls the image itself). Works-anywhere path = begin/write/end.
// ══════════════════════════════════════════════════════════════

#include <type_traits>
#include <cstddef>

class Stream;      // [lib/common/Stream.h]  write/read/available/flush

// ──────────────────────────────────────────────────────────────
// [CommandManager/CommandEntry.h] — entry v2
// ──────────────────────────────────────────────────────────────

struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, Stream& in, Stream& out);   // ← was (json, JsonWriter&)

    void* ctx = nullptr;          // stamped by Register(), as today
    CommandEntry* next = nullptr;
    bool registered = false;
    // ~CommandEntry: FATAL if a registered entry dies (unchanged)
};

// The trampoline barely changes — same owner-deduction trick, new
// argument list. Const members and free functions as before.
template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<void (C::*)(Stream&, Stream&)>       { using type = C; };
template <typename C> struct CommandOwner<void (C::*)(Stream&, Stream&) const> { using type = const C; };

template <auto Handler>
void InvokeCommand(void* ctx, Stream& in, Stream& out)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename CommandOwner<decltype(Handler)>::type;
        (static_cast<C*>(ctx)->*Handler)(in, out);
    }
    else
    {
        Handler(in, out);   // free/static function — ctx unused
    }
}

// ──────────────────────────────────────────────────────────────
// The adapter classes (JSON dialect + stream fakes)
// ──────────────────────────────────────────────────────────────
//
// JsonWriter   [lib/json/JsonWriter.h] — EXISTS, already takes a
//              Stream. Unchanged.
//
// JsonReader   [lib/json/JsonReader.h] — NEW. Buffered, not
//              streaming: consumes `in` into an internal bounded
//              buffer at construction, then serves typed getters
//              (today's JsonHelpers become methods):
//
//                  JsonReader<1024> req(in);       // capacity as template
//                  char url[128];                  // param, sane default
//                  req.GetString("url", url, sizeof(url));
//                  int32_t port = req.GetInt("port", 1883);
//
//              A true streaming parser can replace the internals
//              later — handlers only see the getters.
//
// JsonResponse [lib/json/ or CommandManager/] — NEW, optional sugar.
//              RAII: wraps a JsonWriter, writes beginObject() at
//              construction, endObject() on destruction. Kills the
//              brace boilerplate; a handler that wants full control
//              uses JsonWriter directly.
//
// MemoryStream [lib/common/MemoryStream.h] — NEW. Read-only Stream
//              view over existing bytes (a received WS frame, a
//              serial line buffer). The transport-side fake.
//
// BufferStream [lib/common/BufferStream.h] — EXISTS. Write-side
//              compose-in-RAM adapter for transports that must send
//              whole frames.

// ──────────────────────────────────────────────────────────────
// [UpdateManager] — the two extremes side by side
// ──────────────────────────────────────────────────────────────

class UpdateManagerSketch
{
    // JSON-dialect handler — structured request/response:
    void Cmd_UpdateFromUrl(Stream& in, Stream& out)
    {
        // JsonReader<512> req(in);            // dialect chosen HERE,
        // JsonResponse resp(out);             // not by the contract
        //
        // char url[256];
        // if (!req.GetString("url", url, sizeof(url)))
        //     { resp.field("ok", false); resp.field("error", "no url"); return; }
        //
        // ... esp_http_client GET url → Begin/WriteChunk/Finalize ...
        // resp.field("ok", true);
    }                                          // } written by ~JsonResponse

    // Raw handler — firmware bytes, JSON never mentioned:
    void Cmd_UpdateWrite(Stream& in, Stream& out)
    {
        // char buf[1024];
        // while (size_t n = in.read(buf, sizeof(buf), timeout))
        //     if (!WriteAppChunk(buf, n)) { /* error reply; return */ }
        // out.write("{\"ok\":true}", 11);     // reply format is the
    }                                          // handler's own business

    // downloadPartition mirrors it: tiny JSON request in, megabytes
    // of raw partition bytes streamed out. No response buffer.

    inline static CommandEntry commands_[] = {
        { "updateStatus",      /* &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateStatus> */ },
        { "updateFromUrl",     /* &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateFromUrl> */ },
        { "updateBegin",       /* ... */ },   // {"target":"app"|"www"}
        { "updateWrite",       /* ... */ },   // raw payload
        { "updateEnd",         /* ... */ },
        { "downloadPartition", /* ... */ },   // raw response
        { "partitions",        /* ... */ },
    };
    // HandleUploadApp/HandleUploadWww/HandleDownloadPartition and all
    // /api/upload|download routes in WebServerManager: DELETED. The
    // Begin/Write/Finalize state machine stays exactly as it is.
};

// ──────────────────────────────────────────────────────────────
// The transports become dumb pipes
// ──────────────────────────────────────────────────────────────
//
// Each entrance does two jobs: (1) parse the ENVELOPE — which
// command, where does the reply go; (2) present payload and reply
// as streams. Wire framing is the transport's own business.
//
//   WebSocket   text frame {"type":"ping",...} as today:
//                 in  = MemoryStream(frame bytes)          ← fake, bounded
//                 out = BufferStream(respBuf) → one frame  ← fake, bounded
//               Fine for every structured command. Bulk over WS
//               would need fragmented/binary frames — don't promise
//               it; HTTP and serial cover bulk.
//
//   HTTP        POST /api/command — the ONE route that remains:
//                 in  = stream over httpd_req_recv          ← real
//                 out = stream over httpd_resp_send_chunk   ← real
//               Full-speed upload/download with zero UpdateManager
//               knowledge in WebServerManager.
//
//   Serial      "updateWrite <len>\n" + raw bytes:
//                 in  = length-bounded view over UART       ← real
//                 out = UART                                ← real
//               Full device API on a bench with no network.
//
// CommandManager::Execute(name, in, out): Find under lock, handler
// outside lock — unchanged.

// ──────────────────────────────────────────────────────────────
// Open points (NOT decided by this sketch)
// ──────────────────────────────────────────────────────────────
//
// 1. Envelope composition: transports wrap the handler's reply by
//    concatenation — write `{"id":7,"payload":`, then the handler
//    bytes, then `}`. Requires a JSON-dialect reply to be one
//    complete JSON value (it is: one object). Raw replies need a
//    different envelope rule per transport (e.g. HTTP: body IS the
//    reply; content-type from the envelope request).
//
// 2. How `in` knows its end: length known upfront (bounded view,
//    from Content-Length / frame size / serial <len>) vs read-until-
//    close. Bounded view fits all three transports — prefer it.
//
// 3. JsonReader capacity: template param with a sane default. Lives
//    on the handler's stack — watch task stack sizes (httpd: 8 KB).
//
// 4. Frontend: SettingsPage etc. unchanged (same JSON over WS).
//    Firmware page moves from POST /api/upload/* to updateFromUrl,
//    or chunked updateWrite via POST /api/command.
// ══════════════════════════════════════════════════════════════
