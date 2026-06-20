idf_component_register(
    SRCS "main.c" "wifi.c" "tesla_client.c" "display.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp-tls json nvs_flash esp_wifi esp_netif esp_event esp_timer
             esp_lcd driver esp_lcd_st7701 esp_lvgl_port lvgl
)
