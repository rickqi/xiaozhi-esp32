#pragma once

#include "esp_brookesia.hpp"
#include "esp_lcd_types.h"

class VideoPlayerApp : public esp_brookesia::systems::phone::App {
public:
    VideoPlayerApp();

    static void SetPanelHandle(esp_lcd_panel_handle_t panel) { s_panel = panel; }
    static esp_lcd_panel_handle_t s_panel;

    bool run() override;
    bool back() override;

private:
};
