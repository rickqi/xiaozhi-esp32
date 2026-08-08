#include "fluidbox_app.h"
#include "application.h"

extern "C" {
#include "config.h"
#include "fb_display.h"
#include "render.h"
#include "sim.h"
#include "imu.h"
}

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

#define TAG "FluidBoxApp"
#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_15
#define I2C_SCL GPIO_NUM_14

esp_lcd_panel_handle_t FluidBoxApp::s_panel = nullptr;
i2c_master_bus_handle_t FluidBoxApp::s_i2c = nullptr;

static TaskHandle_t s_sim_task = nullptr;
static TaskHandle_t s_render_task = nullptr;
static volatile bool s_fluid_running = false;

static i2c_master_bus_handle_t s_i2c_bus;

static void sim_task_func(void* arg)
{
    sim_forces_t forces = {
        .gravity = {0.0f, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0.0f},
        .omega = {0.0f, 0.0f, 0.0f},
        .alpha = {0.0f, 0.0f, 0.0f},
    };
    int64_t last_us = esp_timer_get_time();

    while (s_fluid_running) {
        const int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) * 1e-6f;
        last_us = now;
        if (dt > 0.05f) dt = 0.05f;
        else if (dt < 1e-4f) dt = 1e-4f;

        imu_read(dt, &forces);
        sim_step(dt, &forces);
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

static void fluid_launcher_task(void* arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    auto* app = static_cast<FluidBoxApp*>(arg);
    app->StartFluid();
    vTaskDelete(NULL);
}

static void render_task_func(void* arg)
{
    while (s_fluid_running) {
        render_frame();
        // Cap at ~30 fps: QSPI 40MHz caps a full screen at ~48 fps, and the
        // render outruns the simulation (which publishes ~120 steps/s).
        vTaskDelay(pdMS_TO_TICKS(33));
    }
    vTaskDelete(NULL);
}

LV_IMG_DECLARE(esp_brookesia_image_large_app_launcher_default_112_112);

FluidBoxApp::FluidBoxApp()
    : esp_brookesia::systems::phone::App(
          "FluidBox", &esp_brookesia_image_large_app_launcher_default_112_112, true, true, false) {}

bool FluidBoxApp::run()
{
    ESP_LOGI(TAG, "FluidBox app run");
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_scr_load(scr);
    xTaskCreate(fluid_launcher_task, "fb_launcher", 4096, this, 5, nullptr);
    return true;
}

bool FluidBoxApp::back()
{
    ESP_LOGI(TAG, "FluidBox app back");
    StopFluid();
    return true;
}

bool FluidBoxApp::pause()
{
    StopFluid();
    return true;
}

bool FluidBoxApp::resume()
{
    return StartFluid();
}

bool FluidBoxApp::close()
{
    StopFluid();
    return true;
}

bool FluidBoxApp::StartFluid()
{
    ESP_LOGI(TAG, "StartFluid: begin");
    if (s_fluid_running) {
        return true;
    }
    if (s_panel == nullptr) {
        ESP_LOGE(TAG, "Panel handle not set");
        return false;
    }
    if (fb_panel_set_handle(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set panel handle");
        return false;
    }
    ESP_LOGI(TAG, "StartFluid: panel ok");

    i2c_master_bus_handle_t imu_bus = s_i2c;
    if (imu_bus == nullptr) {
        i2c_master_bus_config_t i2c_cfg = {};
        i2c_cfg.i2c_port = I2C_PORT;
        i2c_cfg.sda_io_num = I2C_SDA;
        i2c_cfg.scl_io_num = I2C_SCL;
        i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_cfg.glitch_ignore_cnt = 7;
        i2c_cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&i2c_cfg, &imu_bus) != ESP_OK) {
            ESP_LOGW(TAG, "I2C bus init failed");
        }
    }
    if (imu_init(imu_bus) != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without motion input");
    }
    ESP_LOGI(TAG, "StartFluid: imu done");

    extern esp_err_t lvgl_port_suspend(void);
    lvgl_port_suspend();
    Application::GetInstance().GetAudioService().Stop();
    ESP_LOGI(TAG, "StartFluid: LVGL suspended, audio stopped");

    sim_init();
    ESP_LOGI(TAG, "StartFluid: sim_init done");
    render_init();
    ESP_LOGI(TAG, "StartFluid: render_init done");

    s_fluid_running = true;
    ESP_LOGI(TAG, "StartFluid: creating tasks");
    xTaskCreatePinnedToCore(sim_task_func, "fb_sim", 16384, NULL, 5, &s_sim_task, 1);
    xTaskCreatePinnedToCore(render_task_func, "fb_render", 8192, NULL, 5, &s_render_task, 0);

    ESP_LOGI(TAG, "FluidBox started, internal free=%u psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void FluidBoxApp::StopFluid()
{
    if (!s_fluid_running) {
        return;
    }
    s_fluid_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_sim_task) {
        vTaskDelete(s_sim_task);
        s_sim_task = nullptr;
    }
    if (s_render_task) {
        vTaskDelete(s_render_task);
        s_render_task = nullptr;
    }

    extern esp_err_t lvgl_port_resume(void);
    lvgl_port_resume();
    Application::GetInstance().GetAudioService().Start();
    ESP_LOGI(TAG, "FluidBox stopped, LVGL + audio resumed");
}

void FluidBoxApp::CreateInfoLabel(lv_obj_t* screen)
{
    (void)screen;
}
