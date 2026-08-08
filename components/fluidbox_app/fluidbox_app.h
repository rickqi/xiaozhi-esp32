#pragma once

#include "esp_brookesia.hpp"
#include "esp_lcd_types.h"
#include "driver/i2c_master.h"

namespace esp_brookesia::systems::phone { class Phone; }

class FluidBoxApp : public esp_brookesia::systems::phone::App {
public:
    FluidBoxApp();

    bool run() override;
    bool back() override;
    bool pause() override;
    bool resume() override;
    bool close() override;

    static void SetPanelHandle(esp_lcd_panel_handle_t panel) { s_panel = panel; }
    static void SetI2cBus(i2c_master_bus_handle_t bus) { s_i2c = bus; }
    static i2c_master_bus_handle_t GetI2cBus() { return s_i2c; }
    static bool IsRunning();

    /// Signal FluidBox to exit back to Phone Shell home.
    /// Safe to call from any context (BLE callback, timer, etc.).
    /// The touch-monitor task picks this up on its next poll cycle (~50 ms).
    static void RequestExit();

    bool StartFluid();
    void StopFluid();

private:
    static esp_lcd_panel_handle_t s_panel;
    static i2c_master_bus_handle_t s_i2c;

    void CreateInfoLabel(lv_obj_t* screen);
};
