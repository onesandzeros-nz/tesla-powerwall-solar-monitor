# Tesla Powerwall Dashboard — ESP32-P4

A wall dashboard showing live solar / Powerwall / grid / home stats on a
**Guition JC4880P443C_I_W** (ESP32-P4 + ESP32-C6, 4.3" 480×800 MIPI-DSI, ST7701S).

Data comes from the **Tesla Fleet API** (cloud). The local Powerwall REST API
(`/api/meters/aggregates`) is **not available on Powerwall 3** — Tesla replaced
it with a Protobuf interface (TEDAPI). The Fleet API returns the same numbers as
clean JSON, so it's the practical path for a PW3.

## Architecture

```
                one-time, on your laptop + web server
  ┌────────────────────────────────────────────────────────────┐
  │ tools/tesla_setup.py: keygen → partner register → OAuth     │
  │  → refresh_token + energy_site_id  (saved to secrets/.env)  │
  └────────────────────────────────────────────────────────────┘
                              │ refresh_token, site_id baked into firmware
                              ▼
  ESP32-P4 runtime loop (firmware/):
    refresh_token ──POST auth.tesla.com/oauth2/v3/token──▶ access_token (cached ~8h)
    access_token  ──GET  …/energy_sites/{id}/live_status─▶ solar/battery/grid/load/%
    → LVGL renders the stats screen, repeat every ~15–30 s
```

## Status

- [x] Project scaffold + secret handling
- [x] One-time Fleet API setup tooling (`tools/tesla_setup.py`)
- [x] Firmware: WiFi (P4↔C6 esp-hosted) + Fleet fetch — **working on-device** ✅
- [x] Firmware: LVGL stats screen (ST7701S DSI) — **working on-device** ✅
- [x] Power bars (solar/home/grid), battery kWh + grid-status badge

## Part 1 — one-time Tesla Fleet API setup (do this first)

You need a domain you control to host a public key. You already have
**onesandzeros.nz**, so this is straightforward.

> ⚠️ **Rotate your client secret first.** The secret shared in chat should be
> considered compromised — generate a fresh one in the developer portal and use
> that below.

1. **Developer portal** (https://developer.tesla.com): in your app settings, make
   sure `onesandzeros.nz` is an allowed origin and add an **Allowed Redirect URI**
   matching `TESLA_REDIRECT_URI` (default `https://onesandzeros.nz/auth/tesla/callback`).

2. **Secrets file:**
   ```bash
   cp secrets/tesla_secrets.example.env secrets/tesla_secrets.env
   # edit it: client id, NEW secret, domain, redirect uri, audience (NA for NZ)
   pip install -r tools/requirements.txt
   ```

3. **Run the flow:**
   ```bash
   cd tools
   python3 tesla_setup.py genkey      # generates the key pair + prints where to host it
   #  → host secrets/public-key.pem at
   #    https://onesandzeros.nz/.well-known/appspecific/com.tesla.3p.public-key.pem
   python3 tesla_setup.py register    # registers your partner account
   python3 tesla_setup.py login       # opens an OAuth URL; paste the redirected URL back
   python3 tesla_setup.py site        # finds + saves your energy_site_id
   python3 tesla_setup.py test        # prints live_status — your real numbers
   ```

When `test` prints your live solar/battery/grid figures, the cloud side is done.
`secrets/tesla_secrets.env` now holds the `TESLA_REFRESH_TOKEN` and
`TESLA_ENERGY_SITE_ID` the firmware needs.

## Part 2 — firmware (data-first build)

ESP-IDF project under `firmware/`. This first version brings up WiFi and prints
live figures to the serial log — **no display yet** — so we can prove the cloud
path on the device before adding LVGL.

What it does on boot: connect WiFi (via the C6) → load refresh token (NVS or
compiled-in) → get an access token → poll `live_status` every 20 s → log
`Solar / Home / Grid / Batt / %`.

### Prerequisites

- **ESP-IDF v5.3+** (v5.4/5.5 recommended for the P4↔C6 esp-hosted WiFi).
  Install per Espressif's guide, then `. $HOME/esp/esp-idf/export.sh` each shell.
- The board's **ESP32-C6 must run esp-hosted slave firmware**. On these Guition
  boards the managed `esp_hosted` component can deploy it from P4 flash; if WiFi
  never connects, flashing the C6 with the matching slave firmware is the usual
  fix (see esp_hosted docs).

### Build, flash, watch

```bash
cd firmware
./gen_secrets.sh                 # writes main/secrets.h from ../secrets/tesla_secrets.env
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # adjust the port
```

Success looks like:

```
I (3xxx) wifi: got IP 192.168.x.x
I (4xxx) tesla: access token refreshed (valid ~28800 s)
I (5xxx) main: Solar  3.41 kW | Home  1.12 kW | Grid -0.31 kW | Batt +1.90 kW |  78%
```

### Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Stuck at `connecting to '…'`, repeated retries | C6 esp-hosted link / SDIO. Confirm the C6 slave firmware; check `menuconfig → Wi-Fi Remote` transport + SDIO pins for this board. |
| `token refresh HTTP 401/400` | Refresh token expired/revoked — re-run `tesla_setup.py login`, regenerate `secrets.h`. |
| `live_status HTTP 4xx` | Wrong `TESLA_ENERGY_SITE_ID` or `TESLA_AUDIENCE` region. |
| TLS / cert errors | Ensure the cert bundle options in `sdkconfig.defaults` stuck (check `menuconfig`). |

### The LVGL stats screen

`display.c` brings up the ST7701S panel (verbatim init sequence, 2-lane DSI @
500 Mbps, 34 MHz, reset GPIO 5, backlight GPIO 23) via `esp_lcd_st7701` +
`esp_lvgl_port`. `ui.c` draws the dashboard (Solar / Home / Grid cards + a
battery panel with %, bar, and charge/discharge state) and `ui_update()` binds
it to `tesla_live_status_t` each poll.

First-light tuning (display-only, no effect on the data path) — adjust in `display.c`:

| Symptom | Fix |
|---|---|
| Colours look swapped (red↔blue) | set `.flags.swap_bytes = true` in the lvgl display cfg, or change `rgb_ele_order` to `BGR` |
| Image mirrored / upside-down | set `rotation` mirror_x / mirror_y in `lvgl_port_display_cfg_t` |
| Backlight stays dark | backlight may be active-low or on a different pin — flip the `gpio_set_level` value / `PIN_BACKLIGHT` |
| Garbled / no sync | re-check the DSI timings/lane rate against your panel revision |

## Security notes

- `secrets/` contents (`.env`, `*.pem`) and `firmware/main/secrets.h` are git-ignored.
- The private key never leaves your machine; only the public key is hosted.
- Refresh tokens are long-lived — treat `tesla_secrets.env` like a password.
