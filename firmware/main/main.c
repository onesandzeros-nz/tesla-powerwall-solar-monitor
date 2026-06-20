#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "tesla_client.h"
#include "display.h"
#include "ui.h"
#include "secrets.h"

static const char *TAG = "main";

#define POLL_INTERVAL_MS 20000   // Fleet realtime limit is 60/min; 20s is gentle

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Display first so we can show status while the network comes up.
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    ui_init();

    ui_set_status("Connecting to Wi-Fi...", false);
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — check SSID/password and the C6 esp-hosted link");
        ui_set_status("Wi-Fi failed - check credentials", true);
        return;
    }

    // Non-fatal: a transient network/TLS hiccup at boot shouldn't kill the app.
    // The poll loop refreshes the token on demand and just retries on failure.
    ui_set_status("Connecting to Tesla...", false);
    if (tesla_client_init() != ESP_OK) {
        ESP_LOGW(TAG, "initial Tesla auth failed (network/token?) — will keep retrying");
        ui_set_status("Tesla connection failed - retrying", true);
    }

    ESP_LOGI(TAG, "polling live_status every %d s", POLL_INTERVAL_MS / 1000);
    int fails = 0;
    while (1) {
        tesla_live_status_t s;
        if (tesla_fetch_live_status(&s) == ESP_OK) {
            fails = 0;
            ESP_LOGI(TAG,
                     "Solar %5.2f kW | Home %5.2f kW | Grid %+5.2f kW | Batt %+5.2f kW | %3.0f%%",
                     s.solar_power / 1000.0, s.load_power / 1000.0,
                     s.grid_power / 1000.0, s.battery_power / 1000.0,
                     s.percentage_charged);
            ui_update(&s);   // sets footer to "Live"
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        } else {
            // Back off on errors (capped), show it on screen, then retry.
            if (fails < 6) fails++;
            ESP_LOGW(TAG, "fetch failed (%d) — retrying", fails);
            ui_set_status("No data - reconnecting...", true);
            vTaskDelay(pdMS_TO_TICKS(fails * 2000));
        }
    }
}
