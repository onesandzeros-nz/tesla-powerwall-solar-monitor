#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define COL_BG      lv_color_hex(0x0E1116)
#define COL_CARD    lv_color_hex(0x1A1F27)
#define COL_TRACK   lv_color_hex(0x2A2F38)
#define COL_TEXT    lv_color_hex(0xE6E6E6)
#define COL_DIM     lv_color_hex(0x8A93A0)
#define COL_SOLAR   lv_color_hex(0xFFC107)
#define COL_HOME    lv_color_hex(0x29B6F6)
#define COL_GRID    lv_color_hex(0xB388FF)
#define COL_BATT    lv_color_hex(0x66BB6A)
#define COL_IMPORT  lv_color_hex(0xEF5350)   /* drawing from grid */
#define COL_EXPORT  lv_color_hex(0x66BB6A)   /* sending to grid   */
#define COL_BATT_OUT lv_color_hex(0xFFA726)  /* battery discharging */

#define SOLAR_MAX_W       18000.0   /* your solar array nameplate */
#define BATTERY_CAP_KWH   27.0      /* site_info nameplate_energy_watts (PW3 + Expansion) */

// Bar auto-scale floors (grow to the largest value seen).
static double s_home_max  = 4000.0;
static double s_grid_max  = 4000.0;
static double s_battp_max = 4000.0;

typedef struct { lv_obj_t *value; lv_obj_t *bar; } metric_t;
static metric_t s_solar, s_home, s_grid, s_batt_pow;
static lv_obj_t *s_grid_badge, *s_batt_pct, *s_batt_kwh, *s_batt_state, *s_soc_bar;
static lv_obj_t *s_status;

// A card with a name (left) + value (right) header row and a bar underneath.
static void make_metric(lv_obj_t *parent, const char *name, lv_color_t accent,
                        bool symmetric, metric_t *out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 96);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lname = lv_label_create(row);
    lv_label_set_text(lname, name);
    lv_obj_set_style_text_color(lname, accent, 0);
    lv_obj_set_style_text_font(lname, &lv_font_montserrat_20, 0);

    out->value = lv_label_create(row);
    lv_label_set_text(out->value, "-- kW");
    lv_obj_set_style_text_color(out->value, COL_TEXT, 0);
    lv_obj_set_style_text_font(out->value, &lv_font_montserrat_28, 0);

    out->bar = lv_bar_create(card);
    lv_obj_set_size(out->bar, lv_pct(100), 16);
    lv_obj_set_style_radius(out->bar, 8, 0);
    lv_obj_set_style_bg_color(out->bar, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(out->bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(out->bar, 8, LV_PART_INDICATOR);
    if (symmetric) lv_bar_set_mode(out->bar, LV_BAR_MODE_SYMMETRICAL);
}

void ui_init(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);

    // Header: title + grid status badge
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "POWERWALL");
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    s_grid_badge = lv_label_create(hdr);
    lv_label_set_text(s_grid_badge, "GRID --");
    lv_obj_set_style_text_color(s_grid_badge, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_grid_badge, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_opa(s_grid_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_grid_badge, COL_TRACK, 0);
    lv_obj_set_style_radius(s_grid_badge, 10, 0);
    lv_obj_set_style_pad_hor(s_grid_badge, 12, 0);
    lv_obj_set_style_pad_ver(s_grid_badge, 6, 0);

    make_metric(scr, "Solar",   COL_SOLAR, false, &s_solar);
    make_metric(scr, "Home",    COL_HOME,  false, &s_home);
    make_metric(scr, "Grid",    COL_GRID,  true,  &s_grid);
    make_metric(scr, "Battery", COL_BATT,  true,  &s_batt_pow);

    // Battery card
    lv_obj_t *bcard = lv_obj_create(scr);
    lv_obj_set_size(bcard, lv_pct(100), 224);
    lv_obj_set_style_bg_color(bcard, COL_CARD, 0);
    lv_obj_set_style_border_width(bcard, 0, 0);
    lv_obj_set_style_radius(bcard, 14, 0);
    lv_obj_set_style_pad_all(bcard, 16, 0);
    lv_obj_set_flex_flow(bcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bcard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(bcard, 6, 0);
    lv_obj_clear_flag(bcard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *blabel = lv_label_create(bcard);
    lv_label_set_text(blabel, "BATTERY");
    lv_obj_set_style_text_color(blabel, COL_DIM, 0);
    lv_obj_set_style_text_font(blabel, &lv_font_montserrat_20, 0);

    s_batt_pct = lv_label_create(bcard);
    lv_label_set_text(s_batt_pct, "--%");
    lv_obj_set_style_text_color(s_batt_pct, COL_BATT, 0);
    lv_obj_set_style_text_font(s_batt_pct, &lv_font_montserrat_48, 0);

    s_soc_bar = lv_bar_create(bcard);
    lv_obj_set_size(s_soc_bar, lv_pct(100), 22);
    lv_bar_set_range(s_soc_bar, 0, 100);
    lv_obj_set_style_radius(s_soc_bar, 11, 0);
    lv_obj_set_style_bg_color(s_soc_bar, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_soc_bar, COL_BATT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_soc_bar, 11, LV_PART_INDICATOR);

    s_batt_kwh = lv_label_create(bcard);
    lv_label_set_text(s_batt_kwh, "-- / -- kWh");
    lv_obj_set_style_text_color(s_batt_kwh, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_batt_kwh, &lv_font_montserrat_20, 0);

    s_batt_state = lv_label_create(bcard);
    lv_label_set_text(s_batt_state, "");
    lv_obj_set_style_text_color(s_batt_state, COL_DIM, 0);
    lv_obj_set_style_text_font(s_batt_state, &lv_font_montserrat_20, 0);

    // Footer status line (boot progress / errors / "Live")
    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "Starting...");
    lv_obj_set_style_text_color(s_status, COL_DIM, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);

    lvgl_port_unlock();
}

void ui_set_status(const char *msg, bool error)
{
    if (!s_status) return;
    lvgl_port_lock(0);
    lv_label_set_text(s_status, msg);
    lv_obj_set_style_text_color(s_status, error ? COL_IMPORT : COL_DIM, 0);
    lvgl_port_unlock();
}

void ui_update(const tesla_live_status_t *s)
{
    if (!s) return;
    char buf[48];
    lvgl_port_lock(0);

    // grow auto-scales
    if (s->load_power > s_home_max) s_home_max = s->load_power;
    double grid_abs = fabs(s->grid_power);
    if (grid_abs > s_grid_max) s_grid_max = grid_abs;

    // Solar
    snprintf(buf, sizeof(buf), "%.2f kW", s->solar_power / 1000.0);
    lv_label_set_text(s_solar.value, buf);
    lv_bar_set_range(s_solar.bar, 0, (int)SOLAR_MAX_W);
    lv_bar_set_value(s_solar.bar, (int)s->solar_power, LV_ANIM_OFF);

    // Home
    snprintf(buf, sizeof(buf), "%.2f kW", s->load_power / 1000.0);
    lv_label_set_text(s_home.value, buf);
    lv_bar_set_range(s_home.bar, 0, (int)s_home_max);
    lv_bar_set_value(s_home.bar, (int)s->load_power, LV_ANIM_OFF);

    // Grid (center-zero: import right / export left)
    double grid = s->grid_power / 1000.0;
    if (grid > 0.05)       snprintf(buf, sizeof(buf), "%.2f kW " LV_SYMBOL_DOWN, grid);
    else if (grid < -0.05) snprintf(buf, sizeof(buf), "%.2f kW " LV_SYMBOL_UP, -grid);
    else                   snprintf(buf, sizeof(buf), "0.00 kW");
    lv_label_set_text(s_grid.value, buf);
    lv_obj_set_style_bg_color(s_grid.bar,
                              s->grid_power >= 0 ? COL_IMPORT : COL_EXPORT, LV_PART_INDICATOR);
    lv_bar_set_range(s_grid.bar, -(int)s_grid_max, (int)s_grid_max);
    lv_bar_set_value(s_grid.bar, (int)s->grid_power, LV_ANIM_OFF);

    // Battery power (center-zero: charge left/green, discharge right/amber).
    // battery_power is +discharge / -charge.
    double battp_abs = fabs(s->battery_power);
    if (battp_abs > s_battp_max) s_battp_max = battp_abs;
    snprintf(buf, sizeof(buf), "%.2f kW", battp_abs / 1000.0);
    lv_label_set_text(s_batt_pow.value, buf);
    lv_obj_set_style_bg_color(s_batt_pow.bar,
                              s->battery_power > 0 ? COL_BATT_OUT : COL_BATT, LV_PART_INDICATOR);
    lv_bar_set_range(s_batt_pow.bar, -(int)s_battp_max, (int)s_battp_max);
    lv_bar_set_value(s_batt_pow.bar, (int)s->battery_power, LV_ANIM_OFF);

    // Battery
    int pct = (int)(s->percentage_charged + 0.5);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_batt_pct, buf);
    lv_bar_set_value(s_soc_bar, pct, LV_ANIM_ON);

    // live_status has no energy fields for this site; derive kWh from % of capacity
    double left_kwh = BATTERY_CAP_KWH * s->percentage_charged / 100.0;
    snprintf(buf, sizeof(buf), "%.1f / %.1f kWh", left_kwh, BATTERY_CAP_KWH);
    lv_label_set_text(s_batt_kwh, buf);

    double batt = s->battery_power / 1000.0;
    if (batt > 0.05)       snprintf(buf, sizeof(buf), "Discharging %.2f kW", batt);
    else if (batt < -0.05) snprintf(buf, sizeof(buf), "Charging %.2f kW", -batt);
    else                   snprintf(buf, sizeof(buf), "Idle");
    lv_label_set_text(s_batt_state, buf);

    // Grid status badge
    bool on_grid = (strcmp(s->grid_status, "Active") == 0);
    snprintf(buf, sizeof(buf), "GRID %s", on_grid ? "ON" : "OFF");
    lv_label_set_text(s_grid_badge, buf);
    lv_obj_set_style_bg_color(s_grid_badge, on_grid ? COL_EXPORT : COL_IMPORT, 0);

    lv_label_set_text(s_status, LV_SYMBOL_OK " Live");
    lv_obj_set_style_text_color(s_status, COL_EXPORT, 0);

    lvgl_port_unlock();
}
