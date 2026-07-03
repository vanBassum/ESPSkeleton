#pragma once

// ══════════════════════════════════════════════════════════════
// STREAM COMMANDS — the whole design as code. Not built.
// This is the readable version of
// docs/superpowers/specs/2026-07-03-stream-commands-design.md.
//
// One handler shape. Streams are the contract, JSON is a dialect
// handlers opt into on line one. Every entrance (WS, HTTP, serial,
// relay) is a dumb pipe into CommandManager.
// ══════════════════════════════════════════════════════════════

#include <type_traits>
#include <cstddef>
#include <cstdint>

class Stream;   // [lib/common/Stream.h] — exists, unchanged

// ──────────────────────────────────────────────────────────────
// 1. The registry — [CommandManager/CommandEntry.h], entry v2
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

// Trampoline — identical trick to today, new argument list.
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
        Handler(in, out);   // free/static, ctx unused
    }
}

// CommandManager::Execute(name, in, out) -> bool (command found).
// Find under lock, handler outside lock — unchanged.

// ──────────────────────────────────────────────────────────────
// 2. New lib pieces
// ──────────────────────────────────────────────────────────────

// [lib/common/MemoryStream.h] — a stream backed by memory. NOT the
// same thing as BufferStream (that one is about chunked I/O
// buffering, e.g. relieving SPI of byte-per-byte reads).
class MemoryStream /* : public Stream */
{
    const char* buf_; size_t len_; size_t pos_ = 0;
public:
    MemoryStream(const void* buf, size_t len);
    // read(): copies from pos_, advances. available(): len_ - pos_.
    // write(): returns 0 (read-only view).
};

// [lib/json/JsonReader.h] — buffered, NOT streaming. Consumes `in`
// into an internal buffer at construction (truncate + log past
// capacity), then typed getters (today's JsonHelpers as methods).
// A streaming parser can replace the internals later; handlers only
// ever see the getters.
template <size_t N = 1024>
class JsonReader
{
    char buf_[N];
public:
    explicit JsonReader(Stream& in);                             // slurps
    bool    GetString(const char* key, char* out, size_t maxLen);
    int32_t GetInt   (const char* key, int32_t def = 0);
    bool    GetBool  (const char* key, bool def = false);
};

// [lib/json/JsonScope.h] — JsonObject / JsonArray RAII scopes.
// Rules:
//   - ctor writes the opener, dtor writes the closer
//   - locals die in reverse order → inner braces close first
//   - EVERY return path yields well-formed JSON (the early-return fix,
//     at every nesting level — not just the outer braces)
//   - writing to a parent while a child is open is LEGAL and means
//     "done with the child": parent auto-closes it (cascading,
//     depth-first) and marks it invalid
//   - opening a second child auto-closes the first (same rule)
//   - writing to an invalidated scope → FATAL (abandoned-scope use
//     is always a bug)
//   - dtor of an invalidated scope: no-op. Copy/assign: deleted.
class JsonObject
{
public:
    explicit JsonObject(Stream& out);                  // root: writes '{'
    ~JsonObject();                                     // writes '}' unless invalidated

    void field(const char* key, const char* v);        // leaves
    void field(const char* key, int32_t v);
    void field(const char* key, uint32_t v);
    void field(const char* key, bool v);

    JsonObject object(const char* key);                // "key":{ ... child scope
    // JsonArray array(const char* key);               // "key":[ ... same rules
};

// ──────────────────────────────────────────────────────────────
// 3. Handlers — what migrated code actually looks like
// ──────────────────────────────────────────────────────────────

class ExampleManager
{
    // Typical JSON command. Note: no beginObject/endObject anywhere,
    // and the early return CANNOT produce broken JSON.
    void Cmd_SetSetting(Stream& in, Stream& out)
    {
        JsonReader<512> req(in);
        JsonObject resp(out);

        char key[48], value[128];
        if (!req.GetString("key", key, sizeof(key)))
        {
            resp.field("ok", false);
            resp.field("error", "missing key");
            return;                                   // resp closes itself
        }
        req.GetString("value", value, sizeof(value));

        // ... apply ...
        resp.field("ok", true);
    }

    // Nesting + auto-close, all in one:
    void Cmd_Info(Stream& in, Stream& out)            // `in` simply ignored
    {
        JsonObject root(out);
        root.field("firmware", "1.2.3");

        JsonObject heap = root.object("heap");        // "heap":{
        heap.field("free", 123456);
        heap.field("min", 98765);

        root.field("uptime", 42);                     // auto-closes heap → },"uptime":42
        // heap.field("late", 1);                     // would be FATAL: scope abandoned
    }                                                 // } via ~root

    // Bulk command — JSON never mentioned. No base64, no image in RAM.
    void Cmd_UpdateWrite(Stream& in, Stream& out)
    {
        char buf[1024];
        // while (size_t n = in.read(buf, sizeof(buf), timeout))
        //     if (!WriteChunk(buf, n)) { out.write("{\"ok\":false}", 12); return; }
        // out.write("{\"ok\":true}", 11);
        (void)buf;
    }

    // Mirror image: tiny JSON request in, megabytes raw out.
    void Cmd_DownloadPartition(Stream& in, Stream& out)
    {
        JsonReader<256> req(in);
        char label[17];
        req.GetString("partition", label, sizeof(label));
        // loop: esp_partition_read → out.write, 4 KB at a time
    }

    inline static CommandEntry commands_[] = {
        { "setSetting",        &InvokeCommand<&ExampleManager::Cmd_SetSetting> },
        { "info",              &InvokeCommand<&ExampleManager::Cmd_Info> },
        { "updateWrite",       &InvokeCommand<&ExampleManager::Cmd_UpdateWrite> },
        { "downloadPartition", &InvokeCommand<&ExampleManager::Cmd_DownloadPartition> },
    };
    // Register(this, commands_) in Init() — unchanged.
};

// ──────────────────────────────────────────────────────────────
// 4. Transports (dumb pipes)
// ──────────────────────────────────────────────────────────────
//
// WebSocket [WebSocketHandler.cpp] — request unchanged:
//   {"id":7,"type":"info", ...params}
// Dispatch:
//   MemoryStream in(frameBuf, frameLen);          // whole frame; envelope
//   BufferStream out(wsBuf_, sizeof(wsBuf_));     // fields are harmless
//   write to socket:  {"id":7,"payload":  +  out bytes  +  }
//   unknown command:  {"id":7,"error":"info"}
// Response envelope is now NESTED (was flat). Frontend change is one
// line in backend.ts: resolve(msg.payload) instead of resolve(msg).
// Pages untouched. Raw/bulk replies not supported over WS — bulk goes
// over HTTP. Broadcasts (console log lines): unchanged.
//
// HTTP [WebServerManager.cpp] — phase 2, ONE route replaces all:
//   POST /api/command?type=updateWrite     (body = payload, streamed)
//   in  = stream over httpd_req_recv       (bounded by Content-Length)
//   out = stream over httpd_resp_send_chunk
//   Response body IS the reply — no JSON envelope; HTTP is the
//   envelope. /api/upload/*, /api/download: DELETED. Firmware page:
//   updateBegin (WS) → POST updateWrite (XHR, progress works) →
//   updateEnd (WS). Downloads via fetch + blob.
//
// Serial (later): "updateWrite <len>\n" + raw bytes → full device API
// on a bench with no network. Relay server (later): outbound pipe,
// same commands, see docs/backlog/remote-access.md.
//
// ──────────────────────────────────────────────────────────────
// 5. What phase 1 delivers vs phase 2
// ──────────────────────────────────────────────────────────────
//
// Phase 1: MemoryStream, JsonReader, JsonObject/JsonArray, entry v2,
//          trampoline, Execute(name, in, out), WS adaptation, migrate
//          ALL existing handlers, backend.ts payload envelope.
//          → device behaves exactly as before.
// Phase 2: /api/command route, UpdateManager pure (updateBegin/Write/
//          End, updateFromUrl, downloadPartition), delete bespoke
//          routes, firmware page rework.
// ══════════════════════════════════════════════════════════════
