#ifndef HTTP_FILE_SERVER_H
#define HTTP_FILE_SERVER_H

#include <esp_http_server.h>
#include <string>

// Lightweight WiFi HTTP file server for downloading SD card files (logs,
// chatlogs, recordings, music) via a browser or curl. Avoids serial port
// contention entirely — runs over the already-active WiFi link.
//
// Usage:
//   HttpFileServer::GetInstance().Start(80);   // http://<device-ip>/
//   HttpFileServer::GetInstance().Stop();
//
// Routes (wildcard):
//   GET /          — index page (links to /sdcard subdirs)
//   GET /status    — JSON server status
//   GET /<path>    — browse directory (HTML) or download file
//                    e.g. /logs/chatlogs/  → list chatlogs
//                         /logs/chatlogs/chat_xxx.txt → download
//                         /records/rec_xxx.wav → download
class HttpFileServer {
public:
    static HttpFileServer& GetInstance();

    // Start the HTTP server on the given port. Returns true on success.
    // Idempotent: calling Start() while already running is a no-op.
    bool Start(uint16_t port = 80);

    // Stop the HTTP server. Safe to call when not running.
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }

    // Returns the URL string "http://<ip>:<port>/" for display, or empty
    // if WiFi is not connected.
    std::string GetUrl() const;

private:
    HttpFileServer() = default;
    httpd_handle_t server_ = nullptr;
    uint16_t port_ = 0;

    // URI handlers
    static esp_err_t IndexHandler(httpd_req_t *req);      // GET /
    static esp_err_t StatusHandler(httpd_req_t *req);     // GET /status
    static esp_err_t WildcardHandler(httpd_req_t *req);   // GET /* (dir browse / file download)

    // Helpers
    static std::string GetDeviceIp();
    static void ServeDirectory(httpd_req_t *req, const char *sd_path, const char *uri_path);
    static void ServeFile(httpd_req_t *req, const char *sd_path, const char *uri_path);
    static const char *GetContentType(const char *filename);
};

#endif  // HTTP_FILE_SERVER_H
