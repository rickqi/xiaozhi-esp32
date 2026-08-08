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

#define FT5X06_I2C_ADDR     0x38
#define FT5X06_REG_TD_STATUS 0x02
#define TOUCH_LONG_PRESS_US  800000
#define TOUCH_POLL_MS        50

esp_lcd_panel_handle_t FluidBoxApp::s_panel = nullptr;
i2c_master_bus_handle_t FluidBoxApp::s_i2c = nullptr;

static TaskHandle_t s_sim_task = nullptr;
static TaskHandle_t s_render_task = nullptr;
static TaskHandle_t s_touch_task = nullptr;
static volatile bool s_fluid_running = false;
static volatile bool s_exit_requested = false;
static lv_obj_t* s_fluid_screen = nullptr;

// ---------------------------------------------------------------------------
// FT5x06 raw register read (bypasses esp_lcd_touch — which is dormant while
// LVGL is suspended).  We create a transient I2C device on the shared bus,
// poll the touch-status register, and parse point 1 coordinates.
// ---------------------------------------------------------------------------

static i2c_master_dev_handle_t s_touch_dev = nullptr;

static bool touch_read_point(uint16_t* x, uint16_t* y)
{
    if (!s_touch_dev) return false;

    uint8_t reg = FT5X06_REG_TD_STATUS;
    uint8_t buf[5];
    i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, sizeof(buf), -1);

    if ((buf[0] & 0x0F) == 0) return false;

    *x = ((buf[1] & 0x0F) << 8) | buf[2];
    *y = ((buf[3] & 0x0F) << 8) | buf[4];
    return true;
}

// ---------------------------------------------------------------------------
// Async callback: runs inside the LVGL task after lvgl_port_resume().
// Navigates the Phone Shell back to HOME and deletes the fluid screen.
// ---------------------------------------------------------------------------

static void exit_async_cb(void* user_data)
{
    auto* app = static_cast<FluidBoxApp*>(user_data);
    auto* phone = app->getSystem();
    if (phone) {
        phone->sendNavigateEvent(
            esp_brookesia::systems::base::Manager::NavigateType::HOME);
    }
    if (s_fluid_screen) {
        lv_obj_del(s_fluid_screen);
        s_fluid_screen = nullptr;
    }
    ESP_LOGI(TAG, "Phone Shell HOME navigation + screen cleanup done");
}

/// Full exit sequence invoked from the touch-monitor task when an exit
/// gesture (long-press) or RequestExit() is detected.  Stops fluid, resumes
/// LVGL, then schedules Phone Shell HOME via lv_async_call so it runs in
/// the correct (LVGL task) context.
static void perform_fluid_exit(FluidBoxApp* app)
{
    ESP_LOGI(TAG, "Exit requested — stopping FluidBox");

    // 1. Stop sim + render, resume LVGL + audio
    app->StopFluid();

    // 2. Release touch I2C device (bus returns to LVGL touch driver)
    if (s_touch_dev) {
        i2c_master_bus_rm_device(s_touch_dev);
        s_touch_dev = nullptr;
    }

    // 3. Schedule Phone Shell HOME in LVGL task context
    if (lvgl_port_lock(2000)) {
        lv_async_call(exit_async_cb, app);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "LVGL lock timeout — deleting screen without HOME nav");
        if (lvgl_port_lock(1000)) {
            if (s_fluid_screen) {
                lv_obj_del(s_fluid_screen);
                s_fluid_screen = nullptr;
            }
            lvgl_port_unlock();
        }
    }
}

// ---------------------------------------------------------------------------
// Touch-monitor task: created in StartFluid(), polls FT5x06 every 50 ms
// while LVGL is suspended.  Exits on long-press (>800 ms) or RequestExit().
// ---------------------------------------------------------------------------

static void touch_monitor_task_func(void* arg)
{
    auto* app = static_cast<FluidBoxApp*>(arg);

    // Create a transient I2C device for FT5x06 on the shared bus
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = FT5X06_I2C_ADDR;
    dev_cfg.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(FluidBoxApp::GetI2cBus(), &dev_cfg, &s_touch_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add FT5x06 I2C device for touch polling");
        s_touch_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    uint16_t x = 0, y = 0;
    int64_t press_start_us = 0;
    bool was_pressed = false;

    while (s_fluid_running) {
        if (s_exit_requested) {
            s_exit_requested = false;
            break;
        }

        bool pressed = touch_read_point(&x, &y);

        if (pressed && !was_pressed) {
            press_start_us = esp_timer_get_time();
        }

        if (pressed && (esp_timer_get_time() - press_start_us > TOUCH_LONG_PRESS_US)) {
            break;
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }

    perform_fluid_exit(app);

    s_touch_task = nullptr;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Simulation + render tasks (unchanged)
// ---------------------------------------------------------------------------

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
        vTaskDelay(pdMS_TO_TICKS(33));
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// FluidBoxApp
// ---------------------------------------------------------------------------

LV_IMG_DECLARE(esp_brookesia_image_large_app_launcher_default_112_112);

FluidBoxApp::FluidBoxApp()
    : esp_brookesia::systems::phone::App(
          "FluidBox", &esp_brookesia_image_large_app_launcher_default_112_112, true, true, false) {}

bool FluidBoxApp::run()
{
    ESP_LOGI(TAG, "FluidBox app run");
    s_fluid_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_fluid_screen, lv_color_black(), 0);
    lv_scr_load(s_fluid_screen);
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

void FluidBoxApp::RequestExit()
{
    s_exit_requested = true;
}

bool FluidBoxApp::IsRunning()
{
    return s_fluid_running;
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

    s_exit_requested = false;
    s_fluid_running = true;
    ESP_LOGI(TAG, "StartFluid: creating tasks");
    xTaskCreatePinnedToCore(sim_task_func, "fb_sim", 16384, NULL, 5, &s_sim_task, 1);
    xTaskCreatePinnedToCore(render_task_func, "fb_render", 8192, NULL, 5, &s_render_task, 0);
    xTaskCreatePinnedToCore(touch_monitor_task_func, "fb_touch", 4096, this, 3, &s_touch_task, 0);

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
