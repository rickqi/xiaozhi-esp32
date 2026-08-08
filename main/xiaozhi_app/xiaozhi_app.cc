#include "xiaozhi_app.h"
#include "esp_lvgl_port.h"
#include "lvgl_display/lvgl_theme.h"
#include <cstring>
#include <esp_log.h>

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_30_4);
LV_FONT_DECLARE(font_awesome_30_4);
LV_IMG_DECLARE(app_icon_xiaozhi_112_112);

#define TAG "XiaoZhiApp"

static const lv_font_t* GetChatFont() {
    auto theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (theme != nullptr && theme->text_font() != nullptr) {
        const lv_font_t* f = theme->text_font()->font();
        const auto* dsc = static_cast<const lv_font_fmt_txt_dsc_t*>(f->dsc);
        ESP_LOGI(TAG, "ChatFont: lh=%d bl=%d dsc=%p cmap_num=%u bpp=%u",
                 f->line_height, f->base_line, f->dsc,
                 dsc ? (unsigned)dsc->cmap_num : 0, dsc ? (unsigned)dsc->bpp : 0);
        return f;
    }
    ESP_LOGI(TAG, "ChatFont: fallback basic");
    return &font_puhui_basic_30_4;
}

XiaoZhiApp::XiaoZhiApp()
    : esp_brookesia::systems::phone::App(
          "XiaoZhi", &app_icon_xiaozhi_112_112, true, true, false) {}

bool XiaoZhiApp::run() {
    ESP_LOGI(TAG, "run() called - creating UI");
    lv_obj_t* screen = lv_scr_act();

    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 6, 0);
    // The system status bar (time/WiFi) is a top-layer overlay ~50px tall:
    // keep the app content below it or it overlaps.
    lv_obj_set_style_pad_top(container, 50, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    emotion_container_ = lv_obj_create(container);
    lv_obj_set_height(emotion_container_, LV_SIZE_CONTENT);
    lv_obj_set_width(emotion_container_, LV_PCT(100));
    lv_obj_set_flex_align(emotion_container_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(emotion_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(emotion_container_, 0, 0);
    lv_obj_clear_flag(emotion_container_, LV_OBJ_FLAG_SCROLLABLE);

    emotion_image_ = lv_image_create(emotion_container_);
    lv_obj_add_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);

    emotion_label_ = lv_label_create(emotion_container_);
    // The emotion text is a font-awesome symbol (e.g. LV_SYMBOL_AUDIO), so it
    // needs the icon font: the text fonts carry no PUA glyphs and would render
    // the title as a tofu box.
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, lv_color_hex(0x00CED1), 0);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(emotion_label_, LV_SYMBOL_AUDIO);

    chat_container_ = lv_obj_create(container);
    lv_obj_set_flex_grow(chat_container_, 1);
    lv_obj_set_width(chat_container_, LV_PCT(100));
    lv_obj_set_flex_flow(chat_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(chat_container_, 4, 0);
    lv_obj_set_style_bg_opa(chat_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_container_, 0, 0);

    status_label_ = lv_label_create(container);
    lv_obj_set_height(status_label_, LV_SIZE_CONTENT);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(status_label_, "");
    // Status text can carry CJK (e.g. "正在播放: 中文歌名.mp3"), so use the
    // full-CJK chat font rather than the ~474-glyph basic font.
    lv_obj_set_style_text_font(status_label_, GetChatFont(), 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x888888), 0);
    // The AMOLED has rounded corners (~30px radius): center the status text
    // and keep it off the corner so it is not clipped.
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(status_label_, 30, 0);
    lv_obj_set_style_pad_right(status_label_, 30, 0);

    notification_label_ = lv_label_create(screen);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(notification_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(notification_label_, LV_OPA_80, 0);
    // Notifications carry CJK too (e.g. "正在播放: 中文歌名.mp3").
    lv_obj_set_style_text_font(notification_label_, GetChatFont(), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(notification_label_, 10, 0);
    lv_obj_set_style_radius(notification_label_, 8, 0);
    lv_obj_align(notification_label_, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notification_label_, LV_PCT(85));

    if (!pending_status_.empty()) {
        lv_label_set_text(status_label_, pending_status_.c_str());
    }
    if (!pending_chat_content_.empty()) {
        AppendChatBubble(pending_chat_role_.c_str(), pending_chat_content_.c_str());
    }

    return true;
}

bool XiaoZhiApp::back() {
    return true;
}

void XiaoZhiApp::SetChatMessage(const char* role, const char* content) {
    if (chat_container_ == nullptr) {
        pending_chat_role_ = role ? role : "";
        pending_chat_content_ = content ? content : "";
        return;
    }
    AppendChatBubble(role, content);
}

void XiaoZhiApp::AppendChatBubble(const char* role, const char* content) {
    if (!content || !content[0]) return;

    lv_obj_t* bubble = lv_obj_create(chat_container_);
    lv_obj_set_width(bubble, LV_PCT(88));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_set_style_radius(bubble, 10, 0);

    uint32_t bg_color = 0x4CAF50;
    if (role && strcmp(role, "user") == 0) bg_color = 0x2196F3;
    else if (role && strcmp(role, "system") == 0) bg_color = 0x607D8B;

    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg_color), 0);
    lv_obj_set_style_border_width(bubble, 0, 0);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, GetChatFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, content);

    int count = lv_obj_get_child_count(chat_container_);
    while (count > MAX_BUBBLES) {
        lv_obj_del(lv_obj_get_child(chat_container_, 0));
        count--;
    }
    // Scroll instantly, not with an animation: while the LV_ANIM_ON scroll is
    // still running the container's layout/offset is unstable, so the first
    // paint of a new bubble lands at the wrong rows and leaves black lines
    // until a later repaint lands on the settled layout.
    lv_obj_scroll_to_y(chat_container_, LV_COORD_MAX, LV_ANIM_OFF);

    // Belt and braces: re-invalidate the new bubble once layout has fully
    // settled so any first-paint gaps are repainted.
    lv_obj_t* new_bubble = lv_obj_get_child(chat_container_, count - 1);
    lv_timer_t* inv_timer = lv_timer_create([](lv_timer_t* timer) {
        lv_obj_invalidate(static_cast<lv_obj_t*>(timer->user_data));
        lv_timer_del(timer);
    }, 30, new_bubble);
    lv_timer_set_repeat_count(inv_timer, 1);
}

void XiaoZhiApp::SetEmotionImage(const void* src) {
    if (emotion_image_ == nullptr) return;
    lv_image_set_src(emotion_image_, src);
    lv_obj_clear_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
    if (emotion_label_) lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
}

void XiaoZhiApp::SetEmotionText(const char* text) {
    if (emotion_label_ == nullptr) return;
    lv_label_set_text(emotion_label_, text ? text : LV_SYMBOL_AUDIO);
    lv_obj_clear_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
    if (emotion_image_) lv_obj_add_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
}

void XiaoZhiApp::SetStatus(const char* status) {
    if (status_label_ == nullptr) {
        pending_status_ = status ? status : "";
        return;
    }
    lv_label_set_text(status_label_, status ? status : "");
}

void XiaoZhiApp::ShowNotification(const char* msg, int duration_ms) {
    if (notification_label_ == nullptr) return;

    lv_label_set_text(notification_label_, msg ? msg : "");
    lv_obj_clear_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    if (notification_timer_) {
        lv_timer_del(notification_timer_);
    }
    auto cb = [](lv_timer_t* t) {
        auto* app = static_cast<XiaoZhiApp*>(t->user_data);
        if (app->notification_label_) {
            lv_obj_add_flag(app->notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_timer_del(t);
        app->notification_timer_ = nullptr;
    };
    notification_timer_ = lv_timer_create(cb, duration_ms, this);
    lv_timer_set_repeat_count(notification_timer_, 1);
}
