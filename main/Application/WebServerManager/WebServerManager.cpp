#include "WebServerManager.h"
#include "ConsoleManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "StringReader.h"
#include "JsonHelpers.h"

#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_vfs_fat.h>

static constexpr const char* TAG = "WebServerManager";
static constexpr const char* BASE_PATH = "/www";
static WebServerManager* s_instance_ = nullptr;

WebServerManager::WebServerManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
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

    wsHandler_.SetCommandManager(serviceProvider_.getCommandManager());

    auth_.Register(serviceProvider_.getSettingsManager());
    wsHandler_.SetAuth(auth_);

    MountFatPartition();
    StartServer();
    RegisterRoutes();

    serviceProvider_.getCommandManager().Register(this, commands_);

    // Wire console broadcast to WS clients
    serviceProvider_.getConsoleManager().SetBroadcastCallback(
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
    serviceProvider_.getRelayManager().BroadcastLog(json, len);
}

void WebServerManager::BroadcastBinary(const uint8_t* data, size_t len)
{
    if (server_)
        wsHandler_.BroadcastBinary(server_, data, len);
}

// ──────────────────────────────────────────────────────────────
// Commands
// ──────────────────────────────────────────────────────────────

void WebServerManager::Cmd_GetWebFile(Stream& in, Stream& out)
{
    char line[256];
    StringReader(in).readLine(line, sizeof(line));

    char path[192] = {};
    ExtractJsonString(line, "path", path, sizeof(path));

    StaticFileHandler::Resolved file;
    FILE* f = nullptr;

    if (path[0] != '\0' && StaticFileHandler::Resolve(BASE_PATH, path, file))
        f = fopen(file.path, "rb");

    if (!f)
    {
        // A real 404 — SPA fallback is the asking route layer's decision, not
        // ours (see StaticFileHandler::Resolve).
        static constexpr const char* notFound = "{\"ok\":true,\"status\":404}\n";
        out.write(notFound, strlen(notFound));
        return;
    }

    char header[256];
    int n = snprintf(header, sizeof(header),
                     "{\"ok\":true,\"status\":200,\"contentType\":\"%s\"%s}\n",
                     file.contentType,
                     file.gzipped ? ",\"contentEncoding\":\"gzip\"" : "");
    out.write(header, static_cast<size_t>(n));

    // Streams out chunk-by-chunk through the session window; a 200 KB bundle
    // never needs a 200 KB buffer here or on the transport.
    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        out.write(buf, r);

    fclose(f);
}
