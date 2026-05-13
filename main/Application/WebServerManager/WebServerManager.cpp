#include "WebServerManager.h"
#include "CommandManager.h"
#include "ConsoleManager.h"
#include "UpdateManager.h"

#include <unistd.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <esp_partition.h>

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

    // Upload endpoints (must be before wildcard)
    const httpd_uri_t upload_app = {
        .uri = "/api/upload/app",
        .method = HTTP_POST,
        .handler = HandleUploadApp,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &upload_app);

    const httpd_uri_t upload_www = {
        .uri = "/api/upload/www",
        .method = HTTP_POST,
        .handler = HandleUploadWww,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &upload_www);

    // CORS preflight handlers — browsers send OPTIONS before cross-origin
    // POST with a binary body (e.g. from the dev vite server on a different origin).
    const httpd_uri_t upload_app_opts = {
        .uri = "/api/upload/app",
        .method = HTTP_OPTIONS,
        .handler = HandleCorsPreflight,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &upload_app_opts);

    const httpd_uri_t upload_www_opts = {
        .uri = "/api/upload/www",
        .method = HTTP_OPTIONS,
        .handler = HandleCorsPreflight,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &upload_www_opts);

    const httpd_uri_t download = {
        .uri = "/api/download",
        .method = HTTP_GET,
        .handler = HandleDownloadPartition,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &download);

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
// Upload handlers
// ──────────────────────────────────────────────────────────────

esp_err_t WebServerManager::HandleUploadApp(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);
    auto& update = self->serviceProvider_.getUpdateManager();

    ESP_LOGI(TAG, "App upload started (content-length: %d)", req->content_len);

    if (!update.BeginAppUpdate())
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to begin OTA");
        return ESP_FAIL;
    }

    char buf[1024];
    int received = 0;
    int total = 0;

    while (total < req->content_len)
    {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0)
        {
            ESP_LOGE(TAG, "App upload recv error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        if (!update.WriteAppChunk(buf, received))
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        total += received;
    }

    const char* err = update.FinalizeAppUpdate();
    if (err)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
        return ESP_FAIL;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"size\":%d}", total);
    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    ESP_LOGI(TAG, "App upload complete (%d bytes) — restart required to apply", total);
    return ESP_OK;
}

esp_err_t WebServerManager::HandleUploadWww(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);
    auto& update = self->serviceProvider_.getUpdateManager();

    ESP_LOGI(TAG, "WWW upload started (content-length: %d)", req->content_len);

    if (!update.BeginWwwUpdate())
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to begin WWW update");
        return ESP_FAIL;
    }

    char buf[1024];
    int received = 0;
    int total = 0;

    while (total < req->content_len)
    {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0)
        {
            ESP_LOGE(TAG, "WWW upload recv error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        if (!update.WriteWwwChunk(buf, received))
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        total += received;
    }

    const char* err = update.FinalizeWwwUpdate();
    if (err)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
        return ESP_FAIL;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"size\":%d}", total);
    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    ESP_LOGI(TAG, "WWW upload complete (%d bytes) — restart required to apply", total);
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// CORS + partition download
// ──────────────────────────────────────────────────────────────

void WebServerManager::SetCorsHeaders(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

esp_err_t WebServerManager::HandleCorsPreflight(httpd_req_t* req)
{
    SetCorsHeaders(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WebServerManager::HandleDownloadPartition(httpd_req_t* req)
{
    SetCorsHeaders(req);

    char query[96] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?partition=");
        return ESP_FAIL;
    }

    char label[17] = {};
    if (httpd_query_key_value(query, "partition", label, sizeof(label)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'partition' param");
        return ESP_FAIL;
    }

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Download partition '%s' (%lu bytes)", label, (unsigned long)p->size);

    httpd_resp_set_type(req, "application/octet-stream");

    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.bin\"", label);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    constexpr size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    size_t offset = 0;
    while (offset < p->size)
    {
        size_t n = (p->size - offset < CHUNK) ? (p->size - offset) : CHUNK;
        if (esp_partition_read(p, offset, buf, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), n) != ESP_OK)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return ESP_FAIL;
        }
        offset += n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}
