#pragma once
#include "esp_err.h"

// Bring up WiFi STA (routed to the ESP32-C6 over esp-hosted) and block until
// we have an IP. Returns ESP_OK on success, ESP_FAIL after retries are exhausted.
esp_err_t wifi_connect(const char *ssid, const char *pass);
