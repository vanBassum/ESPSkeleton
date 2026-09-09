#include "UiManager.h"
#include "CommandManager.h"
#include "ContextLock.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include <cassert>
#include <cstring>

UiManager::UiManager(StruxProvider& strux)
    : strux_(strux)
{
}

void UiManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getCommandManager().Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void UiManager::Register(std::initializer_list<UiModule*> modules)
{
    LOCK(mutex_);
    for (UiModule* m : modules)
    {
        // Chain-corruption class → FATAL, the same call as SettingsManager makes:
        // re-linking a registered entry cycles the chain and hangs every walk.
        if (m->registered)
            FATAL("ui module '%s' registered twice", m->id);
        for (const UiModule* existing = head_; existing; existing = existing->next)
            if (strcmp(existing->id, m->id) == 0)
                FATAL("duplicate ui module id '%s'", m->id);

        // Sloppiness class → assert. Every string is held for the life of the device
        // and written straight into a reply, so it must live in flash and never dangle.
        assert(esp_ptr_in_drom(m->id) && "ui module id must be a string literal");
        assert(esp_ptr_in_drom(m->entry) && "ui module entry must be a string literal");

        // A shell joins this onto the device's base — its own origin locally,
        // /devices/<id>/ through the relay. A relative entry would resolve against
        // whichever shell happened to load it, which on the relay means its own
        // wwwroot: the module would 404 and the device would look broken.
        assert(m->entry[0] == '/' && "ui module entry must be an absolute www path");

        assert((m->pageCount > 0 || m->cardCount > 0)
               && "a ui module that contributes nothing has nothing to load for");

        for (size_t i = 0; i < m->pageCount; ++i)
        {
            assert(esp_ptr_in_drom(m->pages[i].id) && "page id must be a string literal");
            assert(esp_ptr_in_drom(m->pages[i].title) && "page title must be a string literal");
            assert(esp_ptr_in_drom(m->pages[i].icon) && "page icon must be a string literal");
        }
        for (size_t i = 0; i < m->cardCount; ++i)
            assert(esp_ptr_in_drom(m->cards[i]) && "card id must be a string literal");

        m->registered = true;
        m->next = head_;
        head_ = m;

        ESP_LOGI(TAG, "Registered module '%s' (%u page(s), %u card(s)) at %s",
                 m->id, (unsigned)m->pageCount, (unsigned)m->cardCount, m->entry);
    }
}

// ──────────────────────────────────────────────────────────────
// `ui modules` — the device describing its own UI.
//
// A shell reads this before it draws anything, so an empty `modules` array is the
// ordinary answer for a product that ships none, and the template's default. A device
// old enough not to know the command rejects it, which a shell reads the same way: no
// modules, fall back to serving the device's whole page.
// ──────────────────────────────────────────────────────────────

RequestError UiManager::Cmd_Modules(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    UiModule* head;
    {
        LOCK(mutex_);
        head = head_;
    }
    // The lock covered reading the head and nothing more: the links behind it are
    // write-once and the entries immortal, so the walk itself needs none — the same
    // reasoning as SettingsManager::begin().

    auto resp = ctx.reply.object();

    {
        auto api = resp.object("hostApi");
        api.field("min", HOST_API_MIN);
        api.field("max", HOST_API_MAX);
    }

    auto modules = resp.array("modules");
    for (const UiModule* m = head; m; m = m->next)
    {
        auto mod = modules.object();
        mod.field("id", m->id);
        mod.field("entry", m->entry);

        auto pages = mod.array("pages");
        for (size_t i = 0; i < m->pageCount; ++i)
        {
            auto page = pages.object();
            page.field("id", m->pages[i].id);
            page.field("title", m->pages[i].title);
            page.field("icon", m->pages[i].icon);
        }

        // Writing to `mod` closes `pages` — see ReplyWriter's scope rules.
        auto cards = mod.array("cards");
        for (size_t i = 0; i < m->cardCount; ++i)
            cards.value(m->cards[i]);
    }

    return RequestError::Ok;
}
