#include "http_file_server.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

static const char *TAG = "HttpFileServer";

// PSRAM scratch buffer for file streaming (large for throughput)
#define SCRATCH_BUFSIZE 8192

HttpFileServer &HttpFileServer::GetInstance() {
    static HttpFileServer instance;
    return instance;
}

std::string HttpFileServer::GetDeviceIp() {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
        uint32_t addr = ip_info.ip.addr;
        char buf[24];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                 (int)(addr & 0xFF), (int)((addr >> 8) & 0xFF),
                 (int)((addr >> 16) & 0xFF), (int)((addr >> 24) & 0xFF));
        return std::string(buf);
    }
    return "";
}

std::string HttpFileServer::GetUrl() const {
    std::string ip = GetDeviceIp();
    if (ip.empty()) return "";
    char buf[64];
    snprintf(buf, sizeof(buf), "http://%s:%u/", ip.c_str(), (unsigned)port_);
    return std::string(buf);
}

bool HttpFileServer::Start(uint16_t port) {
    if (server_ != nullptr) {
        ESP_LOGW(TAG, "Already running on port %u", (unsigned)port_);
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;  // close least-recent connection when full
    config.stack_size = 8192;        // default 4096 too small for dir browse + file serve
    config.recv_wait_timeout = 10;    // seconds (default 5)
    config.send_wait_timeout = 30;   // seconds (default 5, too short for multi-MB files)
    config.uri_match_fn = httpd_uri_match_wildcard;  // enable wildcard URI matching

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %u", (unsigned)port);
        server_ = nullptr;
        return false;
    }

    // Register handlers
    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = IndexHandler;
    httpd_register_uri_handler(server_, &index_uri);

    httpd_uri_t status_uri = {};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = StatusHandler;
    httpd_register_uri_handler(server_, &status_uri);

    // Wildcard: match everything else for dir browsing / file download
    httpd_uri_t wild_uri = {};
    wild_uri.uri = "/*";
    wild_uri.method = HTTP_GET;
    wild_uri.handler = WildcardHandler;
    httpd_register_uri_handler(server_, &wild_uri);

    port_ = port;
    std::string url = GetUrl();
    ESP_LOGI(TAG, "HTTP file server started: %s", url.c_str());
    return true;
}

void HttpFileServer::Stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP file server stopped");
    }
}

// --- Handlers ---

esp_err_t HttpFileServer::IndexHandler(httpd_req_t *req) {
    const char *html =
        "<html><head><meta charset=\"utf-8\"><title>小智 SD 卡文件</title>"
        "<style>body{font-family:sans-serif;margin:2em}a{display:block;padding:4px 0}"
        "</style></head><body>"
        "<h1>SD 卡文件浏览</h1>"
        "<p><a href=\"/logs/chatlogs/\">对话日志 (chatlogs)</a></p>"
        "<p><a href=\"/logs/\">系统日志 (logs)</a></p>"
        "<p><a href=\"/records/\">录音文件 (records)</a></p>"
        "<p><a href=\"/music/\">音乐文件 (music)</a></p>"
        "<hr><a href=\"/status\">服务器状态</a>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

esp_err_t HttpFileServer::StatusHandler(httpd_req_t *req) {
    std::string ip = GetDeviceIp();
    bool running = GetInstance().server_ != nullptr;
    char json[256];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"ip\":\"%s\",\"port\":%u,\"url\":\"http://%s:%u/\"}",
             running ? "true" : "false",
             ip.c_str(), (unsigned)GetInstance().port_,
             ip.c_str(), (unsigned)GetInstance().port_);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t HttpFileServer::WildcardHandler(httpd_req_t *req) {
    // req->uri is the path after the host, e.g. "/logs/chatlogs/chat_xxx.txt"
    const char *uri = req->uri;
    if (uri == nullptr || uri[0] != '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }

    // Prevent path traversal
    if (strstr(uri, "..") != nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path traversal not allowed");
        return ESP_OK;
    }

    // Map URI → SD card path: "/logs/chatlogs/" → "/sdcard/logs/chatlogs/"
    // The URI starts with '/', so we prepend "/sdcard" directly.
    char sd_path[512];
    snprintf(sd_path, sizeof(sd_path), "/sdcard%.*s", (int)(sizeof(sd_path) - 8), uri);
    ESP_LOGI(TAG, "Request: %s -> %s", uri, sd_path);

    // Is it a directory or a file?
    struct stat st;
    if (stat(sd_path, &st) != 0) {
        ESP_LOGE(TAG, "stat failed: %s errno=%d", sd_path, errno);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    if (S_ISDIR(st.st_mode)) {
        ServeDirectory(req, sd_path, uri);
    } else {
        ServeFile(req, sd_path, uri);
    }
    return ESP_OK;
}

// --- Directory listing (HTML with download links) ---

void HttpFileServer::ServeDirectory(httpd_req_t *req, const char *sd_path, const char *uri_path) {
    DIR *dir = opendir(sd_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Cannot open directory");
        return;
    }

    // Begin HTML response
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char header[768];
    snprintf(header, sizeof(header),
             "<html><head><meta charset=\"utf-8\"><title>%s</title>"
             "<style>body{font-family:sans-serif;margin:2em}"
             "a{display:block;padding:3px 0;text-decoration:none}"
             "a:hover{text-decoration:underline}.size{color:#888;font-size:0.85em}"
             "</style></head><body><h1>%s</h1>",
             uri_path, uri_path);
    httpd_resp_sendstr_chunk(req, header);

    // Parent link (not for root)
    if (strcmp(uri_path, "/") != 0 && strlen(uri_path) > 1) {
        // Find last '/' and trim
        char parent[512];
        strncpy(parent, uri_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        // Ensure trailing slash removed
        size_t len = strlen(parent);
        if (len > 1 && parent[len - 1] == '/') parent[len - 1] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash) {
            // Keep up to and including the slash
            last_slash[1] = '\0';
        } else {
            strcpy(parent, "/");
        }
        char link[640];
        snprintf(link, sizeof(link), "<a href=\"%s\">&#8592; 返回</a><hr>", parent);
        httpd_resp_sendstr_chunk(req, link);
    }

    // List entries
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;  // skip hidden

        // Build full path for stat
        char full[640];
        snprintf(full, sizeof(full), "%s/%s", sd_path, ent->d_name);
        struct stat est;
        long size = 0;
        bool is_dir = false;
        if (stat(full, &est) == 0) {
            size = est.st_size;
            is_dir = S_ISDIR(est.st_mode);
        }

        // Build href (ensure directory paths end with '/')
        char href[640];
        strncpy(href, uri_path, sizeof(href) - 1);
        href[sizeof(href) - 1] = '\0';
        size_t hlen = strlen(href);
        if (hlen > 0 && href[hlen - 1] != '/') {
            strncat(href, "/", sizeof(href) - strlen(href) - 1);
        }
        strncat(href, ent->d_name, sizeof(href) - strlen(href) - 1);

        // Build entry HTML via strncat (avoids format-truncation warnings
        // from snprintf with variable-length d_name/uri_path).
        char entry[1200];
        entry[0] = '\0';
        if (is_dir) {
            strncat(entry, "<a href=\"", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, href, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "/\">&#128193; ", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, ent->d_name, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "/</a>", sizeof(entry) - strlen(entry) - 1);
        } else {
            // Human-readable size
            char size_str[32];
            if (size < 1024) snprintf(size_str, sizeof(size_str), "%ld B", size);
            else if (size < 1048576) snprintf(size_str, sizeof(size_str), "%.1f KB", size / 1024.0);
            else snprintf(size_str, sizeof(size_str), "%.1f MB", size / 1048576.0);
            strncat(entry, "<a href=\"", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, href, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "\">&#128196; ", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, ent->d_name, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "</a> <span class=\"size\">", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, size_str, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "</span>", sizeof(entry) - strlen(entry) - 1);
        }
        httpd_resp_sendstr_chunk(req, entry);
        count++;
    }
    closedir(dir);

    char footer[128];
    snprintf(footer, sizeof(footer), "<hr><p>%d 个项目</p></body></html>", count);
    httpd_resp_sendstr_chunk(req, footer);
    httpd_resp_sendstr_chunk(req, NULL);  // end response
}

// --- File download (streamed with PSRAM buffer) ---

void HttpFileServer::ServeFile(httpd_req_t *req, const char *sd_path, const char *uri_path) {
    ESP_LOGI(TAG, "ServeFile: opening %s", sd_path);
    FILE *fd = fopen(sd_path, "rb");
    if (!fd) {
        ESP_LOGE(TAG, "ServeFile: fopen failed %s errno=%d", sd_path, errno);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Cannot open file");
        return;
    }

    // Get file size
    fseek(fd, 0, SEEK_END);
    long file_size = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    ESP_LOGI(TAG, "ServeFile: %s size=%ld bytes", sd_path, file_size);

    // Set Content-Length so the browser can show total size + download progress.
    // Without this, httpd defaults to chunked transfer encoding and the browser
    // only shows bytes received so far.
    char len_str[24];
    snprintf(len_str, sizeof(len_str), "%ld", file_size);
    httpd_resp_set_hdr(req, "Content-Length", len_str);

    // Set content type
    const char *ctype = GetContentType(uri_path);
    httpd_resp_set_type(req, ctype);

    // Set Content-Disposition for download (use basename)
    const char *basename = strrchr(uri_path, '/');
    if (basename) basename++;
    else basename = uri_path;
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", basename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    // Stream with PSRAM scratch buffer (fallback to internal RAM)
    size_t chunk_bufsize = SCRATCH_BUFSIZE;
    char *scratch = (char *)heap_caps_malloc(SCRATCH_BUFSIZE, MALLOC_CAP_SPIRAM);
    if (!scratch) {
        scratch = (char *)malloc(4096);
        if (!scratch) {
            ESP_LOGE(TAG, "ServeFile: OOM for scratch buffer");
            fclose(fd);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return;
        }
        chunk_bufsize = 4096;
        ESP_LOGW(TAG, "ServeFile: PSRAM alloc failed, using internal RAM 4KB");
    }

    size_t read_bytes;
    long total_sent = 0;
    int chunk_count = 0;
    while ((read_bytes = fread(scratch, 1, chunk_bufsize, fd)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, scratch, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ServeFile: send_chunk failed at offset %ld, err=0x%x", total_sent, err);
            break;
        }
        total_sent += read_bytes;
        chunk_count++;
        if (chunk_count % 100 == 0) {
            ESP_LOGI(TAG, "ServeFile: progress %ld/%ld (%d%%)", total_sent, file_size,
                     (int)(file_size > 0 ? total_sent * 100 / file_size : 100));
        }
    }

    fclose(fd);
    free(scratch);
    httpd_resp_send_chunk(req, NULL, 0);  // end response
    ESP_LOGI(TAG, "Served %s (%ld/%ld bytes, %d chunks)", sd_path, total_sent, file_size, chunk_count);
}

// --- Content type by extension ---

const char *HttpFileServer::GetContentType(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "log") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(dot, "html") == 0 || strcasecmp(dot, "htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(dot, "json") == 0) return "application/json";
    if (strcasecmp(dot, "wav") == 0) return "audio/wav";
    if (strcasecmp(dot, "mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, "csv") == 0) return "text/csv";
    return "application/octet-stream";
}
