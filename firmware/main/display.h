#pragma once
#include "esp_err.h"

// Bring up the ST7701S MIPI-DSI panel + backlight and start esp_lvgl_port.
// After this returns ESP_OK, build the UI under lvgl_port_lock()/unlock().
esp_err_t display_init(void);
