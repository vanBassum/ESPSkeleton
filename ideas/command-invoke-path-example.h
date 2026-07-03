#pragma once

// ══════════════════════════════════════════════════════════════
// MINIMAL EXAMPLE — the life of one command, as implemented on the
// modular-managers branch. Not built; every piece here is a
// simplified copy of real code (real locations in [brackets]).
//
// Includes the DEAD END section at the bottom: storing member
// function pointers directly (tried 2026-07-03) and why it cannot
// work — which is the reason the trampoline exists.
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
        // resp.field("fooCount", fooCount_);   // (JsonWriter is only
        // forward-declared in this sketch, so the call is commented)
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
//    binary. The METHOD is baked into code; it costs no runtime bytes.
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
//    deduced from — and calls the ordinary member with `this` = the
//    ctx from (B). The handler writes fields into resp. Done.
//
// Costs: one tiny trampoline per command in flash, ~24 bytes RAM per
// entry, one strcmp walk per dispatch. No heap, no std::function, no
// vtable, no RTTI. The void* exists ONLY inside InvokeCommand and the
// entry it flows through — no handler or manager code sees it.

// ──────────────────────────────────────────────────────────────
// DEAD END — storing the member pointer directly (tried 2026-07-03)
// ──────────────────────────────────────────────────────────────
//
//  struct CommandEntry
//  {
//      void* handlerPtr;      // ← store either kind of pointer here?
//      void* ctx;
//      bool isFunction;       // ← branch on it at dispatch?
//
//      template <typename T>
//      CommandEntry(const char* n, void (T::*mp)(const char*, JsonWriter&))
//      {
//          handlerPtr = mp;   // ✗ DOES NOT COMPILE
//      }
//  };
//
// Why it can't work:
//
// 1. A pointer-to-member-function IS NOT A POINTER. On GCC/Xtensa
//    (Itanium C++ ABI) it is a two-word struct:
//        { fn-address-or-vtable-offset, this-adjustment }
//    → 8 bytes on this 32-bit target vs void*'s 4. It literally does
//    not fit, and C++ defines no conversion to void* (reinterpret_cast
//    included) precisely because the value can't survive the trip.
//
// 2. Even stored in a big-enough buffer, Invoke() can't call it: the
//    cast back needs the exact type void (T::*)(...), and T died with
//    the constructor. Without RTTI, the only vehicle that can carry
//    "how to invoke on T" from compile time to dispatch time is a
//    FUNCTION whose code embodies the cast — i.e. the trampoline. It
//    isn't a style choice; it's the only mechanism the language has.
//
// 3. The legal variant (memcpy the member ptr into an aligned buffer;
//    a per-class trampoline memcpy's it back out — std::function's
//    trick, minus heap) works but is strictly worse than
//    InvokeCommand<auto>: +8 bytes RAM per entry, a memcpy per
//    dispatch, and it still instantiates a trampoline anyway. Baking
//    the method in as a template argument makes it cost zero runtime
//    bytes. (A constructor can't do the baking: template<auto> needs a
//    compile-time constant, and constructor arguments are runtime
//    values.)
//
// 4. The isFunction flag dissolves under trampolines: free/static
//    handlers either take the uniform (void* ctx, ...) signature and
//    ignore ctx, or get their own InvokeCommand overload — either way
//    the dispatch stays a single unconditional call.
