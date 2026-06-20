#include "tesla_client.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "secrets.h"

static const char *TAG = "tesla";

#define AUTH_TOKEN_URL "https://auth.tesla.com/oauth2/v3/token"
#define HTTP_RESP_MAX  8192
#define REFRESH_TOK_MAX 600

static char   *s_access_token = NULL;   // heap-allocated JWT
static int64_t s_token_expiry_us = 0;   // esp_timer time when our token expires
static char    s_refresh_token[REFRESH_TOK_MAX];

// ---- HTTP plumbing -------------------------------------------------------

typedef struct { char *buf; int len; int cap; } resp_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        resp_buf_t *r = (resp_buf_t *)evt->user_data;
        int space = r->cap - r->len - 1;
        int n = evt->data_len < space ? evt->data_len : space;
        if (n > 0) {
            memcpy(r->buf + r->len, evt->data, n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
        if (n < evt->data_len) ESP_LOGW(TAG, "response truncated (cap %d)", r->cap);
    }
    return ESP_OK;
}

// Returns HTTP status code, or -1 on transport error. resp is null-terminated.
static int http_do(esp_http_client_method_t method, const char *url,
                   const char *bearer, const char *content_type,
                   const char *body, char *resp, int resp_cap)
{
    resp_buf_t rb = { .buf = resp, .len = 0, .cap = resp_cap };
    if (resp && resp_cap) resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_evt,
        .user_data = &rb,
        .crt_bundle_attach = esp_crt_bundle_attach,   // verify against Mozilla CA bundle
        .timeout_ms = 15000,
        // The access token is a ~1 KB JWT sent in the Authorization header, so the
        // default 512-byte TX buffer overflows ("Buffer length is small to fit all
        // the headers"). Give both buffers room.
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    if (content_type) esp_http_client_set_header(c, "Content-Type", content_type);
    if (bearer) {
        // The access token is a ~1 KB JWT — size the header to it, don't use a
        // fixed buffer (a small one silently truncates the token -> 401).
        size_t n = strlen(bearer) + 8;   // "Bearer " + token + NUL
        char *hdr = malloc(n);
        if (hdr) {
            snprintf(hdr, n, "Bearer %s", bearer);
            esp_http_client_set_header(c, "Authorization", hdr);  // copied internally
            free(hdr);
        }
    }
    if (body) esp_http_client_set_post_field(c, body, strlen(body));

    int status = -1;
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) status = esp_http_client_get_status_code(c);
    else ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return status;
}

// ---- token management ----------------------------------------------------

static void save_refresh_token(const char *tok)
{
    if (!tok || !tok[0] || strcmp(tok, s_refresh_token) == 0) return;
    strlcpy(s_refresh_token, tok, sizeof(s_refresh_token));
    nvs_handle_t h;
    if (nvs_open("tesla", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "refresh_tok", s_refresh_token);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "persisted rotated refresh token");
    }
}

static esp_err_t refresh_access_token(void)
{
    // refresh_token grant needs only client_id + refresh_token (no client secret).
    char body[REFRESH_TOK_MAX + 128];
    snprintf(body, sizeof(body),
             "grant_type=refresh_token&client_id=%s&refresh_token=%s",
             TESLA_CLIENT_ID, s_refresh_token);

    char *resp = malloc(HTTP_RESP_MAX);
    if (!resp) return ESP_ERR_NO_MEM;

    int status = http_do(HTTP_METHOD_POST, AUTH_TOKEN_URL, NULL,
                         "application/x-www-form-urlencoded", body, resp, HTTP_RESP_MAX);
    if (status != 200) {
        ESP_LOGE(TAG, "token refresh HTTP %d: %s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) { ESP_LOGE(TAG, "token JSON parse failed"); return ESP_FAIL; }

    esp_err_t ret = ESP_FAIL;
    cJSON *at = cJSON_GetObjectItem(root, "access_token");
    cJSON *ex = cJSON_GetObjectItem(root, "expires_in");
    cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    if (cJSON_IsString(at)) {
        free(s_access_token);
        s_access_token = strdup(at->valuestring);
        int expires_in = cJSON_IsNumber(ex) ? ex->valueint : 28800;
        // refresh 5 min early
        s_token_expiry_us = esp_timer_get_time() + (int64_t)(expires_in - 300) * 1000000;
        if (cJSON_IsString(rt)) save_refresh_token(rt->valuestring);
        ESP_LOGI(TAG, "access token refreshed (valid ~%d s)", expires_in);
        ret = (s_access_token != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
    } else {
        ESP_LOGE(TAG, "no access_token in token response");
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ensure_token(void)
{
    if (s_access_token && esp_timer_get_time() < s_token_expiry_us) return ESP_OK;
    return refresh_access_token();
}

// ---- public API ----------------------------------------------------------

esp_err_t tesla_client_init(void)
{
    // Tesla rotates refresh tokens, so we persist the rotated one to NVS and
    // reuse it across reboots. But if a NEW token was just flashed (secrets.h
    // changed), it must override the stale NVS token — otherwise re-minting a
    // token has no effect. We detect this by storing the flashed token as a
    // "seed": when the seed changes, we reset NVS to the flashed token.
    const char *flashed = TESLA_REFRESH_TOKEN;
    strlcpy(s_refresh_token, flashed, sizeof(s_refresh_token));

    nvs_handle_t h;
    if (nvs_open("tesla", NVS_READWRITE, &h) == ESP_OK) {
        char seed[REFRESH_TOK_MAX] = {0};
        size_t slen = sizeof(seed);
        bool have_seed = (nvs_get_str(h, "seed_tok", seed, &slen) == ESP_OK);

        if (!have_seed || strcmp(seed, flashed) != 0) {
            // First boot, or a freshly flashed token — adopt it, reset NVS.
            nvs_set_str(h, "seed_tok", flashed);
            nvs_set_str(h, "refresh_tok", flashed);
            nvs_commit(h);
            ESP_LOGI(TAG, "using freshly flashed refresh token");
        } else {
            char tmp[REFRESH_TOK_MAX];
            size_t len = sizeof(tmp);
            if (nvs_get_str(h, "refresh_tok", tmp, &len) == ESP_OK && tmp[0]) {
                strlcpy(s_refresh_token, tmp, sizeof(s_refresh_token));
                ESP_LOGI(TAG, "using stored (rotated) refresh token");
            }
        }
        nvs_close(h);
    }

    if (!s_refresh_token[0]) {
        ESP_LOGE(TAG, "no refresh token — run gen_secrets.sh after tesla_setup.py login");
        return ESP_ERR_INVALID_STATE;
    }
    return refresh_access_token();
}

esp_err_t tesla_fetch_live_status(tesla_live_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (ensure_token() != ESP_OK) return ESP_FAIL;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/1/energy_sites/%s/live_status",
             TESLA_AUDIENCE, TESLA_ENERGY_SITE_ID);

    char *resp = malloc(HTTP_RESP_MAX);
    if (!resp) return ESP_ERR_NO_MEM;

    int status = http_do(HTTP_METHOD_GET, url, s_access_token, NULL, NULL, resp, HTTP_RESP_MAX);
    if (status == 401) {                          // token rejected — refresh once and retry
        ESP_LOGW(TAG, "401 — refreshing token and retrying");
        if (refresh_access_token() == ESP_OK)
            status = http_do(HTTP_METHOD_GET, url, s_access_token, NULL, NULL, resp, HTTP_RESP_MAX);
    }
    if (status != 200) {
        ESP_LOGE(TAG, "live_status HTTP %d: %s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItem(root, "response");
    if (!r) { ESP_LOGE(TAG, "no 'response' in live_status"); cJSON_Delete(root); return ESP_FAIL; }

#define GETD(f) (cJSON_IsNumber(cJSON_GetObjectItem(r, f)) ? cJSON_GetObjectItem(r, f)->valuedouble : 0.0)
    out->solar_power        = GETD("solar_power");
    out->battery_power      = GETD("battery_power");
    out->grid_power         = GETD("grid_power");
    out->load_power         = GETD("load_power");
    out->percentage_charged = GETD("percentage_charged");
    out->energy_left        = GETD("energy_left");
    out->total_pack_energy  = GETD("total_pack_energy");
#undef GETD

    cJSON *gs = cJSON_GetObjectItem(r, "grid_status");
    if (cJSON_IsString(gs)) strlcpy(out->grid_status, gs->valuestring, sizeof(out->grid_status));
    else out->grid_status[0] = '\0';

    cJSON_Delete(root);
    return ESP_OK;
}
