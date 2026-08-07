#include "brookesia_display.h"
#include "xiaozhi_app/xiaozhi_app.h"
#include "board.h"
#include "esp_lvgl_port.h"
#include <esp_log.h>
#include <time.h>

#include "stylesheet/stylesheet.hpp"

#define TAG "BrookesiaDisplay"

using namespace esp_brookesia::systems::phone;

BrookesiaDisplay::BrookesiaDisplay(lv_display_t* lv_disp,
                                    esp_lcd_panel_io_handle_t panel_io,
                                    int width, int height)
    : lv_display_(lv_disp), panel_io_(panel_io) {
    width_ = width;
    height_ = height;

    esp_brookesia::gui::LvLock::registerCallbacks(
        [](int timeout_ms) -> bool {
            if (timeout_ms < 0) timeout_ms = 0;
            else if (timeout_ms == 0) timeout_ms = 1;
            return lvgl_port_lock(timeout_ms);
        },
        []() -> bool {
            lvgl_port_unlock();
            return true;
        }
    );

    CreatePhoneShell();
}

BrookesiaDisplay::~BrookesiaDisplay() {
    delete phone_;
    delete xiaozhi_app_;
}

void BrookesiaDisplay::CreatePhoneShell() {
    DisplayLockGuard lock(this);

    phone_ = new Phone();

    Stylesheet* stylesheet = new Stylesheet(STYLESHEET_410_502_DARK);
    if (width_ == 410 && height_ == 502) {
        phone_->addStylesheet(*stylesheet);
        phone_->activateStylesheet(*stylesheet);
    }
    delete stylesheet;

    if (!phone_->begin()) {
        ESP_LOGE(TAG, "Phone begin failed");
        return;
    }

    xiaozhi_app_ = new XiaoZhiApp();
    int app_id = phone_->installApp(*xiaozhi_app_);
    if (app_id < 0) {
        ESP_LOGE(TAG, "Install XiaoZhiApp failed");
    }

    lv_timer_create([](lv_timer_t* t) {
        auto* self = static_cast<BrookesiaDisplay*>(t->user_data);
        self->UpdateClock();
    }, 1000, this);
}

bool BrookesiaDisplay::Lock(int timeout_ms) {
    if (timeout_ms == 0) timeout_ms = 1;
    return lvgl_port_lock(timeout_ms);
}

void BrookesiaDisplay::Unlock() {
    lvgl_port_unlock();
}

void BrookesiaDisplay::SetStatus(const char* status) {
    if (xiaozhi_app_) {
        DisplayLockGuard lock(this);
        xiaozhi_app_->SetStatus(status);
    }
}

void BrookesiaDisplay::SetChatMessage(const char* role, const char* content) {
    if (xiaozhi_app_) {
        DisplayLockGuard lock(this);
        xiaozhi_app_->SetChatMessage(role, content);
    }
}

void BrookesiaDisplay::SetEmotion(const char* emotion) {
    if (xiaozhi_app_) {
        DisplayLockGuard lock(this);
        xiaozhi_app_->SetEmotionText(emotion);
    }
}

void BrookesiaDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (xiaozhi_app_) {
        DisplayLockGuard lock(this);
        xiaozhi_app_->ShowNotification(notification, duration_ms);
    }
}

void BrookesiaDisplay::SetTheme(Theme* theme) {
    current_theme_ = theme;
}

void BrookesiaDisplay::UpdateStatusBar(bool update_all) {
    if (!phone_) return;
    DisplayLockGuard lock(this);

    auto& board = Board::GetInstance();

    auto* status_bar = phone_->getDisplay().getStatusBar();

    int level; bool charging, discharging;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        status_bar->setBatteryPercent(charging, level);
    }

    const char* network_icon = board.GetNetworkStateIcon();
    (void)network_icon;
}

void BrookesiaDisplay::SetPowerSaveMode(bool on) {
    auto& board = Board::GetInstance();
    if (on) {
        board.GetBacklight()->SetBrightness(20);
    } else {
        board.GetBacklight()->RestoreBrightness();
    }
}

void BrookesiaDisplay::SetBluetoothIcon(const char* icon) {
}

void BrookesiaDisplay::UpdateClock() {
    if (!phone_) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    phone_->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min);
}
