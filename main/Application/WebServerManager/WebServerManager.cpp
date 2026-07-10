#include "WebServerManager.h"
#include "CommandManager.h"
#include "ConsoleManager.h"
#include "Stream.h"
#include "SettingsManager.h"
#include "ContextLock.h"

#include <unistd.h>
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

    // The one API route: every command, streamed both ways.
    // (Must be before the wildcard static-file route.)
    const httpd_uri_t api_command = {
        .uri = "/api/command",
        .method = HTTP_POST,
        .handler = HandleApiCommand,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &api_command);

    // CORS preflight — browsers send OPTIONS before cross-origin POST
    // with a binary body (e.g. from the dev vite server).
    const httpd_uri_t api_command_opts = {
        .uri = "/api/command",
        .method = HTTP_OPTIONS,
        .handler = HandleCorsPreflight,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &api_command_opts);

    wsHandler_.RegisterRoute(server_);
    staticFileHandler_.RegisterRoute(server_, BASE_PATH);
}

void WebServerManager::Broadcast(const char* json, int len)
{
    if (server_)
        wsHandler_.Broadcast(server_, json, len);
}

void WebServerManager::BroadcastBinary(const uint8_t* data, size_t len)
{
    if (server_)
        wsHandler_.BroadcastBinary(server_, data, len);
}

// ──────────────────────────────────────────────────────────────
// /api/command — the generic command entrance. Request body in,
// handler reply out, streamed both directions. The HTTP layer knows
// no commands; HTTP status is only the transport envelope.
// ──────────────────────────────────────────────────────────────

namespace {

class HttpRequestStream : public Stream
{
    httpd_req_t* req_;
    int remaining_;

public:
    explicit HttpRequestStream(httpd_req_t* req)
        : req_(req), remaining_(req->content_len) {}

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;   // httpd's socket recv timeout applies
        if (remaining_ <= 0) return 0;
        int want = (size < static_cast<size_t>(remaining_)) ? static_cast<int>(size) : remaining_;
        int n = httpd_req_recv(req_, static_cast<char*>(dst), want);
        if (n <= 0) { remaining_ = 0; return 0; }
        remaining_ -= n;
        return static_cast<size_t>(n);
    }

    size_t write(const void*, size_t, TickType_t) override { return 0; }
    size_t available() const override { return remaining_ > 0 ? static_cast<size_t>(remaining_) : 0; }
};

class HttpResponseStream : public Stream
{
    httpd_req_t* req_;
    bool failed_ = false;

public:
    explicit HttpResponseStream(httpd_req_t* req) : req_(req) {}

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        if (failed_) return 0;
        if (httpd_resp_send_chunk(req_, static_cast<const char*>(data), size) != ESP_OK)
        {
            failed_ = true;
            return 0;
        }
        return size;
    }

    size_t read(void*, size_t, TickType_t) override { return 0; }
    bool failed() const { return failed_; }
};

} // namespace

esp_err_t WebServerManager::HandleApiCommand(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);

    if (!self->CheckBearer(req))
    {
        SendUnauthorized(req);
        return ESP_FAIL;
    }

    char query[96] = {};
    char type[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "type", type, sizeof(type)) != ESP_OK ||
        type[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?type=");
        return ESP_FAIL;
    }

    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/octet-stream");

    HttpRequestStream in(req);
    HttpResponseStream out(req);

    if (!self->serviceProvider_.getCommandManager().Execute(type, in, out))
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown command");
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, nullptr, 0);   // finish chunked response
    return out.failed() ? ESP_FAIL : ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// CORS
// ──────────────────────────────────────────────────────────────

void WebServerManager::SetCorsHeaders(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}

esp_err_t WebServerManager::HandleCorsPreflight(httpd_req_t* req)
{
    SetCorsHeaders(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// Auth — sessions at the transport edge. Nothing below this layer
// (commands, streams) ever sees a token or password.
// ──────────────────────────────────────────────────────────────

bool WebServerManager::CheckBearer(httpd_req_t* req)
{
    char hdr[48] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Bearer ", 7) != 0)
        return false;
    return auth_.ValidateKey(hdr + 7);
}

void WebServerManager::SendUnauthorized(httpd_req_t* req)
{
    // Manual 401 (esp_http_server's httpd_err_code_t has no 401) with
    // CORS headers so cross-origin JS can read the status.
    SetCorsHeaders(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}
