#pragma once

#include <cstddef>

class UiManager;

// ──────────────────────────────────────────────────────────────
// One page a module contributes to a shell's navigation.
//
// `id` is the identity the module's own `activate()` must register under — the
// declaration here and the registration there are matched by it, so a shell can draw a
// sidebar without loading a single byte of module code. That is the whole reason the
// pages are declared rather than discovered.
//
// `icon` is a name, not an image: a manifest crosses a wire, so it can say "lightbulb"
// but cannot hand over a component. Shells resolve names and fall back when they do not
// know one — two shells may carry different icon-set versions.
// ──────────────────────────────────────────────────────────────
struct UiPage
{
    const char* id;
    const char* title;
    const char* icon;
};

// ──────────────────────────────────────────────────────────────
// A UI module this firmware ships: one self-contained ES module in `www`, plus the
// contributions it advertises.
//
// Declared as an `inline static` member by the manager that owns the feature and handed
// to UiManager::Register() from its Init() — the same shape as a Setting or a command
// table, and with the same requirement: static storage duration, because the registry
// keeps the pointer for the life of the device.
//
// `entry` is a path inside the device's `www`, in the same vocabulary `web read` uses
// (`/index.html`), and it is STABLE rather than content-hashed. Firmware names its own
// bundle, which it could not do if the frontend build were free to rename it. A shell
// resolves this against the device's base — its own origin locally, `/devices/<id>/`
// through the relay — and never imports it raw.
// ──────────────────────────────────────────────────────────────
class UiModule
{
public:
    template <size_t P, size_t C>
    UiModule(const char* id, const char* entry,
             const UiPage (&pages)[P], const char* const (&cards)[C])
        : id(id), entry(entry),
          pages(pages), pageCount(P),
          cards(cards), cardCount(C) {}

    template <size_t P>
    UiModule(const char* id, const char* entry, const UiPage (&pages)[P])
        : id(id), entry(entry), pages(pages), pageCount(P) {}

    const char*        id;
    const char*        entry;
    const UiPage*      pages     = nullptr;
    size_t             pageCount = 0;

    /// Dashboard-card contributions, by id. A card has nothing to declare but its
    /// identity — no title, no icon — because the card draws its own heading.
    const char* const* cards     = nullptr;
    size_t             cardCount = 0;

private:
    friend class UiManager;
    UiModule* next = nullptr;
    bool      registered = false;
};
