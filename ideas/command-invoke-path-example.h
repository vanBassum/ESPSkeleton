#pragma once

// ══════════════════════════════════════════════════════════════
// MINIMAL EXAMPLE — the life of one command, as implemented on the
// modular-managers branch. Not built; every piece here is a
// simplified copy of real code (real locations in [brackets]).
//
// The cast of characters:
//   CommandEntry     — one command; also the chain link
//   InvokeCommand<>  — compile-time-generated trampoline
//   CommandManager   — pure dispatcher (knows no commands)
//   FooManager       — owns one command, "fooStatus"
// ══════════════════════════════════════════════════════════════

class JsonWriter;

// ──────────────────────────────────────────────────────────────
// [CommandManager/CommandEntry.h]
// ──────────────────────────────────────────────────────────────

struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, const char* json, JsonWriter& resp);  // plain fn ptr

    void* ctx = nullptr;          // stamped by Register(): the owner's `this`
    CommandEntry* next = nullptr; // stamped by Register(): the chain link
    bool registered = false;
    // ~CommandEntry aborts if a registered entry dies (omitted here)
};

// The trampoline GENERATOR. For every distinct Method it is used with,
// the compiler emits one concrete function with the fn-ptr signature
// above. The owning class C is deduced FROM the method pointer itself,
// so the cast below can never target the wrong type.
template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<void (C::*)(const char*, JsonWriter&)> { using type = C; };

template <auto Method>
void InvokeCommand(void* ctx, const char* json, JsonWriter& resp)
{
    using C = typename CommandOwner<decltype(Method)>::type;
    (static_cast<C*>(ctx)->*Method)(json, resp);
}

// ──────────────────────────────────────────────────────────────
// [Any manager] — the owner side, everything a command costs you:
// ──────────────────────────────────────────────────────────────

class FooManager
{
public:
    void Init(/* ServiceProvider& sp */)
    {
        // sp.getCommandManager().Register(this, commands_);      // (boot, step B)
    }

private:
    // An ordinary private member. No ctx, no static, `this` just works.
    void Cmd_FooStatus(const char* json, JsonWriter& resp)
    {
        resp.field("fooCount", fooCount_);
    }

    int fooCount_ = 42;

    // &InvokeCommand<&FooManager::Cmd_FooStatus> forces the compiler to
    // instantiate the trampoline for this exact method (compile, step A)
    // and stores its address as a plain function pointer.
    inline static CommandEntry commands_[] = {
        { "fooStatus", &InvokeCommand<&FooManager::Cmd_FooStatus> },
    };
};

// ──────────────────────────────────────────────────────────────
// The complete path of {"type":"fooStatus"}, in order
// ──────────────────────────────────────────────────────────────
//
// A) COMPILE TIME
//    InvokeCommand<&FooManager::Cmd_FooStatus> is instantiated. The
//    compiler emits, in effect:
//
//        void trampoline_FooStatus(void* ctx, const char* json, JsonWriter& resp)
//        {
//            static_cast<FooManager*>(ctx)->Cmd_FooStatus(json, resp);
//        }
//
//    commands_[0] = { "fooStatus", &trampoline_FooStatus } sits in the
//    binary. There is no runtime registration of the FUNCTION — only
//    of the instance, which is the next step.
//
// B) BOOT — FooManager::Init()
//    Register(this, commands_) walks the array once, under the lock:
//        commands_[0].ctx  = this;         // the missing instance
//        commands_[0].next = head_;        // linked into the chain
//        head_ = &commands_[0];
//    From here on the entry is immortal and immutable.
//
// C) RUNTIME — a WebSocket frame arrives
//    WebSocketHandler parses {"type":"fooStatus", ...} and calls
//    [WebSocketHandler.cpp] commandManager.Execute("fooStatus", json, resp);
//
//    Execute() [CommandManager.cpp]:
//      1. Find("fooStatus")   — takes the mutex, walks head_->next->...
//                               comparing names, returns the entry,
//                               releases the mutex.
//      2. e->handler(e->ctx, json, resp);
//                             — OUTSIDE the lock (entries are immortal,
//                               so the pointer can't go stale; and a
//                               handler may itself Register/Execute
//                               without deadlock).
//
//    That call lands in the compiler-generated trampoline from (A),
//    which casts ctx back to FooManager* — the exact type it was
//    deduced from — and calls the ordinary member:
//
//        Cmd_FooStatus(json, resp)   // `this` = the ctx from (B)
//
//    The handler writes fields into resp; WebSocketHandler wraps and
//    sends the reply. Done.
//
// What it costs: one fn-pointer-sized trampoline per command in flash,
// ~24 bytes RAM per entry, one strcmp walk per dispatch. No heap, no
// std::function, no vtable, no RTTI.
//
// Where the void* still lives: ONLY inside InvokeCommand (the type
// erasure that lets one chain hold commands of many classes) and the
// CommandEntry it flows through. No handler or manager code sees it.
