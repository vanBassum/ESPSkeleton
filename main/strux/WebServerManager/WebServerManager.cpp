#include "WebServerManager.h"
#include "ConsoleManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "JsonHelpers.h"
#include "SessionTable.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_vfs_fat.h>

static constexpr const char* TAG = "WebServerManager";
static constexpr const char* BASE_PATH = "/www";
static WebServerManager* s_instance_ = nullptr;

WebServerManager::WebServerManager(StruxProvider& strux)
    : strux_(strux)
{
}

void WebServerManager::Init()
{
    auto initAttempt = initState.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    s_instance_ = this;

    wsHandler_.SetCommandManager(strux_.getCommandManager());

    strux_.getSettingsManager().Register({ &webPassword_ });
    auth_.Init();   // snapshot the stored password (after registration)
    wsHandler_.SetAuth(auth_);

    MountFatPartition();
    StartServer();
    RegisterRoutes();

    strux_.getCommandManager().Register(this, commands_);

    // Wire console broadcast to WS clients
    strux_.getConsoleManager().SetBroadcastCallback(
        [](const char* json, int32_t len, void* ctx) {
            static_cast<WebServerManager*>(ctx)->Broadcast(json, len);
        },
        this);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void WebServerManager::MountFatPartition()
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, "www", &mount_config, &wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount FAT partition: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "FAT partition mounted at %s", BASE_PATH);
}

void WebServerManager::StartServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 20;
    config.close_fn = [](httpd_handle_t, int fd) {
        if (s_instance_)
            s_instance_->wsHandler_.OnClientDisconnected(fd);
        close(fd);
    };
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}

void WebServerManager::RegisterRoutes()
{
    if (!server_) return;

    // HTTP serves two things only: the WebSocket upgrade (which carries ALL
    // device interaction — commands, uploads, downloads, auth) and the static
    // app that bootstraps the page. No /api command route, no CORS: every
    // device interaction is a session on the one socket.
    wsHandler_.RegisterRoute(server_);
    staticFileHandler_.RegisterRoute(server_, BASE_PATH);
}

void WebServerManager::Broadcast(const char* json, int len)
{
    if (server_)
        wsHandler_.Broadcast(server_, json, len);

    // ConsoleManager holds a single broadcast callback, so the fan-out to the
    // second transport happens here: relayed frontends get the same live log
    // stream. No-op while the relay is disabled or disconnected.
    strux_.getRelayManager().BroadcastLog(json, len);
}

void WebServerManager::BroadcastBinary(const uint8_t* data, size_t len)
{
    if (server_)
        wsHandler_.BroadcastBinary(server_, data, len);
}

// ──────────────────────────────────────────────────────────────
// Commands
// ──────────────────────────────────────────────────────────────

RequestError WebServerManager::Cmd_GetWebFile(CommandContext& ctx)
{
    // First handler on the pull contract: no envelope handling, no JsonReader, and
    // it will keep working unchanged when the request format stops being JSON.
    char path[192] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("path", path)));

    StaticFileHandler::Resolved file;
    FILE* f = nullptr;

    if (StaticFileHandler::Resolve(BASE_PATH, path, file))
        f = fopen(file.path, "rb");

    // The header is a record; the body is raw bytes after it. The scope must therefore
    // close before the newline that divides them, hence the braces — the reply is not
    // one document, and ctx.out stays reachable alongside ctx.reply for exactly this.
    if (!f)
    {
        // A real 404 — SPA fallback is the asking route layer's decision, not
        // ours (see StaticFileHandler::Resolve).
        {
            auto head = ctx.reply.object();
            head.field("ok", true);
            head.field("status", static_cast<uint32_t>(404));
        }
        ctx.out.write("\n", 1);
        return RequestError::Ok;   // the request was fine; the file simply is not there
    }

    {
        auto head = ctx.reply.object();
        head.field("ok", true);
        head.field("status", static_cast<uint32_t>(200));
        head.field("contentType", file.contentType);
        if (file.gzipped)
            head.field("contentEncoding", "gzip");
    }
    ctx.out.write("\n", 1);

    // Streams out chunk-by-chunk through the session window; a 200 KB bundle
    // never needs a 200 KB buffer here or on the transport.
    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        ctx.out.write(buf, r);

    fclose(f);
    return RequestError::Ok;
}

// ──────────────────────────────────────────────────────────────
// Content digest — what the relay's cache is keyed on
// ──────────────────────────────────────────────────────────────

// FNV-1a, 64-bit. Not a cryptographic claim: this answers "did the served tree
// change", and nobody gains anything by forging it — a device that lies about its own
// content only makes its own UI stale on its own relay.
static constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
static constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

static uint64_t HashEntry(const char* name, uint32_t size)
{
    uint64_t h = FNV_OFFSET;
    for (const char* p = name; *p; ++p)
    {
        h ^= static_cast<unsigned char>(*p);
        h *= FNV_PRIME;
    }
    for (int i = 0; i < 4; ++i)
    {
        h ^= (size >> (i * 8)) & 0xff;
        h *= FNV_PRIME;
    }
    return h;
}

// Per-entry hashes are SUMMED rather than chained, so the result does not depend on
// the order readdir happens to return — which FAT does not promise and which changes
// when files are rewritten in place.
static uint64_t HashTree(const char* dirPath, int depth)
{
    DIR* dir = opendir(dirPath);
    if (!dir) return 0;

    uint64_t acc = 0;
    while (const dirent* entry = readdir(dir))
    {
        // strlcat rather than snprintf: a long-filename entry can be longer than any
        // buffer worth putting on this task's stack, and strlcat reports the length it
        // WOULD have needed, so overflow is detected rather than silently truncated
        // into a stat() of the wrong path.
        char full[192];
        strlcpy(full, dirPath, sizeof(full));
        strlcat(full, "/", sizeof(full));
        if (strlcat(full, entry->d_name, sizeof(full)) >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode))
        {
            // assets/ is one level down; the bound is here so a surprise on flash
            // cannot turn this into an unbounded walk on the relay task's stack.
            if (depth > 0) acc += HashTree(full, depth - 1);
            continue;
        }
        acc += HashEntry(entry->d_name, static_cast<uint32_t>(st.st_size));
    }
    closedir(dir);
    return acc;
}

void WebServerManager::GetContentDigest(char* out, size_t cap) const
{
    const uint64_t h = HashTree(BASE_PATH, 2);
    snprintf(out, cap, "%016llx", static_cast<unsigned long long>(h));
}

// ──────────────────────────────────────────────────────────────
// auth — the handshake as ordinary commands. Nothing here frames its own reply or
// parses its own wire format any more; it is a handler like every other.
// ──────────────────────────────────────────────────────────────

RequestError WebServerManager::Cmd_AuthHello(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    auto resp = ctx.reply.object();
    resp.field("authRequired", auth_.AuthRequired());
    return RequestError::Ok;
}

RequestError WebServerManager::Cmd_AuthLogin(CommandContext& ctx)
{
    char password[64] = {};
    RETURN_IF_ERROR(ctx.readArgs(Optional("password", password)));

    auto resp = ctx.reply.object();

    // A wrong password is MEANING, not form: the request was perfectly well made, the
    // answer is no. So it is a reply, not a refusal.
    if (!auth_.CheckPassword(password))
    {
        resp.field("ok", false);
        return RequestError::Ok;
    }

    char key[SessionTable::TOKEN_LEN] = {};
    auth_.MintKey(key);
    if (ctx.connection) ctx.connection->authenticate(key);

    resp.field("ok", true);
    resp.field("key", key);
    return RequestError::Ok;
}

RequestError WebServerManager::Cmd_AuthResume(CommandContext& ctx)
{
    char key[SessionTable::TOKEN_LEN] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("key", key)));

    auto resp = ctx.reply.object();

    if (!auth_.ValidateKey(key))
    {
        resp.field("ok", false);
        return RequestError::Ok;
    }

    if (ctx.connection) ctx.connection->authenticate(key);
    resp.field("ok", true);
    return RequestError::Ok;
}
