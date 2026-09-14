/*
 * esp_http_server runs a single worker task, so an SSE handler that looped and blocked would stall
 * every other request. Instead the handler writes the response head, hands its socket to this
 * module and returns; frames are then written asynchronously with httpd_socket_send() from
 * whichever task produced the event.
 */
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "http_iface.h"
#include "http_sse.h"
#include "http_util.h"

static const char *TAG = "http";

#define HEARTBEAT_US (15 * 1000 * 1000)

static const char SSE_HEAD[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    /* Defensive: a reverse proxy in front of the device must not buffer. */
    "X-Accel-Buffering: no\r\n"
    "\r\n"
    ": connected\n\n";

static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_heartbeat;
static int s_clients[HTTP_IFACE_MAX_SSE_CLIENTS];

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/** Caller holds the lock. Returns false when the socket is gone, so the slot can be freed. */
static bool send_raw(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        /* MSG_DONTWAIT is the whole point: a client that stopped reading must not stall the
         * producer, it must be dropped. */
        int n = httpd_socket_send(s_server, fd, data + sent, len - sent, MSG_DONTWAIT);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void drop_locked(size_t slot)
{
    if (s_clients[slot] < 0) {
        return;
    }
    ESP_LOGI(TAG, "sse client on fd %d dropped", s_clients[slot]);
    httpd_sess_trigger_close(s_server, s_clients[slot]);
    s_clients[slot] = -1;
}

static void heartbeat(void *arg)
{
    (void)arg;
    /* A comment line keeps the connection alive through NAT and proxies without being an event. */
    static const char ping[] = ": ping\n\n";
    lock();
    for (size_t i = 0; i < HTTP_IFACE_MAX_SSE_CLIENTS; i++) {
        if (s_clients[i] >= 0 && !send_raw(s_clients[i], ping, sizeof(ping) - 1)) {
            drop_locked(i);
        }
    }
    unlock();
}

esp_err_t http_sse_start(httpd_handle_t server)
{
    s_server = server;
    for (size_t i = 0; i < HTTP_IFACE_MAX_SSE_CLIENTS; i++) {
        s_clients[i] = -1;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = {.callback = heartbeat, .name = "sse_ping"};
    esp_err_t err = esp_timer_create(&args, &s_heartbeat);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_heartbeat, HEARTBEAT_US);
}

void http_sse_stop(void)
{
    if (s_heartbeat != NULL) {
        esp_timer_stop(s_heartbeat);
        esp_timer_delete(s_heartbeat);
        s_heartbeat = NULL;
    }
    if (s_lock == NULL) {
        return;
    }
    lock();
    for (size_t i = 0; i < HTTP_IFACE_MAX_SSE_CLIENTS; i++) {
        drop_locked(i);
    }
    unlock();
}

esp_err_t http_sse_open(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return ESP_FAIL;
    }

    lock();
    size_t slot = HTTP_IFACE_MAX_SSE_CLIENTS;
    for (size_t i = 0; i < HTTP_IFACE_MAX_SSE_CLIENTS; i++) {
        if (s_clients[i] == fd) { /* a reconnect on a reused socket */
            slot = i;
            break;
        }
        if (s_clients[i] < 0 && slot == HTTP_IFACE_MAX_SSE_CLIENTS) {
            slot = i;
        }
    }
    if (slot == HTTP_IFACE_MAX_SSE_CLIENTS) {
        unlock();
        /* Refusing is better than evicting: the UI reconnects, and three is the documented cap. */
        ESP_LOGW(TAG, "sse client refused, %d already connected", HTTP_IFACE_MAX_SSE_CLIENTS);
        return http_send_error(req, GW_ERR_BUS_BUSY, "too many event subscribers");
    }

    s_clients[slot] = fd;
    bool ok = send_raw(fd, SSE_HEAD, sizeof(SSE_HEAD) - 1);
    if (!ok) {
        drop_locked(slot);
    }
    unlock();

    if (!ok) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sse client on fd %d", fd);
    /*
     * Returning here leaves the socket open with the response head already written. httpd goes back
     * to waiting for another request on it, which never comes, and we keep writing frames to it.
     */
    return ESP_OK;
}

void http_sse_broadcast(const char *event, const char *json)
{
    if (s_lock == NULL || event == NULL || json == NULL) {
        return;
    }

    char head[64];
    int hlen = snprintf(head, sizeof(head), "event: %s\ndata: ", event);
    if (hlen < 0 || hlen >= (int)sizeof(head)) {
        return;
    }

    lock();
    for (size_t i = 0; i < HTTP_IFACE_MAX_SSE_CLIENTS; i++) {
        if (s_clients[i] < 0) {
            continue;
        }
        if (!send_raw(s_clients[i], head, (size_t)hlen) ||
            !send_raw(s_clients[i], json, strlen(json)) || !send_raw(s_clients[i], "\n\n", 2)) {
            drop_locked(i);
        }
    }
    unlock();
}
