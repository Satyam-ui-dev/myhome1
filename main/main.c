#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#define WIFI_SSID      "Mahakalkijai"
#define WIFI_PASS      "ekse8tak"

// Simple shared secret. Must match API_KEY in index.html
#define API_KEY        "change-me-123"

#define LIGHT_GPIO     GPIO_NUM_2
#define FAN_GPIO       GPIO_NUM_27
#define LIGHT2_GPIO    GPIO_NUM_26
#define SOCKET_GPIO    GPIO_NUM_25

// Active-LOW relay logic (0 = ON, 1 = OFF)
#define RELAY_ON  0
#define RELAY_OFF 1

static const char *TAG = "home_auto";
static bool light_state = false;
static bool fan_state = false;
static bool light2_state = false;
static bool socket_state = false;
static httpd_handle_t server = NULL;

/* ---------- usage analysis ---------- */
// Daily usage history is retained in NVS and dated using synchronized time.
#define DEV_LIGHT   0
#define DEV_FAN     1
#define DEV_LIGHT2  2
#define DEV_SOCKET  3
#define DEVICE_COUNT 4
#define MAX_EVENTS  20
#define HISTORY_DAYS 30
#define MIN_VALID_EPOCH 1700000000

typedef struct { uint32_t t; uint8_t dev; uint8_t on; } event_t;
typedef struct {
    int32_t day;
    uint32_t on_seconds[DEVICE_COUNT];
    uint32_t on_events[DEVICE_COUNT];
    uint32_t off_events[DEVICE_COUNT];
} daily_history_t;
typedef struct {
    int32_t day;
    uint32_t on_seconds[2];
    uint32_t on_events[2];
    uint32_t off_events[2];
} legacy_daily_history_t;
typedef struct {
    uint8_t light;
    uint8_t fan;
    uint8_t light2;
    uint8_t socket;
} relay_states_t;

static event_t  events[MAX_EVENTS];
static uint32_t event_total = 0;          // total events ever logged
static time_t on_since_epoch[DEVICE_COUNT] = {0};
static daily_history_t history[HISTORY_DAYS];
static nvs_handle_t history_nvs;
static int32_t timezone_offset_seconds = 0;
static int32_t history_checkpoint_day = INT32_MIN;
static bool sntp_started = false;

static uint32_t uptime_s(void) { return (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ); }

static bool *dev_state(int dev)
{
    switch (dev) {
    case DEV_LIGHT: return &light_state;
    case DEV_FAN: return &fan_state;
    case DEV_LIGHT2: return &light2_state;
    default: return &socket_state;
    }
}

static gpio_num_t dev_pin(int dev)
{
    switch (dev) {
    case DEV_LIGHT: return LIGHT_GPIO;
    case DEV_FAN: return FAN_GPIO;
    case DEV_LIGHT2: return LIGHT2_GPIO;
    default: return SOCKET_GPIO;
    }
}

static const char *dev_name(int dev)
{
    switch (dev) {
    case DEV_LIGHT: return "light";
    case DEV_FAN: return "fan";
    case DEV_LIGHT2: return "light2";
    default: return "socket";
    }
}

static bool current_epoch(time_t *now)
{
    *now = time(NULL);
    return *now >= MIN_VALID_EPOCH;
}

static int32_t local_day(time_t epoch)
{
    return (int32_t)(((int64_t)epoch + timezone_offset_seconds) / 86400);
}

static void update_timezone_offset(httpd_req_t *req)
{
    char value[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Timezone-Offset-Minutes", value, sizeof(value)) != ESP_OK) return;

    char *end = NULL;
    long minutes = strtol(value, &end, 10);
    if (end != value && *end == '\0' && minutes >= -720 && minutes <= 840) {
        timezone_offset_seconds = (int32_t)(minutes * 60);
    } else {
        ESP_LOGW(TAG, "Ignoring invalid browser timezone offset");
    }
}

static void sync_time_from_browser(httpd_req_t *req)
{
    time_t now;
    if (current_epoch(&now)) return;

    char value[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Client-Epoch", value, sizeof(value)) != ESP_OK) return;
    char *end = NULL;
    long long epoch = strtoll(value, &end, 10);
    if (end == value || *end != '\0' || epoch < MIN_VALID_EPOCH || epoch > 2000000000LL) {
        ESP_LOGW(TAG, "Ignoring invalid browser clock");
        return;
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to set clock from browser");
    }
}

static void history_clear(void)
{
    memset(history, 0, sizeof(history));
    for (int i = 0; i < HISTORY_DAYS; i++) history[i].day = INT32_MIN;
}

static esp_err_t history_save(void);

static esp_err_t save_device_states(void)
{
    relay_states_t states = {
        .light = light_state,
        .fan = fan_state,
        .light2 = light2_state,
        .socket = socket_state
    };
    esp_err_t err = nvs_set_blob(history_nvs, "relay_states", &states, sizeof(states));
    if (err == ESP_OK) err = nvs_commit(history_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save relay states: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t reset_device_states(void)
{
    light_state = false;
    fan_state = false;
    light2_state = false;
    socket_state = false;
    return save_device_states();
}

static esp_err_t load_device_states(void)
{
    size_t size = 0;
    esp_err_t err = nvs_get_blob(history_nvs, "relay_states", NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved relay states; keeping all relays OFF");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read saved relay states: %s", esp_err_to_name(err));
        return err;
    }
    if (size != sizeof(relay_states_t)) {
        ESP_LOGW(TAG, "Saved relay states have an unexpected size; defaulting all relays OFF");
        return reset_device_states();
    }

    relay_states_t states;
    err = nvs_get_blob(history_nvs, "relay_states", &states, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load saved relay states: %s", esp_err_to_name(err));
        return err;
    }
    if (states.light > 1 || states.fan > 1 || states.light2 > 1 || states.socket > 1) {
        ESP_LOGW(TAG, "Saved relay states contain invalid values; defaulting all relays OFF");
        return reset_device_states();
    }

    light_state = states.light;
    fan_state = states.fan;
    light2_state = states.light2;
    socket_state = states.socket;
    ESP_LOGI(TAG, "Restored relay states: light=%u fan=%u light2=%u socket=%u",
             states.light, states.fan, states.light2, states.socket);
    return ESP_OK;
}

static void history_load(void)
{
    ESP_ERROR_CHECK(nvs_open("home_auto", NVS_READWRITE, &history_nvs));
    size_t size = 0;
    esp_err_t err = nvs_get_blob(history_nvs, "history", NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        history_clear();
    } else if (err == ESP_OK && size == sizeof(history)) {
        err = nvs_get_blob(history_nvs, "history", history, &size);
        if (err != ESP_OK) ESP_ERROR_CHECK(err);
    } else if (err == ESP_OK && size == sizeof(legacy_daily_history_t) * HISTORY_DAYS) {
        legacy_daily_history_t legacy[HISTORY_DAYS];
        err = nvs_get_blob(history_nvs, "history", legacy, &size);
        if (err != ESP_OK) ESP_ERROR_CHECK(err);
        history_clear();
        for (int i = 0; i < HISTORY_DAYS; i++) {
            history[i].day = legacy[i].day;
            history[i].on_seconds[DEV_LIGHT] = legacy[i].on_seconds[0];
            history[i].on_seconds[DEV_FAN] = legacy[i].on_seconds[1];
            history[i].on_events[DEV_LIGHT] = legacy[i].on_events[0];
            history[i].on_events[DEV_FAN] = legacy[i].on_events[1];
            history[i].off_events[DEV_LIGHT] = legacy[i].off_events[0];
            history[i].off_events[DEV_FAN] = legacy[i].off_events[1];
        }
        ESP_LOGI(TAG, "Migrated saved light and fan history");
        ESP_ERROR_CHECK(history_save());
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load usage history: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    } else {
        ESP_LOGE(TAG, "Saved usage history has an unexpected size; starting a new history");
        history_clear();
    }
}

static esp_err_t history_save(void)
{
    esp_err_t err = nvs_set_blob(history_nvs, "history", history, sizeof(history));
    if (err == ESP_OK) err = nvs_commit(history_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save usage history: %s", esp_err_to_name(err));
    return err;
}

static bool history_prune(int32_t today)
{
    bool changed = false;
    int32_t first_day = today - HISTORY_DAYS + 1;
    for (int i = 0; i < HISTORY_DAYS; i++) {
        if (history[i].day != INT32_MIN &&
            (history[i].day < first_day || history[i].day > today)) {
            memset(&history[i], 0, sizeof(history[i]));
            history[i].day = INT32_MIN;
            changed = true;
        }
    }
    return changed;
}

static daily_history_t *history_for_day(daily_history_t *records, int32_t day)
{
    int32_t index = day % HISTORY_DAYS;
    if (index < 0) index += HISTORY_DAYS;
    daily_history_t *record = &records[index];
    if (record->day != day) {
        memset(record, 0, sizeof(*record));
        record->day = day;
    }
    return record;
}

static void history_add_duration(daily_history_t *records, int dev, time_t start, time_t end, int32_t today)
{
    int64_t cursor = start;
    int64_t stop = end;
    int64_t first_day_start = (int64_t)(today - HISTORY_DAYS + 1) * 86400 - timezone_offset_seconds;
    if (cursor < first_day_start) cursor = first_day_start;

    while (cursor < stop) {
        int32_t day = (int32_t)((cursor + timezone_offset_seconds) / 86400);
        int64_t next_midnight = (int64_t)(day + 1) * 86400 - timezone_offset_seconds;
        int64_t segment_end = stop < next_midnight ? stop : next_midnight;
        if (day >= today - HISTORY_DAYS + 1 && day <= today) {
            history_for_day(records, day)->on_seconds[dev] += (uint32_t)(segment_end - cursor);
        }
        cursor = segment_end;
    }
}

static esp_err_t history_record_transition(int dev, bool on, time_t now)
{
    int32_t today = local_day(now);
    history_prune(today);
    history_checkpoint_day = today;
    daily_history_t *record = history_for_day(history, today);
    if (on) {
        record->on_events[dev]++;
        on_since_epoch[dev] = now;
    } else {
        record->off_events[dev]++;
        if (on_since_epoch[dev] > 0) {
            history_add_duration(history, dev, on_since_epoch[dev], now, today);
        }
        on_since_epoch[dev] = 0;
    }
    return history_save();
}

// Change a device state, drive the relay, and record the event + ON time
static esp_err_t set_device(int dev, bool on)
{
    bool *st = dev_state(dev);
    esp_err_t history_err = ESP_OK;
    if (*st != on) {
        bool previous_state = *st;
        *st = on;
        esp_err_t state_err = save_device_states();
        if (state_err != ESP_OK) {
            *st = previous_state;
            return state_err;
        }

        uint32_t now = uptime_s();
        time_t epoch;
        events[event_total % MAX_EVENTS] = (event_t){ .t = now, .dev = dev, .on = on };
        event_total++;
        if (current_epoch(&epoch)) {
            history_err = history_record_transition(dev, on, epoch);
        } else {
            ESP_LOGW(TAG, "Device state changed before system time was synchronized; history event was not saved");
        }
    }
    gpio_set_level(dev_pin(dev), on ? RELAY_ON : RELAY_OFF);
    return history_err;
}

static void initialize_relay(gpio_num_t pin, bool on)
{
    ESP_ERROR_CHECK(gpio_reset_pin(pin));
    ESP_ERROR_CHECK(gpio_set_level(pin, on ? RELAY_ON : RELAY_OFF));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
}

/* ---------- helpers ---------- */

static void add_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                       "Content-Type, X-API-Key, X-Timezone-Offset-Minutes, X-Client-Epoch");
}

static bool authorized(httpd_req_t *req)
{
    char key[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", key, sizeof(key)) != ESP_OK) {
        return false;
    }
    return strcmp(key, API_KEY) == 0;
}

static esp_err_t send_status(httpd_req_t *req)
{
    const size_t cap = 8192;
    char *json = malloc(cap);
    if (!json) return httpd_resp_send_500(req);

    time_t now;
    bool time_synced = current_epoch(&now);
    int32_t today = time_synced ? local_day(now) : -1;
    daily_history_t history_view[HISTORY_DAYS];
    memcpy(history_view, history, sizeof(history_view));
    if (time_synced) {
        bool history_changed = history_prune(today);
        if (history_checkpoint_day == INT32_MIN) {
            history_checkpoint_day = today;
        } else if (history_checkpoint_day != today) {
            for (int dev = 0; dev < DEVICE_COUNT; dev++) {
                if (*dev_state(dev) && on_since_epoch[dev] > 0 && now >= on_since_epoch[dev]) {
                    history_add_duration(history, dev, on_since_epoch[dev], now, today);
                    on_since_epoch[dev] = now;
                    history_changed = true;
                }
            }
            history_checkpoint_day = today;
        }
        if (history_changed && history_save() != ESP_OK) {
            free(json);
            return httpd_resp_send_500(req);
        }
        memcpy(history_view, history, sizeof(history_view));
        for (int dev = 0; dev < DEVICE_COUNT; dev++) {
            if (*dev_state(dev) && on_since_epoch[dev] > 0) {
                history_add_duration(history_view, dev, on_since_epoch[dev], now, today);
            }
        }
    }

    int n = snprintf(json, cap,
        "{\"light\":%s,\"fan\":%s,\"light2\":%s,\"socket\":%s,\"up\":%u,\"time_synced\":%s,\"history\":[",
        light_state ? "true" : "false",
        fan_state ? "true" : "false",
        light2_state ? "true" : "false",
        socket_state ? "true" : "false",
        (unsigned) uptime_s(),
        time_synced ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        free(json);
        return httpd_resp_send_500(req);
    }

    if (time_synced) {
        for (int i = HISTORY_DAYS - 1; i >= 0; i--) {
            int32_t day = today - i;
            int32_t index = day % HISTORY_DAYS;
            if (index < 0) index += HISTORY_DAYS;
            daily_history_t empty = { .day = day };
            daily_history_t *entry = history_view[index].day == day ? &history_view[index] : &empty;
            int written = snprintf(json + n, cap - (size_t)n,
                "%s{\"d\":%d,\"light_s\":%u,\"fan_s\":%u,\"light2_s\":%u,\"socket_s\":%u,"
                "\"light_on\":%u,\"light_off\":%u,\"fan_on\":%u,\"fan_off\":%u,"
                "\"light2_on\":%u,\"light2_off\":%u,\"socket_on\":%u,\"socket_off\":%u}",
                i == HISTORY_DAYS - 1 ? "" : ",", (int)day,
                (unsigned)entry->on_seconds[DEV_LIGHT], (unsigned)entry->on_seconds[DEV_FAN],
                (unsigned)entry->on_seconds[DEV_LIGHT2], (unsigned)entry->on_seconds[DEV_SOCKET],
                (unsigned)entry->on_events[DEV_LIGHT], (unsigned)entry->off_events[DEV_LIGHT],
                (unsigned)entry->on_events[DEV_FAN], (unsigned)entry->off_events[DEV_FAN],
                (unsigned)entry->on_events[DEV_LIGHT2], (unsigned)entry->off_events[DEV_LIGHT2],
                (unsigned)entry->on_events[DEV_SOCKET], (unsigned)entry->off_events[DEV_SOCKET]);
            if (written < 0 || (size_t)written >= cap - (size_t)n) {
                free(json);
                return httpd_resp_send_500(req);
            }
            n += written;
        }
    }
    int written = snprintf(json + n, cap - (size_t)n, "],\"events\":[");
    if (written < 0 || (size_t)written >= cap - (size_t)n) {
        free(json);
        return httpd_resp_send_500(req);
    }
    n += written;

    // Output events oldest -> newest
    uint32_t count = event_total < MAX_EVENTS ? event_total : MAX_EVENTS;
    uint32_t start = event_total - count;
    for (uint32_t i = 0; i < count; i++) {
        event_t *e = &events[(start + i) % MAX_EVENTS];
        written = snprintf(json + n, cap - (size_t)n, "%s{\"t\":%u,\"d\":\"%s\",\"s\":%d}",
                           i ? "," : "", (unsigned)e->t, dev_name(e->dev), e->on);
        if (written < 0 || (size_t)written >= cap - (size_t)n) {
            free(json);
            return httpd_resp_send_500(req);
        }
        n += written;
    }
    written = snprintf(json + n, cap - (size_t)n, "]}");
    if (written < 0 || (size_t)written >= cap - (size_t)n) {
        free(json);
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return r;
}

/* Reads ?state=on|off|toggle and applies it. */
static esp_err_t apply_state(httpd_req_t *req, int dev)
{
    char query[64] = {0};
    char val[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "state", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "on") == 0)          return set_device(dev, true);
        else if (strcmp(val, "off") == 0)    return set_device(dev, false);
        else if (strcmp(val, "toggle") == 0) return set_device(dev, !*dev_state(dev));
    }
    return ESP_OK;
}

/* ---------- handlers ---------- */

// Answers CORS preflight requests from the browser
static esp_err_t options_handler(httpd_req_t *req)
{
    add_cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    add_cors(req);
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    update_timezone_offset(req);
    sync_time_from_browser(req);
    return send_status(req);
}

static esp_err_t light_handler(httpd_req_t *req)
{
    add_cors(req);
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    update_timezone_offset(req);
    sync_time_from_browser(req);
    esp_err_t err = apply_state(req, DEV_LIGHT);
    if (err != ESP_OK) return httpd_resp_send_500(req);
    return send_status(req);
}

static esp_err_t fan_handler(httpd_req_t *req)
{
    add_cors(req);
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    update_timezone_offset(req);
    sync_time_from_browser(req);
    esp_err_t err = apply_state(req, DEV_FAN);
    if (err != ESP_OK) return httpd_resp_send_500(req);
    return send_status(req);
}

static esp_err_t light2_handler(httpd_req_t *req)
{
    add_cors(req);
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    update_timezone_offset(req);
    sync_time_from_browser(req);
    esp_err_t err = apply_state(req, DEV_LIGHT2);
    if (err != ESP_OK) return httpd_resp_send_500(req);
    return send_status(req);
}

static esp_err_t socket_handler(httpd_req_t *req)
{
    add_cors(req);
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    update_timezone_offset(req);
    sync_time_from_browser(req);
    esp_err_t err = apply_state(req, DEV_SOCKET);
    if (err != ESP_OK) return httpd_resp_send_500(req);
    return send_status(req);
}

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = method, .handler = h, .user_ctx = NULL };
    httpd_register_uri_handler(server, &u);
}

static void start_webserver(void)
{
    if (server != NULL) return;   // avoid starting twice after a Wi-Fi reconnect

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    if (httpd_start(&server, &config) == ESP_OK) {
        register_uri("/api/status", HTTP_GET,     status_handler);
        register_uri("/api/status", HTTP_OPTIONS, options_handler);
        register_uri("/api/light",  HTTP_POST,    light_handler);
        register_uri("/api/light",  HTTP_OPTIONS, options_handler);
        register_uri("/api/fan",    HTTP_POST,    fan_handler);
        register_uri("/api/fan",    HTTP_OPTIONS, options_handler);
        register_uri("/api/light2", HTTP_POST,    light2_handler);
        register_uri("/api/light2", HTTP_OPTIONS, options_handler);
        register_uri("/api/socket", HTTP_POST,    socket_handler);
        register_uri("/api/socket", HTTP_OPTIONS, options_handler);
        ESP_LOGI(TAG, "API server started");
    }
}

static void start_time_sync(void)
{
    if (sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    sntp_started = true;
}

/* ---------- Wi-Fi ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reconnecting to Wi-Fi...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        start_time_sync();
        start_webserver();
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    history_load();
    ESP_ERROR_CHECK(load_device_states());

    initialize_relay(LIGHT_GPIO, light_state);
    initialize_relay(FAN_GPIO, fan_state);
    initialize_relay(LIGHT2_GPIO, light2_state);
    initialize_relay(SOCKET_GPIO, socket_state);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}
