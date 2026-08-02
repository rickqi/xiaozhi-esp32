#ifndef HTTP_FILE_SERVER_H
#define HTTP_FILE_SERVER_H

#include <esp_http_server.h>
#include <esp_timer.h>
#include <string>

// Lightweight WiFi HTTP file server for downloading / browsing SD card files
// (logs, chatlogs, recordings, music) via a browser or curl. Avoids serial
// port contention entirely — runs over the already-active WiFi link.
//
// Usage:
//   HttpFileServer::GetInstance().Start(80);   // http://<device-ip>/
//   HttpFileServer::GetInstance().Stop();
//
// Routes:
//   GET  /              — index page (links to /sdcard subdirs)
//   GET  /status        — JSON server status
//   GET  /view/<path>   — chatlog viewer (renders JSONL as chat bubbles)
//   POST /delete        — delete a file (form body: path=<uri>)
//   GET  /*             — browse directory (HTML) or download / play file
//   mDNS                — device discoverable as xiaozhi.local
//   Auto-close          — stops after 10 min of no access
class HttpFileServer {
public:
    static HttpFileServer& GetInstance();

    bool Start(uint16_t port = 80);
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }
    std::string GetUrl() const;

private:
    HttpFileServer() = default;
    httpd_handle_t server_ = nullptr;
    uint16_t port_ = 0;

    // Auto-close timer
    esp_timer_handle_t auto_close_timer_ = nullptr;
    int64_t last_access_us_ = 0;  // last time any handler was called
    static constexpr int64_t AUTO_CLOSE_TIMEOUT_US = 10 * 60 * 1000000LL;  // 10 min
    void TouchAccess() { last_access_us_ = esp_timer_get_time(); }
    static void AutoCloseCallback(void *arg);

    // mDNS
    bool StartMdns();
    void StopMdns();

    // URI handlers
    static esp_err_t IndexHandler(httpd_req_t *req);      // GET /
    static esp_err_t StatusHandler(httpd_req_t *req);     // GET /status
    static esp_err_t ViewHandler(httpd_req_t *req);       // GET /view/* (chatlog viewer)
    static esp_err_t DeleteHandler(httpd_req_t *req);     // POST /delete
    static esp_err_t UploadHandler(httpd_req_t *req);     // GET /upload (OTA upload form)
    static esp_err_t OtaHandler(httpd_req_t *req);        // POST /ota (receive firmware, flash, reboot)
    static esp_err_t WifiHandler(httpd_req_t *req);       // GET /wifi (WiFi config form)
    static esp_err_t WifiSetHandler(httpd_req_t *req);    // POST /wifi_set (update WiFi config)
    static esp_err_t SensorHandler(httpd_req_t *req);     // GET /sensors (sensor history table)
    static esp_err_t WildcardHandler(httpd_req_t *req);   // GET /* (dir browse / file download)

    // Helpers
    static std::string GetDeviceIp();
    static void ServeDirectory(httpd_req_t *req, const char *sd_path, const char *uri_path);
    static void ServeFile(httpd_req_t *req, const char *sd_path, const char *uri_path);
    static void ServeChatlogView(httpd_req_t *req, const char *sd_path);
    static const char *GetContentType(const char *filename);
};

#endif  // HTTP_FILE_SERVER_H
