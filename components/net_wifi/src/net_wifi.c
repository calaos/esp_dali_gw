#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "app_config.h"
#include "gw_events.h"
#include "net_wifi.h"

#include "dns_hijack.h"
#include "time_sync.h"

static const char *TAG = "net";

#define NET_TASK_STACK 4096
#define NET_TASK_PRIO 5
#define NET_QUEUE_LEN 8

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000
#define AP_GRACE_MS 30000
#define FALLBACK_DEFAULT_S 60
/** Upper bound on an otherwise idle wait, so a lost event cannot wedge the state machine. */
#define IDLE_WAIT_MS 10000

#define AP_IP "192.168.4.1"
#define AP_NETMASK "255.255.255.0"
#define AP_MAX_CONN 4
/** Inside 1-11, so the SoftAP stays legal under every regulatory domain we may be set to. */
#define AP_CHANNEL 6

/** Used when wifi.country is empty: the channels legal in every regulatory domain. */
#define COUNTRY_WORLD_SAFE "01"

static const char *country_code(void)
{
    const char *c = app_config_get()->wifi.country;
    return c[0] != '\0' ? c : COUNTRY_WORLD_SAFE;
}

#define SCAN_RECORDS_MAX 32
#define SCAN_DONE_BIT BIT0

typedef enum {
    NET_EV_STA_START,
    NET_EV_STA_CONNECTED,
    NET_EV_STA_DISCONNECTED,
    NET_EV_GOT_IP,
} net_ev_t;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static QueueHandle_t s_queue;
static EventGroupHandle_t s_scan_events;
static SemaphoreHandle_t s_scan_lock;

static _Atomic int s_state = GW_NET_BOOT;
static _Atomic bool s_ap_active = false;

/* Owned by the net task. */
static bool s_mdns_up;
static bool s_have_creds;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static int64_t s_retry_at;    /**< Next esp_wifi_connect() attempt; 0 = none pending. */
static int64_t s_fallback_at; /**< Deadline for STA_CONNECTING -> STA_FALLBACK_AP. */
static int64_t s_ap_grace_at; /**< Deadline for tearing the recovery AP down. */

static const char *state_name(gw_net_state_t st)
{
    switch (st) {
        case GW_NET_AP_PROVISIONING:
            return "ap_provisioning";
        case GW_NET_STA_CONNECTING:
            return "sta_connecting";
        case GW_NET_STA_CONNECTED:
            return "sta_connected";
        case GW_NET_STA_FALLBACK_AP:
            return "sta_fallback_ap";
        case GW_NET_BOOT:
        default:
            return "boot";
    }
}

static void current_ip(char *out, size_t len)
{
    gw_net_state_t st = (gw_net_state_t)atomic_load(&s_state);
    esp_netif_t *nif = NULL;
    if (st == GW_NET_STA_CONNECTED) {
        nif = s_sta_netif;
    } else if (atomic_load(&s_ap_active)) {
        nif = s_ap_netif;
    }

    esp_netif_ip_info_t info;
    if (nif == NULL || esp_netif_get_ip_info(nif, &info) != ESP_OK) {
        strlcpy(out, "0.0.0.0", len);
        return;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.ip));
}

static int8_t current_rssi(void)
{
    wifi_ap_record_t rec;
    if ((gw_net_state_t)atomic_load(&s_state) != GW_NET_STA_CONNECTED) {
        return 0;
    }
    return esp_wifi_sta_get_ap_info(&rec) == ESP_OK ? rec.rssi : 0;
}

static void set_state(gw_net_state_t st)
{
    atomic_store(&s_state, st);

    gw_event_net_t ev = {.state = st, .rssi = current_rssi()};
    current_ip(ev.ip, sizeof(ev.ip));
    gw_event_post(GW_EVENT_NET_STATE, &ev, sizeof(ev));
    ESP_LOGI(TAG, "%s (ip %s)", state_name(st), ev.ip);
}

/* --- mDNS ------------------------------------------------------------------------------------- */

static void mdns_start(void)
{
    if (s_mdns_up) {
        return;
    }
    const app_config_t *cfg = app_config_get();

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_up = true;

    mdns_hostname_set(cfg->device.hostname);
    mdns_instance_name_set(cfg->device.name[0] != '\0' ? cfg->device.name : cfg->device.hostname);

    mdns_txt_item_t txt[] = {{"board", "esp32c6"}, {"path", "/"}};
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns service: %s", esp_err_to_name(err));
    }
}

/* --- radio configuration ---------------------------------------------------------------------- */

static esp_err_t apply_static_ip(const app_config_wifi_t *wifi)
{
    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_FALSE(esp_netif_str_to_ip4(wifi->static_ip.ip, &info.ip) == ESP_OK,
                        ESP_ERR_INVALID_ARG, TAG, "static ip");
    ESP_RETURN_ON_FALSE(esp_netif_str_to_ip4(wifi->static_ip.mask, &info.netmask) == ESP_OK,
                        ESP_ERR_INVALID_ARG, TAG, "static mask");
    ESP_RETURN_ON_FALSE(esp_netif_str_to_ip4(wifi->static_ip.gw, &info.gw) == ESP_OK,
                        ESP_ERR_INVALID_ARG, TAG, "static gw");

    /* The DHCP client must be stopped before the netif can hold an address of our choosing. */
    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(err, TAG, "dhcpc_stop");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_sta_netif, &info), TAG, "set_ip_info");

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_str_to_ip4(wifi->static_ip.dns, &dns.ip.u_addr.ip4) == ESP_OK) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns), TAG,
                            "set_dns_info");
    }
    return ESP_OK;
}

static esp_err_t configure_sta(const app_config_t *cfg)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, cfg->wifi.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi.password, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "sta config");
    if (cfg->wifi.static_ip.enabled) {
        ESP_RETURN_ON_ERROR(apply_static_ip(&cfg->wifi), TAG, "static ip");
    }
    return ESP_OK;
}

static esp_err_t configure_ap(const app_config_t *cfg)
{
    char id[8];
    app_config_device_id(id, sizeof(id));

    wifi_config_t wc = {0};
    char ssid[sizeof(wc.ap.ssid) + 1];
    int n = snprintf(ssid, sizeof(ssid), "ESP-DALI-GW-%s", id);
    size_t ssid_len = n < 0 ? 0 : (size_t)n >= sizeof(ssid) ? sizeof(wc.ap.ssid) : (size_t)n;

    memcpy(wc.ap.ssid, ssid, ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /* WPA2 needs 8 characters; a configuration that cannot satisfy that stays open rather than
     * refusing to start, because the AP is the only way back into a misconfigured device. */
    if (strlen(cfg->wifi.ap_password) >= 8) {
        strlcpy((char *)wc.ap.password, cfg->wifi.ap_password, sizeof(wc.ap.password));
    } else {
        ESP_LOGW(TAG, "ap password too short, starting the SoftAP open");
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_netif_ip_info_t info = {0};
    esp_netif_str_to_ip4(AP_IP, &info.ip);
    esp_netif_str_to_ip4(AP_IP, &info.gw);
    esp_netif_str_to_ip4(AP_NETMASK, &info.netmask);

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(err, TAG, "dhcps_stop");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &info), TAG, "ap ip");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "dhcps_start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "ap config");

    ESP_LOGI(TAG, "softap %s on " AP_IP, ssid);
    return ESP_OK;
}

/** The STA interface stays up in AP mode: the setup wizard has to be able to scan. */
static esp_err_t ap_up(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");

    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_ap_netif, &info), TAG, "ap ip info");
    atomic_store(&s_ap_active, true);

    esp_err_t err = dns_hijack_start(info.ip.addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "captive dns: %s", esp_err_to_name(err)); /* the AP itself still works */
    }
    return ESP_OK;
}

static void ap_down(void)
{
    if (!atomic_load(&s_ap_active)) {
        return;
    }
    dns_hijack_stop();
    atomic_store(&s_ap_active, false);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dropping the ap: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "recovery ap torn down");
}

/* --- event plumbing --------------------------------------------------------------------------- */

static void notify(net_ev_t ev)
{
    if (s_queue != NULL) {
        xQueueSend(s_queue, &ev, 0);
    }
}

/* Runs on the default event loop: bookkeeping and a notification only, never a radio call. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
        case WIFI_EVENT_STA_START:
            notify(NET_EV_STA_START);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            notify(NET_EV_STA_CONNECTED);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGI(TAG, "disconnected (reason %d)", ev->reason);
            notify(NET_EV_STA_DISCONNECTED);
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            xEventGroupSetBits(s_scan_events, SCAN_DONE_BIT);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "ap client joined");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "ap client left");
            break;
        default:
            break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        notify(NET_EV_GOT_IP);
    }
}

/* --- boot state machine (SPEC 5.1) ------------------------------------------------------------ */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void arm_fallback(void)
{
    const app_config_t *cfg = app_config_get();
    uint32_t s =
        cfg->wifi.fallback_ap_timeout_s != 0 ? cfg->wifi.fallback_ap_timeout_s : FALLBACK_DEFAULT_S;
    s_fallback_at = now_us() + (int64_t)s * 1000000;
}

static void try_connect(void)
{
    s_retry_at = 0;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        s_retry_at = now_us() + (int64_t)s_backoff_ms * 1000;
    }
}

static void schedule_retry(void)
{
    s_retry_at = now_us() + (int64_t)s_backoff_ms * 1000;
    ESP_LOGD(TAG, "retry in %" PRIu32 " ms", s_backoff_ms);
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void handle_disconnected(void)
{
    if (!s_have_creds) {
        return;
    }
    if ((gw_net_state_t)atomic_load(&s_state) == GW_NET_STA_CONNECTED) {
        s_ap_grace_at = 0; /* the recovery AP stays up as long as the link is down */
        if (atomic_load(&s_ap_active)) {
            set_state(GW_NET_STA_FALLBACK_AP);
        } else {
            arm_fallback(); /* losing an established link restarts the whole cycle */
            set_state(GW_NET_STA_CONNECTING);
        }
    }
    schedule_retry();
}

static void handle_got_ip(void)
{
    time_sync_start();
    s_backoff_ms = BACKOFF_MIN_MS;
    s_retry_at = 0;
    s_fallback_at = 0;
    set_state(GW_NET_STA_CONNECTED);
    mdns_start();

    /* The recovery AP outlives the association for a grace period: whoever is on the wizard page
     * gets to see the result of their own "save" before their client is thrown off. */
    if (atomic_load(&s_ap_active)) {
        s_ap_grace_at = now_us() + (int64_t)AP_GRACE_MS * 1000;
    }
}

static void enter_fallback_ap(void)
{
    s_fallback_at = 0;
    esp_err_t err = ap_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fallback ap: %s", esp_err_to_name(err));
        arm_fallback(); /* keep retrying: without the AP there is no way to fix the credentials */
        return;
    }
    set_state(GW_NET_STA_FALLBACK_AP);
}

/** Earliest pending deadline, clamped so the task always wakes up eventually. */
static TickType_t wait_ticks(void)
{
    int64_t next = 0;
    const int64_t deadlines[] = {s_retry_at, s_fallback_at, s_ap_grace_at};
    for (size_t i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
        if (deadlines[i] != 0 && (next == 0 || deadlines[i] < next)) {
            next = deadlines[i];
        }
    }
    if (next == 0) {
        return pdMS_TO_TICKS(IDLE_WAIT_MS);
    }
    int64_t ms = (next - now_us()) / 1000;
    if (ms < 0) {
        ms = 0;
    }
    return pdMS_TO_TICKS(ms > IDLE_WAIT_MS ? IDLE_WAIT_MS : (uint32_t)ms);
}

static void run_deadlines(void)
{
    int64_t t = now_us();
    gw_net_state_t st = (gw_net_state_t)atomic_load(&s_state);

    if (s_retry_at != 0 && t >= s_retry_at) {
        try_connect();
    }
    if (s_fallback_at != 0 && t >= s_fallback_at && st == GW_NET_STA_CONNECTING) {
        enter_fallback_ap();
    }
    if (s_ap_grace_at != 0 && t >= s_ap_grace_at) {
        s_ap_grace_at = 0;
        ap_down();
    }
}

static esp_err_t start_radio(void)
{
    const app_config_t *cfg = app_config_get();

    /* Both interfaces are configured while the radio is still stopped, so that raising the
     * recovery AP later is a mode change and nothing else — no beacon ever carries the driver's
     * default SSID. wifi.* changes need a reboot anyway (app_config impact), so this runs once. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(configure_ap(cfg), TAG, "ap config");
    if (s_have_creds) {
        ESP_RETURN_ON_ERROR(configure_sta(cfg), TAG, "sta config");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    if (s_have_creds) {
        arm_fallback();
        set_state(GW_NET_STA_CONNECTING);
    } else {
        ESP_RETURN_ON_ERROR(ap_up(), TAG, "ap up");
        set_state(GW_NET_AP_PROVISIONING);
    }
    mdns_start();
    return ESP_OK;
}

static void net_task(void *arg)
{
    (void)arg;

    esp_err_t err = start_radio();
    if (err != ESP_OK) {
        /* A radio that will not start must not take the DALI side of the gateway down with it. */
        ESP_LOGE(TAG, "radio start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        net_ev_t ev;
        if (xQueueReceive(s_queue, &ev, wait_ticks()) == pdTRUE) {
            switch (ev) {
                case NET_EV_STA_START:
                    if (s_have_creds) {
                        try_connect();
                    }
                    break;
                case NET_EV_STA_CONNECTED:
                    /* Associated: whatever made the previous attempts fail is gone. */
                    s_backoff_ms = BACKOFF_MIN_MS;
                    s_retry_at = 0;
                    break;
                case NET_EV_STA_DISCONNECTED:
                    handle_disconnected();
                    break;
                case NET_EV_GOT_IP:
                    handle_got_ip();
                    break;
            }
        }
        run_deadlines();
    }
}

/* --- public API ------------------------------------------------------------------------------- */

esp_err_t net_wifi_init(void)
{
    const app_config_t *cfg = app_config_get();
    s_have_creds = cfg->wifi.ssid[0] != '\0';

    s_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(net_ev_t));
    s_scan_events = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_queue && s_scan_events && s_scan_lock, ESP_ERR_NO_MEM, TAG, "no mem");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && s_ap_netif, ESP_FAIL, TAG, "netif");

    esp_netif_set_hostname(s_sta_netif, cfg->device.hostname);
    esp_netif_set_hostname(s_ap_netif, cfg->device.hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");

    /* The whole configuration lives in app_config; the Wi-Fi NVS copy must never win over it. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_country_code(country_code(), true), TAG, "country");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
        "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL), TAG,
        "ip handler");

    ESP_RETURN_ON_FALSE(xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIO, NULL) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "net task");
    return ESP_OK;
}

void net_wifi_get_status(net_wifi_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    gw_net_state_t st = (gw_net_state_t)atomic_load(&s_state);
    out->state = st;
    out->rssi = current_rssi();
    current_ip(out->ip, sizeof(out->ip));
    strlcpy(out->hostname, app_config_get()->device.hostname, sizeof(out->hostname));

    /* The radio is in APSTA during provisioning too — the wizard needs the STA side to scan — but
     * what /api/info is asked to describe is the role, which is a plain AP until a STA is wanted.
     */
    const char *mode = "sta";
    if (st == GW_NET_AP_PROVISIONING) {
        mode = "ap";
    } else if (atomic_load(&s_ap_active)) {
        mode = "apsta";
    }
    strlcpy(out->mode, mode, sizeof(out->mode));
}

gw_net_state_t net_wifi_state(void)
{
    return (gw_net_state_t)atomic_load(&s_state);
}

bool net_wifi_ap_active(void)
{
    return atomic_load(&s_ap_active);
}

/** The closed set of SPEC 9 / net_wifi.h; anything exotic is reported as the WPA2 it behaves like
 * for a client that only knows how to type a passphrase. */
static const char *auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_OWE:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa_wpa2";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2_wpa3";
        default:
            return "wpa2";
    }
}

static void collect(const wifi_ap_record_t *rec, net_wifi_ap_t *out, size_t max, size_t *found)
{
    const char *ssid = (const char *)rec->ssid;
    if (ssid[0] == '\0') {
        return; /* hidden network: nothing the wizard could show or let the user pick */
    }
    for (size_t i = 0; i < *found; i++) {
        if (strcmp(out[i].ssid, ssid) == 0) {
            if (rec->rssi > out[i].rssi) {
                out[i].rssi = rec->rssi;
                out[i].channel = rec->primary;
                strlcpy(out[i].auth, auth_name(rec->authmode), sizeof(out[i].auth));
            }
            return;
        }
    }
    if (*found >= max) {
        return;
    }
    net_wifi_ap_t *ap = &out[(*found)++];
    strlcpy(ap->ssid, ssid, sizeof(ap->ssid));
    ap->rssi = rec->rssi;
    ap->channel = rec->primary;
    strlcpy(ap->auth, auth_name(rec->authmode), sizeof(ap->auth));
}

static esp_err_t scan_locked(net_wifi_ap_t *out, size_t max, size_t *found, uint32_t timeout_ms)
{
    xEventGroupClearBits(s_scan_events, SCAN_DONE_BIT);

    wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan, false), TAG, "scan start");

    EventBits_t bits = xEventGroupWaitBits(s_scan_events, SCAN_DONE_BIT, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & SCAN_DONE_BIT) == 0) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        return ESP_ERR_TIMEOUT;
    }

    uint16_t num = SCAN_RECORDS_MAX;
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    ESP_RETURN_ON_FALSE(recs != NULL, ESP_ERR_NO_MEM, TAG, "scan buffer");

    esp_err_t err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < num; i++) {
            collect(&recs[i], out, max, found);
        }
    }
    free(recs);
    return err;
}

esp_err_t net_wifi_scan(net_wifi_ap_t *out, size_t max, size_t *found, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(out != NULL && found != NULL && max > 0, ESP_ERR_INVALID_ARG, TAG, "args");
    *found = 0;
    ESP_RETURN_ON_FALSE(s_scan_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int64_t left_ms = (deadline - now_us()) / 1000;
    esp_err_t err = left_ms > 0 ? scan_locked(out, max, found, (uint32_t)left_ms) : ESP_ERR_TIMEOUT;

    xSemaphoreGive(s_scan_lock);
    return err;
}
