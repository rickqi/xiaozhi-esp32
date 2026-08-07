#include "brookesia_display.h"
#include "xiaozhi_app/xiaozhi_app.h"
#include "board.h"
#include "esp_lvgl_port.h"
#include <esp_log.h>
#include <time.h>
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/emoji_collection.h"
#include "display/lvgl_display/lvgl_font.h"
#include <font_awesome.h>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

#include "stylesheet/stylesheet.hpp"

#define TAG "BrookesiaDisplay"

using namespace esp_brookesia::systems::phone;

static void InitBrookesiaThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x1A1A2E));
    dark_theme->set_text_color(lv_color_hex(0xE0E0E0));
    dark_theme->set_chat_background_color(lv_color_hex(0x16213E));
    dark_theme->set_user_bubble_color(lv_color_hex(0x2196F3));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x4CAF50));
    dark_theme->set_system_bubble_color(lv_color_hex(0x607D8B));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0x333333));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF6B6B));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(icon_font);
    dark_theme->set_emoji_collection(std::make_shared<Twemoji64>());

    LvglThemeManager::GetInstance().RegisterTheme("dark", dark_theme);
}

BrookesiaDisplay::BrookesiaDisplay(lv_display_t* lv_disp,
                                    esp_lcd_panel_io_handle_t panel_io,
                                    int width, int height)
    : panel_io_(panel_io) {
    display_ = lv_disp;
    width_ = width;
    height_ = height;

    InitBrookesiaThemes();
    current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");

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
    ESP_LOGI(TAG, "CreatePhoneShell: full (with stylesheet)");
    DisplayLockGuard lock(this);

    phone_ = new Phone();
    ESP_LOGI(TAG, "Phone created");

    Stylesheet* stylesheet = new Stylesheet(STYLESHEET_410_502_DARK);
    if (width_ == 410 && height_ == 502) {
        phone_->addStylesheet(*stylesheet);
        phone_->activateStylesheet(*stylesheet);
    }
    ESP_LOGI(TAG, "Stylesheet activated");
    delete stylesheet;

    if (!phone_->begin()) {
        ESP_LOGE(TAG, "Phone begin failed");
        return;
    }
    ESP_LOGI(TAG, "Phone begin OK");

    xiaozhi_app_ = new XiaoZhiApp();
    int app_id = phone_->installApp(*xiaozhi_app_);
    ESP_LOGI(TAG, "App installed id=%d", app_id);

    // FluidBoxApp installation temporarily disabled for debugging
    // auto* fluidbox_app = new FluidBoxApp();
    // int fluid_id = phone_->installApp(*fluidbox_app);
    // ESP_LOGI(TAG, "FluidBox app installed id=%d", fluid_id);

    lv_timer_create([](lv_timer_t* t) {
        auto* self = static_cast<BrookesiaDisplay*>(t->user_data);
        self->UpdateClock();
        self->UpdateStatusBar(true);
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
    ESP_LOGI(TAG, "SetChatMessage: '%s' acquiring lock", role ? role : "null");
    if (xiaozhi_app_) {
        DisplayLockGuard lock(this);
        ESP_LOGI(TAG, "SetChatMessage: lock acquired");
        xiaozhi_app_->SetChatMessage(role, content);
    }
    ESP_LOGI(TAG, "SetChatMessage: done");
}

void BrookesiaDisplay::SetEmotion(const char* emotion) {
    if (!xiaozhi_app_) return;
    DisplayLockGuard lock(this);

    auto* theme = dynamic_cast<LvglTheme*>(current_theme_);
    if (theme && theme->emoji_collection()) {
        auto* img = theme->emoji_collection()->GetEmojiImage(emotion);
        if (img && img->image_dsc()) {
            xiaozhi_app_->SetEmotionImage(img->image_dsc());
            return;
        }
    }

    const char* icon = font_awesome_get_utf8(emotion);
    xiaozhi_app_->SetEmotionText(icon ? icon : LV_SYMBOL_AUDIO);
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
    auto& phone_display = phone_->getDisplay();
    auto* status_bar = phone_display.getStatusBar();
    if (!status_bar) return;

    int level; bool charging, discharging;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        status_bar->setBatteryPercent(charging, level);
    }

    const char* icon = board.GetNetworkStateIcon();
    using WifiState = esp_brookesia::systems::phone::StatusBar::WifiState;
    if (icon && strcmp(icon, FONT_AWESOME_WIFI_SLASH) == 0) {
        status_bar->setWifiIconState(WifiState::DISCONNECTED);
    } else if (icon && strcmp(icon, FONT_AWESOME_WIFI_WEAK) == 0) {
        status_bar->setWifiIconState(WifiState::SIGNAL_1);
    } else if (icon && strcmp(icon, FONT_AWESOME_WIFI_FAIR) == 0) {
        status_bar->setWifiIconState(WifiState::SIGNAL_2);
    } else {
        status_bar->setWifiIconState(WifiState::SIGNAL_3);
    }
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
