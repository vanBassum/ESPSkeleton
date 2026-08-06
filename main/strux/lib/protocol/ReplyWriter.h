#pragma once

#include "Fatal.h"
#include <cstdint>

// The reply half of the handler's contract, mirroring ArgReader on the request side:
// a handler names the SHAPE of what it is writing and never names the wire format.
//
//     RequestError Cmd_Info(CommandContext& ctx)
//     {
//         RETURN_IF_ERROR(ctx.readArgs());
//
//         auto resp = ctx.reply.object();
//         resp.field("name", deviceName);
//         resp.field("heapFree", esp_get_free_heap_size());
//     }
//
// Every scope comes from a factory method — the root from the writer, children from
// their parent — so a call site never spells a type. `auto` is enough, and the methods
// available at any point are exactly the ones the enclosing scope offers.
//
// Depth is not capped. A record may open another object or array, the same way today's
// JsonScope allows, because the alternative (a flat record, or a record plus one level
// of list) cannot express `help list` without changing its wire shape. The cost is that
// a second writer implementation owes the whole grammar; there is only one today.
//
// This is NOT a builder. Bytes reach the transport on every field, nothing is buffered
// and nothing can be revised. That is also why a scope is worth opening and closing
// rather than living for the handler's lifetime: `updateWrite` emits a progress record
// every few kilobytes, and `getWebFile` writes a header record and then streams raw file
// bytes to CommandContext::out. Several roots in sequence, and raw output alongside
// them, are both ordinary.

class ReplyObject;
class ReplyArray;
class ReplyScope;

/// The format half: syntax primitives, one implementation per wire format. Handlers see
/// only the two factory methods; everything else is reached through a scope.
class ReplyWriter
{
public:
    virtual ~ReplyWriter() = default;

    ReplyWriter() = default;
    ReplyWriter(const ReplyWriter&) = delete;
    ReplyWriter& operator=(const ReplyWriter&) = delete;

    /// Open the reply's root. Callable more than once per handler — a second call starts
    /// a second document, which is what a progress report is.
    inline ReplyObject object();
    inline ReplyArray  array();

protected:
    friend class ReplyScope;
    friend class ReplyObject;
    friend class ReplyArray;

    virtual void beginObject() = 0;
    virtual void endObject()   = 0;
    virtual void beginArray()  = 0;
    virtual void endArray()    = 0;

    /// Names the next value. Only ever called inside an object, always immediately
    /// before the value it names.
    virtual void key(const char* name) = 0;

    /// Divides one member of a scope from the next. Never called before the first.
    virtual void separator() = 0;

    virtual void value(const char* v) = 0;
    virtual void value(int32_t v)     = 0;
    virtual void value(uint32_t v)    = 0;
    virtual void value(float v)       = 0;
    virtual void value(bool v)        = 0;
};

/// Lifetime and ordering bookkeeping, shared by both scope shapes and free of any
/// format knowledge: a scope opens on construction, closes on destruction, and locals
/// die in reverse declaration order — so every return path yields a complete reply.
///
/// Rules, unchanged from JsonScope:
///   - writing to a parent while a child is open means "done with the child": the
///     parent closes it, cascading
///   - opening a second child closes the first
///   - writing to a closed scope is FATAL — an abandoned scope in use is a bug
///   - destroying a closed scope does nothing
class ReplyScope
{
protected:
    ReplyWriter* w_;                    // nullptr = closed
    ReplyScope*  parent_;
    ReplyScope*  openChild_ = nullptr;
    bool         needsSeparator_ = false;
    const bool   isArray_;

    ReplyScope(ReplyWriter& w, ReplyScope* parent, bool isArray)
        : w_(&w), parent_(parent), isArray_(isArray)
    {
        if (parent_)
            parent_->openChild_ = this;
        if (isArray_) w_->beginArray(); else w_->beginObject();
    }

    void Prepare()
    {
        if (!w_)
            FATAL("reply scope used after close");
        if (openChild_)
            openChild_->Close();
        if (needsSeparator_)
            w_->separator();
        needsSeparator_ = true;
    }

    void Close()
    {
        if (!w_) return;
        if (openChild_)
            openChild_->Close();        // cascades depth-first
        if (isArray_) w_->endArray(); else w_->endObject();
        if (parent_)
            parent_->openChild_ = nullptr;
        w_ = nullptr;
    }

public:
    ~ReplyScope() { Close(); }

    ReplyScope(const ReplyScope&) = delete;
    ReplyScope& operator=(const ReplyScope&) = delete;
};

/// Named members. Method names match JsonObject deliberately, so converting a handler
/// is a change of type and nothing else.
class ReplyObject : public ReplyScope
{
    friend class ReplyArray;
    friend class ReplyWriter;

    ReplyObject(ReplyWriter& w, ReplyScope* parent) : ReplyScope(w, parent, false) {}

    void Key(const char* key) { Prepare(); w_->key(key); }

public:
    void field(const char* key, const char* v) { Key(key); w_->value(v); }
    void field(const char* key, int32_t v)     { Key(key); w_->value(v); }
    void field(const char* key, uint32_t v)    { Key(key); w_->value(v); }
    void field(const char* key, float v)       { Key(key); w_->value(v); }
    void field(const char* key, bool v)        { Key(key); w_->value(v); }

    inline ReplyObject object(const char* key);
    inline ReplyArray  array(const char* key);
};

/// Positional members.
class ReplyArray : public ReplyScope
{
    friend class ReplyObject;
    friend class ReplyWriter;

    ReplyArray(ReplyWriter& w, ReplyScope* parent) : ReplyScope(w, parent, true) {}

public:
    void value(const char* v) { Prepare(); w_->value(v); }
    void value(int32_t v)     { Prepare(); w_->value(v); }
    void value(uint32_t v)    { Prepare(); w_->value(v); }
    void value(float v)       { Prepare(); w_->value(v); }
    void value(bool v)        { Prepare(); w_->value(v); }

    ReplyObject object() { Prepare(); return ReplyObject(*w_, this); }   // elided
    ReplyArray  array()  { Prepare(); return ReplyArray(*w_, this); }
};

inline ReplyObject ReplyObject::object(const char* key) { Key(key); return ReplyObject(*w_, this); }
inline ReplyArray  ReplyObject::array(const char* key)  { Key(key); return ReplyArray(*w_, this); }

inline ReplyObject ReplyWriter::object() { return ReplyObject(*this, nullptr); }
inline ReplyArray  ReplyWriter::array()  { return ReplyArray(*this, nullptr); }
