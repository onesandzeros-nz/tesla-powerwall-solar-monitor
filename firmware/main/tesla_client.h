#pragma once
#include "esp_err.h"

// Live energy figures from the Fleet API live_status endpoint.
// Power values are watts: grid_power +import/-export, battery_power +discharge/-charge.
typedef struct {
    double solar_power;
    double battery_power;
    double grid_power;
    double load_power;
    double percentage_charged;   // 0..100
    double energy_left;          // Wh
    double total_pack_energy;    // Wh
    char   grid_status[24];      // e.g. "Active" (on-grid) or other
} tesla_live_status_t;

// Load the refresh token (NVS override or compiled-in) and fetch a first
// access token. Call once after WiFi is up.
esp_err_t tesla_client_init(void);

// Fetch the latest live status. Refreshes the access token automatically.
esp_err_t tesla_fetch_live_status(tesla_live_status_t *out);
