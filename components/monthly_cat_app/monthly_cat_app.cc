#include "monthly_cat_app.h"
#include "monthly_cat_frames.h"

#include "esp_log.h"
#include "lvgl.h"

#define TAG "MonthlyCatApp"

LV_IMG_DECLARE(app_icon_salarycat_112_112);

MonthlyCatApp::MonthlyCatApp()
    : App("SalaryCat", &app_icon_salarycat_112_112,
          true, true, false) {}

bool MonthlyCatApp::run()
{
    ESP_LOGI(TAG, "run() - playing GIF1 (%d frames, 50ms)", monthly_cat_gifs[0].count);

    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    image_ = lv_image_create(screen);
    lv_image_set_src(image_, &monthly_cat_frames[0]);
    // I1 indexed images do not compose with lv_image_set_scale (per-line
    // decode vs zoom) - scaling blanks the screen. Show at native 1x centered.
    // A larger render would need lv_canvas with manual scale-up per frame.
    (void)lv_image_set_scale;
    lv_obj_center(image_);

    anim_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<MonthlyCatApp*>(t->user_data)->NextFrame();
        },
        50, this);
    return true;
}

bool MonthlyCatApp::back()
{
    if (anim_timer_ != nullptr) {
        lv_timer_del(anim_timer_);
        anim_timer_ = nullptr;
    }
    return true;
}

void MonthlyCatApp::NextFrame()
{
    if (image_ == nullptr) {
        return;
    }
    cur_frame_++;
    if (cur_frame_ > gif_end_) {
        cur_frame_ = gif_start_;
    }
    lv_image_set_src(image_, &monthly_cat_frames[cur_frame_]);
    lv_obj_invalidate(image_);
}
