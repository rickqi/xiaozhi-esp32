#pragma once

#include "esp_brookesia.hpp"

class MonthlyCatApp : public esp_brookesia::systems::phone::App {
public:
    MonthlyCatApp();

    bool run() override;
    bool back() override;

private:
    lv_obj_t* image_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;
    int cur_frame_ = 0;
    int gif_start_ = 0;
    int gif_end_ = 27;

    void NextFrame();
};
