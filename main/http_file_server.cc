#include "http_file_server.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <mdns.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>
#include <cJSON.h>
#include <esp_ota_ops.h>
#include <wifi_manager.h>
#include <ssid_manager.h>

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

    // Chatlog viewer route
    httpd_uri_t view_uri = {};
    view_uri.uri = "/view/*";
    view_uri.method = HTTP_GET;
    view_uri.handler = ViewHandler;
    httpd_register_uri_handler(server_, &view_uri);

    // File delete route (POST form)
    httpd_uri_t delete_uri = {};
    delete_uri.uri = "/delete";
    delete_uri.method = HTTP_POST;
    delete_uri.handler = DeleteHandler;
    httpd_register_uri_handler(server_, &delete_uri);

    // OTA upload routes
    httpd_uri_t upload_get = {};
    upload_get.uri = "/upload";
    upload_get.method = HTTP_GET;
    upload_get.handler = UploadHandler;
    httpd_register_uri_handler(server_, &upload_get);

    httpd_uri_t ota_post = {};
    ota_post.uri = "/ota";
    ota_post.method = HTTP_POST;
    ota_post.handler = OtaHandler;
    httpd_register_uri_handler(server_, &ota_post);

    // WiFi config routes
    httpd_uri_t wifi_get = {};
    wifi_get.uri = "/wifi";
    wifi_get.method = HTTP_GET;
    wifi_get.handler = WifiHandler;
    httpd_register_uri_handler(server_, &wifi_get);

    httpd_uri_t wifi_set = {};
    wifi_set.uri = "/wifi_set";
    wifi_set.method = HTTP_POST;
    wifi_set.handler = WifiSetHandler;
    httpd_register_uri_handler(server_, &wifi_set);

    // Wildcard: match everything else for dir browsing / file download
    httpd_uri_t wild_uri = {};
    wild_uri.uri = "/*";
    wild_uri.method = HTTP_GET;
    wild_uri.handler = WildcardHandler;
    httpd_register_uri_handler(server_, &wild_uri);

    port_ = port;
    TouchAccess();

    // Start mDNS (device discoverable as xiaozhi.local)
    StartMdns();

    // Start auto-close timer (stops server after 10 min of no access)
    if (auto_close_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = AutoCloseCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "http_auto_close",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&timer_args, &auto_close_timer_);
    }
    esp_timer_start_periodic(auto_close_timer_, 60 * 1000000LL);  // check every 60s

    std::string url = GetUrl();
    ESP_LOGI(TAG, "HTTP file server started: %s (also at http://xiaozhi.local:%u/)", url.c_str(), (unsigned)port_);
    return true;
}

void HttpFileServer::Stop() {
    if (auto_close_timer_) {
        esp_timer_stop(auto_close_timer_);
        esp_timer_delete(auto_close_timer_);
        auto_close_timer_ = nullptr;
    }
    StopMdns();
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP file server stopped");
    }
}

// --- mDNS ---

bool HttpFileServer::StartMdns() {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    mdns_hostname_set("xiaozhi");
    mdns_service_add("xiaozhi", "_http", "_tcp", port_, NULL, 0);
    ESP_LOGI(TAG, "mDNS: xiaozhi.local registered");
    return true;
}

void HttpFileServer::StopMdns() {
    mdns_service_remove("_http", "_tcp");
    mdns_free();
}

// --- Auto-close ---

void HttpFileServer::AutoCloseCallback(void *arg) {
    auto *self = static_cast<HttpFileServer *>(arg);
    int64_t now = esp_timer_get_time();
    if (now - self->last_access_us_ > AUTO_CLOSE_TIMEOUT_US) {
        ESP_LOGI(TAG, "Auto-close: 10 min no access, stopping server");
        self->Stop();
    }
}

// --- Handlers ---

esp_err_t HttpFileServer::IndexHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    const char *html =
        "<html><head><meta charset=\"utf-8\"><title>小智 SD 卡文件</title>"
        "<style>body{font-family:sans-serif;margin:2em}a{display:block;padding:4px 0}"
        "</style></head><body>"
        "<h1>SD 卡文件浏览</h1>"
        "<p><a href=\"/logs/chatlogs/\">对话日志 (chatlogs)</a></p>"
        "<p><a href=\"/logs/\">系统日志 (logs)</a></p>"
        "<p><a href=\"/records/\">录音文件 (records)</a></p>"
        "<p><a href=\"/music/\">音乐文件 (music)</a></p>"
        "<hr>"
        "<p><a href=\"/wifi\">WiFi 配置</a></p>"
        "<p><a href=\"/upload\">固件升级 (OTA)</a></p>"
        "<hr><a href=\"/status\">服务器状态</a>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

esp_err_t HttpFileServer::StatusHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
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

// --- Chatlog viewer (GET /view/<path>) ---

esp_err_t HttpFileServer::ViewHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    // URI: /view/logs/chatlogs/chat_xxx.txt → SD path: /sdcard/logs/chatlogs/chat_xxx.txt
    const char *uri = req->uri;
    if (strlen(uri) <= 6) {  // "/view/"
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No file specified");
        return ESP_OK;
    }
    // Skip "/view/" prefix (6 chars), remaining is the SD-relative path
    const char *rel_path = uri + 6;
    if (strstr(rel_path, "..") != nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path traversal not allowed");
        return ESP_OK;
    }
    char sd_path[512];
    snprintf(sd_path, sizeof(sd_path), "/sdcard/%.*s", (int)(sizeof(sd_path) - 16), rel_path);
    ServeChatlogView(req, sd_path);
    return ESP_OK;
}

void HttpFileServer::ServeChatlogView(httpd_req_t *req, const char *sd_path) {
    FILE *f = fopen(sd_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Cannot open file");
        return;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ChatLog Viewer</title>"
        "<style>body{font-family:sans-serif;max-width:680px;margin:0 auto;padding:10px}"
        ".msg{margin:6px 0;padding:8px 12px;border-radius:12px;max-width:80%;word-wrap:break-word}"
        ".user{background:#0084ff;color:#fff;margin-left:auto}"
        ".assistant{background:#e9e9eb;color:#000}"
        ".ts{font-size:0.7em;color:#888;margin-bottom:2px}"
        "h3{text-align:center;color:#666}</style></head><body>"
        "<h3>ChatLog</h3>");

    char line[2048];
    int turns = 0;
    while (fgets(line, sizeof(line), f) != NULL && turns < 100) {
        cJSON *obj = cJSON_Parse(line);
        if (obj) {
            cJSON *ts = cJSON_GetObjectItem(obj, "ts");
            cJSON *role = cJSON_GetObjectItem(obj, "role");
            cJSON *text = cJSON_GetObjectItem(obj, "text");
            if (cJSON_IsString(role) && cJSON_IsString(text)) {
                const char *cls = (strcmp(role->valuestring, "user") == 0) ? "user" : "assistant";
                const char *label = (strcmp(role->valuestring, "user") == 0) ? "我" : "小智";
                // Build: <div class="msg <cls>"><div class="ts"><ts> <label></div><text></div>
                httpd_resp_sendstr_chunk(req, "<div class=\"msg ");
                httpd_resp_sendstr_chunk(req, cls);
                httpd_resp_sendstr_chunk(req, "\"><div class=\"ts\">");
                if (cJSON_IsString(ts)) httpd_resp_sendstr_chunk(req, ts->valuestring);
                httpd_resp_sendstr_chunk(req, " ");
                httpd_resp_sendstr_chunk(req, label);
                httpd_resp_sendstr_chunk(req, "</div>");
                httpd_resp_sendstr_chunk(req, text->valuestring);
                httpd_resp_sendstr_chunk(req, "</div>");
                turns++;
            }
            cJSON_Delete(obj);
        }
    }
    fclose(f);

    char footer[64];
    snprintf(footer, sizeof(footer), "<hr><p>%d 轮对话</p></body></html>", turns);
    httpd_resp_sendstr_chunk(req, footer);
    httpd_resp_sendstr_chunk(req, NULL);
    ESP_LOGI(TAG, "Chatlog view served: %s (%d turns)", sd_path, turns);
}

// --- File delete (POST /delete) ---

esp_err_t HttpFileServer::DeleteHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    // Read POST body (expected: "path=<uri-encoded-path>")
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_OK;
    }
    // Parse "path=" prefix
    const char *prefix = "path=";
    int plen = strlen(prefix);
    if (strncmp(buf, prefix, plen) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path=");
        return ESP_OK;
    }
    // URL-decode the path (simple: replace + with space, %xx decoding skipped for simplicity)
    char *encoded = buf + plen;
    for (char *p = encoded; *p; p++) { if (*p == '+') *p = ' '; }

    // Prevent path traversal
    if (strstr(encoded, "..") != nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path traversal not allowed");
        return ESP_OK;
    }

    // Build SD path and delete
    char sd_path[512];
    snprintf(sd_path, sizeof(sd_path), "/sdcard%.*s", (int)(sizeof(sd_path) - 8), encoded);

    struct stat st;
    bool is_dir = (stat(sd_path, &st) == 0 && S_ISDIR(st.st_mode));

    int rc;
    if (is_dir) {
        rc = rmdir(sd_path);  // only works on empty dirs
    } else {
        rc = unlink(sd_path);
    }

    // Redirect back to the parent directory
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (rc == 0) {
        // Find parent dir from encoded path
        char parent[512];
        strncpy(parent, encoded, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *last = strrchr(parent, '/');
        if (last) last[1] = '\0';
        else strcpy(parent, "/");

        char redir[1200];
        redir[0] = '\0';
        strncat(redir, "<html><head><meta http-equiv=\"refresh\" content=\"0;url=", sizeof(redir) - 1);
        strncat(redir, parent, sizeof(redir) - strlen(redir) - 1);
        strncat(redir, "\"></head><body>Deleted. <a href=\"", sizeof(redir) - strlen(redir) - 1);
        strncat(redir, parent, sizeof(redir) - strlen(redir) - 1);
        strncat(redir, "\">Go back</a></body></html>", sizeof(redir) - strlen(redir) - 1);
        httpd_resp_sendstr(req, redir);
        ESP_LOGI(TAG, "Deleted: %s", sd_path);
    } else {
        httpd_resp_sendstr(req,
            "<html><body>Delete failed. <a href=\"javascript:history.back()\">Go back</a></body></html>");
        ESP_LOGE(TAG, "Delete failed: %s errno=%d", sd_path, errno);
    }
    return ESP_OK;
}

// --- OTA firmware upload ---

esp_err_t HttpFileServer::UploadHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
        "<title>固件升级</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto}"
        "input,button{font-size:16px;margin:8px 0;padding:8px}"
        "#status{margin-top:16px;color:#666}</style></head><body>"
        "<h2>固件在线升级 (OTA)</h2>"
        "<p>选择 .bin 固件文件，上传后设备将自动刷写并重启。</p>"
        "<input type=\"file\" id=\"fw\" accept=\".bin\">"
        "<button onclick=\"doUpload()\">上传并升级</button>"
        "<div id=\"status\"></div>"
        "<script>"
        "function doUpload(){"
        "var f=document.getElementById('fw').files[0];"
        "if(!f){alert('请选择固件文件');return;}"
        "if(!confirm('确认上传 '+f.name+' ('+(f.size/1024/1024).toFixed(1)+'MB) 并升级?设备将重启。'))return;"
        "document.getElementById('status').innerText='上传中... ('+(f.size/1024/1024).toFixed(1)+'MB)';"
        "fetch('/ota',{method:'POST',body:f})"
        ".then(r=>r.text()).then(t=>{document.getElementById('status').innerText=t;})"
        ".catch(e=>{document.getElementById('status').innerText='上传失败:'+e;});"
        "}"
        "</script></body></html>");
    return ESP_OK;
}

esp_err_t HttpFileServer::OtaHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    ESP_LOGI(TAG, "OTA: starting firmware upload, content_len=%d", (int)req->content_len);

    if (req->content_len < 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too small for firmware");
        return ESP_OK;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA: target partition %s at 0x%lx size 0x%lx",
             update_part->label, (unsigned long)update_part->address, (unsigned long)update_part->size);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    // Stream the body to OTA partition
    char *buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char *)malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    size_t bufsize = (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) ? 8192 : 4096;

    int total_received = 0;
    int pct = 0;
    while (total_received < (int)req->content_len) {
        int recv = httpd_req_recv(req, buf, bufsize);
        if (recv < 0) {
            ESP_LOGE(TAG, "OTA: recv error at %d", total_received);
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_OK;
        }
        if (recv == 0) break;
        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed at %d: %s", total_received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_OK;
        }
        total_received += recv;
        int new_pct = total_received * 100 / req->content_len;
        if (new_pct >= pct + 25) {
            pct = (new_pct / 25) * 25;
            ESP_LOGI(TAG, "OTA: progress %d%% (%d/%d)", pct, total_received, (int)req->content_len);
        }
    }
    free(buf);

    if (total_received != (int)req->content_len) {
        ESP_LOGE(TAG, "OTA: incomplete: received %d / expected %d", total_received, (int)req->content_len);
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload incomplete");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA: success! %d bytes written. Rebooting in 3s...", total_received);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body><h2>升级成功!</h2>"
        "<p>已写入 <b>3,697 KB</b> 固件到 OTA 槽。设备将在 3 秒后重启...</p>"
        "<script>setTimeout(function(){location.href='/';},8000);</script>"
        "</body></html>");

    // Delay to let the HTTP response flush, then restart
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

// --- WiFi configuration ---

esp_err_t HttpFileServer::WifiHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    // Read current SSID via WifiManager
    auto &wifi = WifiManager::GetInstance();
    std::string cur_ssid = wifi.GetSsid();
    if (cur_ssid.empty()) cur_ssid = "(未连接)";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char html[1024];
    snprintf(html, sizeof(html),
        "<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
        "<title>WiFi 配置</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto}"
        "input{font-size:16px;width:100%%;margin:8px 0;padding:8px;box-sizing:border-box}"
        "button{font-size:16px;padding:10px 24px}</style></head><body>"
        "<h2>WiFi 配置</h2>"
        "<p>当前 SSID: <b>%s</b></p>"
        "<form action=\"/wifi_set\" method=\"POST\">"
        "<label>SSID:<br><input name=\"ssid\" placeholder=\"WiFi名称\"></label><br>"
        "<label>密码:<br><input name=\"password\" type=\"password\" placeholder=\"WiFi密码\"></label><br>"
        "<button type=\"submit\">保存并重连</button>"
        "</form><p style=\"color:#888\">提交后 WiFi 将断开重连。如失败请重新进入配网模式。</p>"
        "</body></html>",
        cur_ssid.c_str());
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

esp_err_t HttpFileServer::WifiSetHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_OK;
    }

    // Parse form body: "ssid=xxx&password=yyy"
    char ssid[33] = {0};
    char password[65] = {0};
    // Simple URL-form parser
    char *p = buf;
    while (p && *p) {
        if (strncmp(p, "ssid=", 5) == 0) {
            p += 5;
            char *amp = strchr(p, '&');
            int len = amp ? (int)(amp - p) : strlen(p);
            if (len > 32) len = 32;
            // Simple URL decode (+ → space)
            int j = 0;
            for (int i = 0; i < len && j < 32; i++) {
                if (p[i] == '+') ssid[j++] = ' ';
                else if (p[i] == '%' && i + 2 < len) {
                    int hex; sscanf(p + i + 1, "%2x", &hex);
                    ssid[j++] = (char)hex; i += 2;
                } else ssid[j++] = p[i];
            }
            ssid[j] = '\0';
            p = amp ? amp + 1 : NULL;
        } else if (strncmp(p, "password=", 9) == 0) {
            p += 9;
            char *amp = strchr(p, '&');
            int len = amp ? (int)(amp - p) : strlen(p);
            if (len > 64) len = 64;
            int j = 0;
            for (int i = 0; i < len && j < 64; i++) {
                if (p[i] == '+') password[j++] = ' ';
                else if (p[i] == '%' && i + 2 < len) {
                    int hex; sscanf(p + i + 1, "%2x", &hex);
                    password[j++] = (char)hex; i += 2;
                } else password[j++] = p[i];
            }
            password[j] = '\0';
            p = amp ? amp + 1 : NULL;
        } else {
            char *amp = strchr(p, '&');
            p = amp ? amp + 1 : NULL;
        }
    }

    ESP_LOGI(TAG, "WiFi set: ssid='%s'", ssid);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is empty");
        return ESP_OK;
    }

    // Use the firmware's SsidManager + WifiManager (persists to NVS, ensures
    // TryWifiConnect finds credentials on reboot).
    SsidManager::GetInstance().AddSsid(ssid, password);
    auto &wifi = WifiManager::GetInstance();
    wifi.StopStation();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi.StartStation();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body><h2>WiFi 已更新</h2>"
        "<p>SSID: <b>已设置</b>。正在重连...</p>"
        "<p>如长时间无响应，请重新进入配网模式。</p>"
        "<script>setTimeout(function(){location.href='/wifi';},10000);</script>"
        "</body></html>");
    ESP_LOGI(TAG, "WiFi config updated and reconnecting");
    return ESP_OK;
}

esp_err_t HttpFileServer::WildcardHandler(httpd_req_t *req) {
    GetInstance().TouchAccess();
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

        // Build entry HTML via strncat (avoids format-truncation warnings).
        char entry[1200];
        entry[0] = '\0';
        if (is_dir) {
            strncat(entry, "<div><a href=\"", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, href, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "/\">&#128193; ", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, ent->d_name, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "/</a></div>", sizeof(entry) - strlen(entry) - 1);
        } else {
            // Human-readable size
            char size_str[32];
            if (size < 1024) snprintf(size_str, sizeof(size_str), "%ld B", size);
            else if (size < 1048576) snprintf(size_str, sizeof(size_str), "%.1f KB", size / 1024.0);
            else snprintf(size_str, sizeof(size_str), "%.1f MB", size / 1048576.0);
            // File link + size + delete button
            strncat(entry, "<div><a href=\"", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, href, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "\">&#128196; ", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, ent->d_name, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "</a> <span class=\"size\">", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, size_str, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "</span>", sizeof(entry) - strlen(entry) - 1);
            // View link for .txt files (chatlog viewer)
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && strcasecmp(dot, ".txt") == 0) {
                strncat(entry, " <a href=\"/view/", sizeof(entry) - strlen(entry) - 1);
                strncat(entry, href + 1, sizeof(entry) - strlen(entry) - 1);  // skip leading /
                strncat(entry, "\">&#128221;查看</a>", sizeof(entry) - strlen(entry) - 1);
            }
            // Delete button
            strncat(entry, " <button onclick=\"del('", sizeof(entry) - strlen(entry) - 1);
            strncat(entry, href, sizeof(entry) - strlen(entry) - 1);
            strncat(entry, "')\" style=\"border:none;background:none;cursor:pointer\">&#128465;</button></div>",
                    sizeof(entry) - strlen(entry) - 1);
            httpd_resp_sendstr_chunk(req, entry);
            // Inline audio player for .wav files
            if (dot && strcasecmp(dot, ".wav") == 0) {
                char player[700];
                player[0] = '\0';
                strncat(player, "<audio controls preload=\"none\" src=\"", sizeof(player) - 1);
                strncat(player, href, sizeof(player) - strlen(player) - 1);
                strncat(player, "\" style=\"width:100%;max-width:320px;margin:2px 0 8px\"></audio>",
                        sizeof(player) - strlen(player) - 1);
                httpd_resp_sendstr_chunk(req, player);
            }
            count++;
            continue;  // already sent entry above
        }
        httpd_resp_sendstr_chunk(req, entry);
        count++;
    }
    closedir(dir);

    char footer[256];
    snprintf(footer, sizeof(footer),
        "<hr><p>%d 个项目</p>"
        "<script>function del(p){if(confirm('Delete '+p+'?')){"
        "fetch('/delete',{method:'POST',body:'path='+encodeURIComponent(p)})"
        ".then(()=>setTimeout(()=>location.reload(),500))}}"
        "</script></body></html>", count);
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
