#pragma once
#include <stdbool.h>
#include "tesla_client.h"

// Build the dashboard screen. Call once after display_init().
void ui_init(void);

// Push the latest figures to the screen. Thread-safe (takes the LVGL lock).
void ui_update(const tesla_live_status_t *s);

// Show a status/error message in the footer (e.g. "Connecting to Wi-Fi...").
// error=true colours it red. Thread-safe.
void ui_set_status(const char *msg, bool error);
