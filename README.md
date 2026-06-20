# Tesla Powerwall Dashboard — ESP32-P4

A small, always-on wall dashboard that shows your **live solar, Powerwall, grid,
and home power** on a 4.3" touchscreen, pulled straight from the **Tesla Fleet
API**. Built for the inexpensive **Guition JC4880P443C** (ESP32-P4 + ESP32-C6)
display board using ESP-IDF + LVGL.

> Works great for **Powerwall 3** owners, where Tesla removed the old local REST
> API — this uses the official cloud Fleet API instead, so it works on any
> Powerwall generation and from anywhere.

```
┌─ 480 × 800 ───────────────┐
│ POWERWALL        GRID ON   │   ← green/red grid-status badge
│ Solar            6.20 kW   │
│ ▰▰▰▰▰▱▱▱▱▱▱▱▱▱▱▱▱          │   ← scaled to your array nameplate
│ Home             3.70 kW   │
│ ▰▰▰▰▰▰▰▰▱▱▱▱▱▱▱▱▱          │   ← auto-scales to peak
│ Grid             2.50 kW ↓ │
│        ◀▰▰▰│              │   ← center-zero: import → / ← export
│ ┌───────────────────────┐ │
│ │       BATTERY           │ │
│ │         63%             │ │
│ │ ▰▰▰▰▰▰▰▰▰▰▱▱▱▱▱▱        │ │
│ │     17.0 / 27.0 kWh     │ │
│ │   Charging 1.90 kW      │ │
│ └───────────────────────┘ │
└────────────────────────────┘
```

## Features

- **Live power** for Solar, Home, and Grid — value + bar each. Grid and battery
  are center-zero bars (import/export, charge/discharge).
- **Battery card** — state of charge %, a bar, energy in kWh, and live
  charge/discharge rate.
- **Grid status** badge (on-grid / off-grid).
- **No client secret on the device** — only a refresh token, which the firmware
  rotates and persists. The secret never leaves your setup machine.
- Auto-reconnect, token auto-refresh, and a `~20 s` poll (well under Tesla's rate
  limit).

## Hardware

| Part | Notes |
|---|---|
| **Guition JC4880P443C_I_W** | ESP32-P4 (32 MB PSRAM) + ESP32-C6 (Wi-Fi over esp-hosted/SDIO), 4.3" 480×800 IPS, **ST7701S** MIPI-DSI, GT911 touch. ~US$35. |
| USB-C cable | For flashing + power. |

Any Tesla energy system works (Powerwall 2 / + / 3, solar). The display code is
specific to this panel, but the Tesla client (`tesla_client.c`) is portable.

## How it works

```
  one-time, on your computer + a web server you control
  ┌──────────────────────────────────────────────────────────────┐
  │ tools/tesla_setup.py:                                          │
  │   genkey → host public key → register partner → OAuth login    │
  │   → refresh_token + energy_site_id  (saved to secrets/.env)    │
  └──────────────────────────────────────────────────────────────┘
                         │  baked into firmware via gen_secrets.sh
                         ▼
  ESP32-P4 runtime loop (firmware/):
    refresh_token ──POST auth.tesla.com/oauth2/v3/token──▶ access_token (~8 h)
    access_token  ──GET …/energy_sites/{id}/live_status──▶ solar/grid/load/%
    └─▶ LVGL renders the dashboard, repeat every ~20 s
```

The Fleet API requires a one-time partner registration that involves hosting a
public key on a domain you control. After that the device just refreshes a token
and polls — no secret, no domain dependency at runtime.

## Prerequisites

- **ESP-IDF v5.3+** (developed on v5.4). Install per
  [Espressif's guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/),
  then `. $HOME/esp/esp-idf/export.sh` in each shell.
- **Python 3** with `requests` (`pip install -r tools/requirements.txt`).
- A **Tesla developer app** (https://developer.tesla.com) — client ID + secret.
- A **domain you control** to host the Fleet API public key (a static file is enough).

## Setup — Part 1: Tesla Fleet API (one-time)

```bash
cp secrets/tesla_secrets.example.env secrets/tesla_secrets.env
# Edit secrets/tesla_secrets.env: Wi-Fi creds, client id/secret, your domain,
# redirect URI, and audience (region — NA covers North America + Asia-Pacific).
pip install -r tools/requirements.txt

cd tools
python3 tesla_setup.py check      # sanity-check what was loaded (secret masked)
python3 tesla_setup.py genkey     # make an EC key pair; prints where to host the public key
#   → host secrets/public-key.pem at:
#     https://YOURDOMAIN/.well-known/appspecific/com.tesla.3p.public-key.pem
python3 tesla_setup.py register   # register your partner account with Tesla
python3 tesla_setup.py login      # OAuth in a browser; paste the redirected URL back
python3 tesla_setup.py site       # find + save your energy_site_id
python3 tesla_setup.py test       # prints your live numbers — confirms it all works
python3 tesla_setup.py info       # (optional) dump site_info: capacity, nameplate, etc.
```

In your developer-portal app settings, make sure your domain is an allowed origin
and your redirect URI (default `https://YOURDOMAIN/auth/tesla/callback`) is listed.

When `test` prints real figures, `secrets/tesla_secrets.env` holds the
`TESLA_REFRESH_TOKEN` and `TESLA_ENERGY_SITE_ID` the firmware needs.

## Setup — Part 2: Firmware

First, set two constants for your system in `firmware/main/ui.c`:

```c
#define SOLAR_MAX_W       18000.0   // your solar array nameplate (W) — full-scale for the Solar bar
#define BATTERY_CAP_KWH   27.0      // your battery capacity (site_info "nameplate_energy_watts" / 1000)
```

Then build and flash:

```bash
cd firmware
./gen_secrets.sh                 # writes main/secrets.h from ../secrets/tesla_secrets.env
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # adjust the serial port
```

Healthy boot log:

```
display: ST7701S panel up (480x800)
display: LVGL ready
wifi: got IP 192.168.x.x
tesla: using freshly flashed refresh token
tesla: access token refreshed (valid ~28800 s)
main: Solar  6.20 kW | Home  3.70 kW | Grid +2.50 kW | Batt -1.90 kW | 63%
```

## Configuration reference

| What | Where |
|---|---|
| Wi-Fi, Tesla client id, refresh token, site id, region | `secrets/tesla_secrets.env` → `gen_secrets.sh` |
| Solar bar full-scale / battery capacity | `SOLAR_MAX_W`, `BATTERY_CAP_KWH` in `main/ui.c` |
| Poll interval | `POLL_INTERVAL_MS` in `main/main.c` (rate limit allows down to ~1 s) |
| Panel init / pins / DSI timings | `main/display.c` |
| Region/audience URL | `TESLA_AUDIENCE` (NA `fleet-api.prd.na.vn.cloud.tesla.com`, EU `…eu…`) |

## Troubleshooting

This board has several ESP32-P4-specific gotchas. All of these are handled in the
code/config already — listed here in case you adapt it or hit a variant:

| Symptom | Cause / fix |
|---|---|
| `token refresh HTTP 401 … refresh_token is invalid` | Tesla **rotates** refresh tokens. Re-run `tesla_setup.py login` → `gen_secrets.sh` → flash. The firmware adopts a freshly-flashed token (overrides its NVS copy). |
| `assert failed: sdio_mempool_create … no mem` at boot | esp-hosted needs DMA RAM; LVGL took it. Fixed by `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`. |
| CPU hangs the instant the display inits (`vPortYield`) | The MIPI-DSI PHY LDO isn't powered. `display.c` acquires LDO channel 3 @ 2500 mV first. |
| `lcd.dsi.dpi: can't fetch data … fast enough, underrun` | PSRAM too slow. Needs 200 MHz: `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` + `CONFIG_SPIRAM_SPEED_200M=y` + `CONFIG_SPIRAM_MODE_HEX=y`. |
| Backlight on but screen black | ST7701S left asleep. The init sequence must end with sleep-out `0x11` (+120 ms) and display-on `0x29` — the `esp_lcd_st7701` driver does **not** send these. |
| Display blank with `avoid_tearing=false` | For DSI, use `avoid_tearing=true` + `full_refresh=true` so LVGL renders into the panel's framebuffers. |
| Labels show `f kW` instead of numbers | LVGL's printf has no `%f`. Format with C `snprintf` then `lv_label_set_text`. |
| `HTTP_HEADER: Buffer length is small…` / 401 on data | The access-token JWT is ~1 KB; the HTTP client's TX buffer and the `Authorization` header buffer must be sized to it. |
| Wi-Fi never connects, stuck retrying | ESP32-C6 esp-hosted link. Ensure the C6 runs matching slave firmware; check `menuconfig → Wi-Fi Remote`. |
| Colours swapped / image mirrored | `.flags.swap_bytes` or `rgb_ele_order`; `rotation` mirror flags in `display.c`. |

## A note on refresh-token rotation

Tesla issues a new refresh token on every refresh and invalidates the old one.
The firmware is the single source of truth at runtime — it persists rotated
tokens to NVS. If you run the `tesla_setup.py` commands again after deploying
(they also consume/rotate the token), the device's copy will go stale; just
re-run `login` → `gen_secrets.sh` → flash to resync.

## Project structure

```
tesla-dashboard/
├── tools/
│   ├── tesla_setup.py          # one-time Fleet API setup (genkey/register/login/site/test/info/check)
│   └── requirements.txt
├── secrets/                    # git-ignored; your .env + key pair live here
│   └── tesla_secrets.example.env
└── firmware/
    ├── gen_secrets.sh          # secrets/.env → main/secrets.h
    ├── sdkconfig.defaults      # P4 target, 200 MHz PSRAM, CA bundle, esp-hosted, LVGL fonts
    ├── partitions.csv
    └── main/
        ├── main.c              # boot → display → wifi → tesla → poll loop
        ├── wifi.c/.h           # STA connect via the C6
        ├── tesla_client.c/.h   # OAuth refresh + live_status parse (cJSON), NVS token handling
        ├── display.c/.h        # ST7701S MIPI-DSI bring-up + esp_lvgl_port
        ├── ui.c/.h             # the LVGL dashboard
        └── idf_component.yml    # esp_wifi_remote, esp_hosted, esp_lcd_st7701, esp_lvgl_port, lvgl
```

## Security

- `secrets/` (`.env`, `*.pem`) and `firmware/main/secrets.h` are git-ignored.
- The **client secret never goes on the device** — the refresh-token grant only
  needs the client ID. Only the public key is hosted; the private key stays local.
- Treat `secrets/tesla_secrets.env` like a password (it holds a long-lived token).

## Credits

- Panel init sequence + DSI timings adapted from the
  [ESPHome JC4880P443 support](https://github.com/esphome/esphome/pull/12068)
  and the board vendor's ESP-IDF demo.
- Built on [ESP-IDF](https://github.com/espressif/esp-idf),
  [LVGL](https://lvgl.io), and Espressif's `esp_lcd_st7701` / `esp_lvgl_port` /
  `esp_hosted` components.
- Tesla [Fleet API](https://developer.tesla.com/docs/fleet-api).

## License

MIT — see [`LICENSE`](LICENSE).

*Not affiliated with or endorsed by Tesla, Inc. Tesla and Powerwall are trademarks of Tesla, Inc.*
