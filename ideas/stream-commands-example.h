#pragma once

// ══════════════════════════════════════════════════════════════
// MINIMAL EXAMPLE — commands over streams instead of JSON.
// Not built; sketches the plumbing only.
//
// Idea (2026-07-03): CommandManager is the device's single API
// surface. Every entrance (WebSocket, HTTP, serial, ...) is a dumb
// pipe. The registry signature drops its JSON coupling:
//
//     void handler(void* ctx, Stream& in,  Stream& out)
//                             ~~~~~~~~~~   ~~~~~~~~~~~
//                             request      response
//                             payload      payload
//
// Stream is the lowest common denominator: anything can be faked
// with a memory stream (at the price of a size cap), but a buffer
// can never become limitless. JSON stays available as a CONVENIENCE
// — adapted in ONE place (the trampoline), not baked into every
// handler's contract.
// ══════════════════════════════════════════════════════════════

#include <type_traits>
#include <cstddef>

class Stream;      // [lib/common/Stream.h]  write/read/available/flush
class JsonWriter;  // [lib/json/JsonWriter.h] already writes to a Stream

// ──────────────────────────────────────────────────────────────
// [lib/common/MemoryStream.h] — NEW small lib piece.
// The "fake it" adapter: wraps an existing byte range so that
// non-stream sources (a received WS frame, a serial line buffer)
// can be handed to a stream-consuming handler. Bounded, obviously.
// ──────────────────────────────────────────────────────────────
//
//  class MemoryStream : public Stream
//  {
//      const char* buf_; size_t len_; size_t pos_ = 0;
//  public:
//      MemoryStream(const void* buf, size_t len);
//      size_t read(void* dst, size_t n, TickType_t) override;  // copies, advances pos_
//      size_t available() const override { return len_ - pos_; }
//      size_t write(...) override { return 0; }                // read-only
//  };
//
// (BufferStream already covers the other direction: a handler that
// wants to COMPOSE a bounded response in RAM writes into one, then
// the transport ships buf/len however it likes.)

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

// ──────────────────────────────────────────────────────────────
// The dual-shape trampoline. Handlers come in TWO flavors, and the
// trampoline — not the handler, not the transport — adapts:
//
//   json-style   void Cmd_Foo(const char* json, JsonWriter& resp)
//                → trampoline slurps `in` to a stack buffer and
//                  wraps `out` in a JsonWriter. Every existing
//                  handler keeps its exact current body.
//
//   stream-style void Cmd_Bar(Stream& in, Stream& out)
//                → passed through untouched. For bulk transfer:
//                  read chunks straight to flash, write megabytes
//                  out with no response buffer.
//
// Wrong signature → HandlerTraits has no specialization → compile
// error at the table line, same guarantee as today.
// ──────────────────────────────────────────────────────────────

template <typename T> struct HandlerTraits;   // no primary definition on purpose

// member, json-style
template <typename C> struct HandlerTraits<void (C::*)(const char*, JsonWriter&)>       { using Owner = C;       static constexpr bool json = true;  };
template <typename C> struct HandlerTraits<void (C::*)(const char*, JsonWriter&) const> { using Owner = const C; static constexpr bool json = true;  };
// member, stream-style
template <typename C> struct HandlerTraits<void (C::*)(Stream&, Stream&)>               { using Owner = C;       static constexpr bool json = false; };
template <typename C> struct HandlerTraits<void (C::*)(Stream&, Stream&) const>         { using Owner = const C; static constexpr bool json = false; };
// free/static, both styles (register with ctx = nullptr)
template <> struct HandlerTraits<void (*)(const char*, JsonWriter&)>                    { using Owner = void;    static constexpr bool json = true;  };
template <> struct HandlerTraits<void (*)(Stream&, Stream&)>                            { using Owner = void;    static constexpr bool json = false; };

inline constexpr size_t COMMAND_JSON_MAX = 2048;   // slurp cap for json-style handlers

template <auto Handler>
void InvokeCommand(void* ctx, Stream& in, Stream& out)
{
    using T = HandlerTraits<decltype(Handler)>;

    auto call = [&](auto&&... args)
    {
        if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
            (static_cast<typename T::Owner*>(ctx)->*Handler)(args...);
        else
            Handler(args...);
    };

    if constexpr (T::json)
    {
        // The adapter, in one place: drain the (bounded) request into
        // a stack buffer, hand the response stream to a JsonWriter.
        char json[COMMAND_JSON_MAX];
        size_t n = 0; // = in.read(json, sizeof(json) - 1);
        json[n] = '\0';

        // JsonWriter resp(out);   // JsonWriter already takes a Stream
        // resp.beginObject();  call(json, resp);  resp.endObject();
        (void)json;
    }
    else
    {
        call(in, out);
    }
}

// ──────────────────────────────────────────────────────────────
// [UpdateManager] — the stress test. Pure: no HTTP routes left.
// ──────────────────────────────────────────────────────────────

class UpdateManagerSketch
{
    // json-style — small request/response, exactly like today:
    void Cmd_UpdateStatus (const char* json, JsonWriter& resp);
    void Cmd_UpdateFromUrl(const char* json, JsonWriter& resp);  // {"url": ...} → device PULLS the
                                                                 // image itself (esp_http_client →
                                                                 // Begin/Write/Finalize). Fast path.
    void Cmd_UpdateBegin  (const char* json, JsonWriter& resp);  // {"target":"app"|"www"}
    void Cmd_UpdateEnd    (const char* json, JsonWriter& resp);

    // stream-style — bulk, works over ANY transport, no base64:
    void Cmd_UpdateWrite(Stream& in, Stream& out)
    {
        // char buf[1024];
        // while (size_t n = in.read(buf, sizeof(buf), timeout))
        //     if (!WriteAppChunk(buf, n)) { /* write error to out; return */ }
        // out.write("{\"ok\":true}", ...);   // response format is the
        //                                    // handler's own business here
    }

    void Cmd_DownloadPartition(Stream& in, Stream& out)
    {
        // slurp `in` (tiny JSON: {"partition":"ota_0"}), then stream
        // the partition out chunk by chunk — megabytes, no buffer.
    }

    inline static CommandEntry commands_[] = {
        { "updateStatus",      &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateStatus> },
        { "updateFromUrl",     &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateFromUrl> },
        { "updateBegin",       &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateBegin> },
        { "updateWrite",       &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateWrite> },   // ← stream
        { "updateEnd",         &InvokeCommand<&UpdateManagerSketch::Cmd_UpdateEnd> },
        { "downloadPartition", &InvokeCommand<&UpdateManagerSketch::Cmd_DownloadPartition> }, // ← stream
    };
};

// ──────────────────────────────────────────────────────────────
// The transports become dumb pipes
// ──────────────────────────────────────────────────────────────
//
// Each entrance does exactly two jobs: (1) parse the ENVELOPE —
// which command, where does the reply go; (2) present payload and
// reply as streams. How it frames that on the wire is its own
// business; handlers never know.
//
//   WebSocket   text frame {"type":"ping",...} as today:
//                 in  = MemoryStream(frame bytes)       ← faked, bounded
//                 out = BufferStream(respBuf) → send 1 frame ← faked, bounded
//               (bulk over WS needs fragmented/binary frames — check
//               what esp_http_server allows before promising this)
//
//   HTTP        POST /api/command — the ONE route that remains:
//                 in  = stream over httpd_req_recv       ← real stream
//                 out = stream over httpd_resp_send_chunk ← real stream
//               true streaming both ways; uploads/downloads at full
//               speed with zero UpdateManager knowledge in the server.
//
//   Serial      "updateWrite <len>\n" + raw bytes:
//                 in  = bounded view over the UART stream ← real stream
//                 out = UART                              ← real stream
//               full device API on a bench with no network.
//
// CommandManager::Execute(name, in, out): Find under lock, handler
// outside lock — unchanged.
//
// ──────────────────────────────────────────────────────────────
// Open points (NOT decided by this sketch)
// ──────────────────────────────────────────────────────────────
//
// 1. Envelope composition: today WebSocketHandler shares one
//    JsonWriter between envelope fields and handler fields. With
//    opaque payloads the transport instead wraps by concatenation:
//    write `{"id":7,"payload":` … handler bytes … `}`. Requires
//    json-style responses to be one complete JSON value — they are
//    (the trampoline writes exactly one object).
//
// 2. COMMAND_JSON_MAX (slurp cap for json-style handlers): 2 KB
//    matches today's message sizes; bulk commands bypass it by
//    being stream-style.
//
// 3. How `in` signals end-of-payload: length known upfront from the
//    envelope/Content-Length (bounded stream view) vs read-until-
//    close. Bounded view is simpler and fits all three transports.
//
// 4. Frontend impact: SettingsPage etc. unchanged (same JSON over
//    WS). Firmware page switches from POST /api/upload/* to either
//    updateFromUrl or chunked updateWrite over /api/command.
// ══════════════════════════════════════════════════════════════
