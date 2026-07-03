#pragma once

#include "Stream.h"
#include "Fatal.h"
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstring>

// ──────────────────────────────────────────────────────────────
// RAII JSON scope writers. A scope writes its opener at construction
// and its closer at destruction; locals die in reverse declaration
// order, so inner braces close first and EVERY return path yields
// well-formed JSON.
//
// Rules:
//   - writing to a parent while a child is open is LEGAL and means
//     "done with the child": the parent auto-closes it (cascading)
//   - opening a second child auto-closes the first
//   - writing to a closed scope → FATAL (abandoned-scope use is a bug)
//   - destructor of a closed scope: no-op
//
// JsonWriter remains for legacy callers (MQTT discovery); new code
// uses these scopes.
// ──────────────────────────────────────────────────────────────

class JsonScope
{
protected:
    Stream* out_;                 // nullptr = closed/invalidated
    JsonScope* parent_;
    JsonScope* openChild_ = nullptr;
    bool needsComma_ = false;
    const char closer_;

    JsonScope(Stream& out, JsonScope* parent, char opener, char closer)
        : out_(&out), parent_(parent), closer_(closer)
    {
        out_->write(&opener, 1);
        if (parent_)
            parent_->openChild_ = this;
    }

    // FATAL on closed scope; auto-close open child; comma bookkeeping.
    void Prepare()
    {
        if (!out_)
            FATAL("JSON scope used after close");
        if (openChild_)
            openChild_->Close();
        if (needsComma_)
            out_->write(",", 1);
        needsComma_ = true;
    }

    void Close()
    {
        if (!out_) return;
        if (openChild_)
            openChild_->Close();          // cascades depth-first
        out_->write(&closer_, 1);
        if (parent_)
            parent_->openChild_ = nullptr;
        out_ = nullptr;
    }

    void WriteEscaped(const char* s)
    {
        out_->write("\"", 1);
        for (const char* p = s; *p; p++)
        {
            switch (*p)
            {
            case '"':  out_->write("\\\"", 2); break;
            case '\\': out_->write("\\\\", 2); break;
            case '\n': out_->write("\\n", 2); break;
            case '\r': out_->write("\\r", 2); break;
            case '\t': out_->write("\\t", 2); break;
            default:
                if (static_cast<uint8_t>(*p) >= 0x20)
                    out_->write(p, 1);
                break;
            }
        }
        out_->write("\"", 1);
    }

    void WriteRaw(const char* s) { out_->write(s, strlen(s)); }

public:
    ~JsonScope() { Close(); }

    JsonScope(const JsonScope&) = delete;
    JsonScope& operator=(const JsonScope&) = delete;
};

class JsonArray;

class JsonObject : public JsonScope
{
    friend class JsonArray;

    JsonObject(Stream& out, JsonScope* parent) : JsonScope(out, parent, '{', '}') {}

    void WriteKey(const char* key)
    {
        Prepare();
        WriteEscaped(key);
        out_->write(":", 1);
    }

public:
    explicit JsonObject(Stream& out) : JsonScope(out, nullptr, '{', '}') {}

    void field(const char* key, const char* v) { WriteKey(key); WriteEscaped(v); }

    void field(const char* key, int32_t v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        WriteRaw(buf);
    }

    void field(const char* key, uint32_t v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, v);
        WriteRaw(buf);
    }

    void field(const char* key, float v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        WriteRaw(buf);
    }

    void field(const char* key, bool v) { WriteKey(key); WriteRaw(v ? "true" : "false"); }

    inline JsonObject object(const char* key);
    inline JsonArray array(const char* key);
};

class JsonArray : public JsonScope
{
    friend class JsonObject;

    JsonArray(Stream& out, JsonScope* parent) : JsonScope(out, parent, '[', ']') {}

public:
    void value(const char* v) { Prepare(); WriteEscaped(v); }

    void value(int32_t v)
    {
        Prepare();
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        WriteRaw(buf);
    }

    void value(bool v) { Prepare(); WriteRaw(v ? "true" : "false"); }

    JsonObject object()
    {
        Prepare();
        return JsonObject(*out_, this);   // guaranteed copy elision
    }
};

inline JsonObject JsonObject::object(const char* key)
{
    WriteKey(key);
    return JsonObject(*out_, this);
}

inline JsonArray JsonObject::array(const char* key)
{
    WriteKey(key);
    return JsonArray(*out_, this);
}
