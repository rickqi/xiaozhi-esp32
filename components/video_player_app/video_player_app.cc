#include "video_player_app.h"
#include "avi_parser.h"

#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>

#define TAG "VideoPlayerApp"
#define LCD_W 410
#define LCD_H 502

esp_lcd_panel_handle_t VideoPlayerApp::s_panel = nullptr;

static volatile bool s_playing = false;
static lv_obj_t* s_status_label = nullptr;

LV_IMG_DECLARE(esp_brookesia_image_large_app_launcher_default_112_112);

VideoPlayerApp::VideoPlayerApp()
    : App("Video", &esp_brookesia_image_large_app_launcher_default_112_112,
          true, true, false) {}

static void SetStatusText(const char* text) {
    if (s_status_label != nullptr) {
        lv_label_set_text(s_status_label, text);
    }
}

static bool PlayOneAvi(FILE* f, avi_info_t* info) {
    // Decode + draw loop for one AVI file.
    uint32_t video_idx = 0;
    uint32_t audio_idx = 0;
    uint32_t frame_period_us = info->micro_sec_per_frame ? info->micro_sec_per_frame : 40000;

    std::vector<uint8_t> jpeg_buf(256 * 1024);
    std::vector<int16_t> pcm_buf(4096);

    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
    codec->EnableInput(false);

    int64_t next_frame_us = esp_timer_get_time();
    uint32_t frame_count = 0;
    int64_t fps_log_us = esp_timer_get_time();

    while (s_playing && video_idx < info->video_frame_count) {
        // Video: read MJPEG frame.
        const auto& vc = info->video_chunks[video_idx];
        if (vc.size > jpeg_buf.size()) {
            jpeg_buf.resize(vc.size + 64 * 1024);
        }
        if (fseek(f, (long)vc.offset, SEEK_SET) != 0 || fread(jpeg_buf.data(), 1, vc.size, f) != vc.size) {
            break;
        }

        // Decode to RGB565 (reuse existing jpeg_to_image wrapper).
        uint8_t* rgb = nullptr;
        size_t rgb_len = 0, w = 0, h = 0, stride = 0;
        if (jpeg_to_image(jpeg_buf.data(), vc.size, &rgb, &rgb_len, &w, &h, &stride) == ESP_OK && rgb != nullptr && w > 0 && h > 0) {
            // jpeg_to_image outputs RGB565_LE (little-endian); the panel was
            // configured with swap_bytes=1 (LVGL pushes big-endian), so swap
            // bytes before drawing or colors come out swapped.
            uint8_t* p = rgb;
            for (size_t i = 0; i < rgb_len; i += 2) {
                uint8_t t = p[i];
                p[i] = p[i + 1];
                p[i + 1] = t;
            }
            // Draw centered.
            int x = (LCD_W - (int)w) / 2;
            int y = (LCD_H - (int)h) / 2;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            esp_lcd_panel_draw_bitmap(VideoPlayerApp::s_panel, x, y, x + (int)w, y + (int)h, rgb);
            frame_count++;
            heap_caps_free(rgb);
        }

        // Audio: feed as many PCM chunks as fit in this video frame period.
        if (info->has_audio && audio_idx < info->audio_chunk_count) {
            const auto& ac = info->audio_chunks[audio_idx];
            if (ac.size > pcm_buf.size() * 2) {
                pcm_buf.resize(ac.size / 2 + 4096);
            }
            if (fseek(f, (long)ac.offset, SEEK_SET) == 0 && fread(pcm_buf.data(), 1, ac.size, f) == ac.size) {
                size_t samples = ac.size / 2;
                std::vector<int16_t> mono(pcm_buf.data(), pcm_buf.data() + samples);
                codec->OutputData(mono);
            }
            audio_idx++;
        }

        // FPS log every ~2s.
        int64_t now = esp_timer_get_time();
        if (now - fps_log_us >= 2000000) {
            ESP_LOGI(TAG, "AVI: %u frames, %.1f fps", frame_count,
                     1000000.0f * frame_count / (float)(now - fps_log_us));
            fps_log_us = now;
            frame_count = 0;
        }

        // Pace to frame rate (audio OutputData already paces when present).
        if (!info->has_audio) {
            next_frame_us += frame_period_us;
            int64_t wait = next_frame_us - esp_timer_get_time();
            if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        }
        video_idx++;
    }

    codec->EnableOutput(false);
    return true;
}

static void PlaybackTask(void* arg) {
    auto* self = static_cast<VideoPlayerApp*>(arg);
    (void)self;

    if (VideoPlayerApp::s_panel == nullptr) {
        ESP_LOGE(TAG, "Panel not set");
        vTaskDelete(NULL);
        return;
    }

    // Black background for letterboxing.
    std::vector<uint16_t> black(LCD_W * LCD_H, 0x0000);
    esp_lcd_panel_draw_bitmap(VideoPlayerApp::s_panel, 0, 0, LCD_W, LCD_H, black.data());

    DIR* dir = opendir("/sdcard/avi");
    if (dir == nullptr) {
        mkdir("/sdcard/avi", 0755);
        SetStatusText("No /sdcard/avi");
        ESP_LOGE(TAG, "No /sdcard/avi dir");
        vTaskDelete(NULL);
        return;
    }

    std::vector<std::string> files;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".avi") == 0) {
            files.push_back("/sdcard/avi/" + name);
        }
    }
    closedir(dir);
    if (files.empty()) {
        SetStatusText("No AVI files");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Found %d AVI files", (int)files.size());

    // Loop playback: play all files, then restart from the first.
    while (s_playing) {
        for (const auto& path : files) {
            if (!s_playing) break;
            SetStatusText(("Playing: " + path.substr(11)).c_str());
            ESP_LOGI(TAG, "Playing %s", path.c_str());
            FILE* f = fopen(path.c_str(), "rb");
            if (f == nullptr) continue;
            avi_info_t info;
            if (avi_parse(f, &info)) {
                PlayOneAvi(f, &info);
                ESP_LOGI(TAG, "Done: %s (%u frames, %u audio chunks)",
                         path.c_str(), info.video_frame_count, info.audio_chunk_count);
                avi_free(&info);
            } else {
                ESP_LOGE(TAG, "Not a valid AVI: %s", path.c_str());
            }
            fclose(f);
        }
    }

    // Restore the panel to black before handing control back to LVGL.
    esp_lcd_panel_draw_bitmap(VideoPlayerApp::s_panel, 0, 0, LCD_W, LCD_H, black.data());
    vTaskDelete(NULL);
}

bool VideoPlayerApp::run()
{
    ESP_LOGI(TAG, "run() - video player");

    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    s_status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_center(s_status_label);
    lv_label_set_text(s_status_label, "Loading AVI...");

    // Suspend LVGL rendering: we push frames straight to the panel via DMA.
    extern esp_err_t lvgl_port_suspend(void);
    lvgl_port_suspend();
    Application::GetInstance().GetAudioService().Stop();

    s_playing = true;
    xTaskCreatePinnedToCore(PlaybackTask, "avi_player", 16384, this, 3, nullptr, 1);
    return true;
}

bool VideoPlayerApp::back()
{
    s_playing = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    extern esp_err_t lvgl_port_resume(void);
    lvgl_port_resume();
    Application::GetInstance().GetAudioService().Start();
    s_status_label = nullptr;
    return true;
}
