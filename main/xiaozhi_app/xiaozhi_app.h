#pragma once

#include "esp_brookesia.hpp"
#include <lvgl.h>
#include <string>

class XiaoZhiApp : public esp_brookesia::systems::phone::App {
public:
    XiaoZhiApp();

    bool run() override;
    bool back() override;

    void SetChatMessage(const char* role, const char* content);
    void SetEmotionImage(const void* src);
    void SetEmotionText(const char* text);
    void SetStatus(const char* status);
    void ShowNotification(const char* msg, int duration_ms);

private:
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_container_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_timer_t* notification_timer_ = nullptr;

    std::string pending_status_;
    std::string pending_chat_role_;
    std::string pending_chat_content_;

    static constexpr int MAX_BUBBLES = 15;

    void AppendChatBubble(const char* role, const char* content);
};
