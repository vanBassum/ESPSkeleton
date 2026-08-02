#pragma once

#include <esp_http_server.h>
#include <cstddef>

class StaticFileHandler {
public:
    // A resolved static file: where it actually lives, plus the two HTTP facts a
    // route layer needs to serve it.
    struct Resolved {
        char        path[600];
        const char* contentType;
        bool        gzipped;
    };

    // Logical path → stored file. Shared by the local HTTP route and the
    // `getWebFile` command that serves the relay, so both agree on `.gz`
    // preference and MIME type. Accepts an optional query string and a path with
    // or without a leading '/'.
    //
    // Deliberately does NOT do SPA fallback — a missing path returns false.
    // Falling back to index.html is an HTTP decision that belongs to each route
    // layer, which keeps a mistyped asset a real 404 instead of HTML with status
    // 200 (which a browser rejects as a MIME error).
    static bool Resolve(const char* basePath, const char* uri, Resolved& out);

    void RegisterRoute(httpd_handle_t server, const char* basePath);

private:
    static esp_err_t Handle(httpd_req_t* req);
    static const char* GetContentType(const char* ext);
    static bool IsSafePath(const char* uri);
};
