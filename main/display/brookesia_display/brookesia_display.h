#pragma once

#include "display/lvgl_display/lvgl_display.h"
#include "esp_brookesia.hpp"
#include <lvgl.h>
#include "esp_lcd_panel_io.h"

namespace esp_brookesia::systems::phone { class Phone; }
class XiaoZhiApp;

class BrookesiaDisplay : public LvglDisplay {
public:
    BrookesiaDisplay(lv_display_t* lv_disp, esp_lcd_panel_io_handle_t panel_io,
                     int width, int height);
    ~BrookesiaDisplay() override;

    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    void SetBluetoothIcon(const char* icon) override;

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    esp_lcd_panel_io_handle_t panel_io_;
    esp_brookesia::systems::phone::Phone* phone_ = nullptr;
    XiaoZhiApp* xiaozhi_app_ = nullptr;

    void CreatePhoneShell();
    void UpdateClock();
};
