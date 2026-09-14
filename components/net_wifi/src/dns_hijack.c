#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "dns_hijack.h"

static const char *TAG = "net";

#define DNS_PORT 53
#define DNS_MSG_MAX 512 /**< Anything larger is a TCP/EDNS query we are not required to serve. */
#define DNS_TTL_S 60
#define DNS_RECV_TIMEOUT_MS 500
#define DNS_JOIN_TIMEOUT_MS 2000
#define DNS_TASK_STACK 3072
#define DNS_TASK_PRIO 4

static int s_sock = -1;
static volatile bool s_run;
static SemaphoreHandle_t s_exited;
static uint32_t s_addr_be;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/**
 * @return length of the reply in @p out, or -1 when the query must be ignored.
 *
 * Non-A queries get a NOERROR/no-answer reply rather than silence: a client that keeps waiting for
 * its AAAA probe never opens the portal page.
 */
static int build_reply(const uint8_t *q, int qlen, uint8_t *out, int cap)
{
    if (qlen < 12 || cap < qlen) {
        return -1;
    }
    uint16_t flags = rd16(q + 2);
    if ((flags & 0x8000) != 0 || rd16(q + 4) != 1) {
        return -1;
    }

    int p = 12;
    while (p < qlen && q[p] != 0) {
        if ((q[p] & 0xc0) != 0) { /* a compression pointer inside a question is malformed */
            return -1;
        }
        p += q[p] + 1;
    }
    if (p + 5 > qlen) {
        return -1;
    }
    p += 5;
    uint16_t qtype = rd16(q + p - 4);
    uint16_t qclass = rd16(q + p - 2);

    memcpy(out, q, (size_t)p);
    wr16(out + 2, (uint16_t)(0x8400 | (flags & 0x0100))); /* QR + AA, echo RD */
    wr16(out + 6, 0);
    wr16(out + 8, 0);
    wr16(out + 10, 0);

    if (qtype != 1 || qclass != 1 || p + 16 > cap) {
        return p;
    }

    uint8_t *a = out + p;
    wr16(a, 0xc00c); /* name: pointer back to the question */
    wr16(a + 2, 1);
    wr16(a + 4, 1);
    a[6] = 0;
    a[7] = 0;
    wr16(a + 8, DNS_TTL_S);
    wr16(a + 10, 4);
    memcpy(a + 12, &s_addr_be, 4);
    wr16(out + 6, 1);
    return p + 16;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_MSG_MAX];
    uint8_t tx[DNS_MSG_MAX];

    while (s_run) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue; /* receive timeout, which is how this task notices s_run going false */
        }
        int n = build_reply(rx, len, tx, sizeof(tx));
        if (n > 0) {
            sendto(s_sock, tx, (size_t)n, 0, (struct sockaddr *)&from, from_len);
        }
    }

    close(s_sock);
    s_sock = -1;
    xSemaphoreGive(s_exited);
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(uint32_t addr_be)
{
    if (s_run) {
        return ESP_OK;
    }

    s_exited = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_exited != NULL, ESP_ERR_NO_MEM, TAG, "dns sem");

    esp_err_t ret = ESP_OK;
    s_addr_be = addr_be;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_GOTO_ON_FALSE(s_sock >= 0, ESP_FAIL, fail, TAG, "dns socket: %d", errno);

    struct timeval tv = {.tv_sec = 0, .tv_usec = DNS_RECV_TIMEOUT_MS * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    ESP_GOTO_ON_FALSE(bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0, ESP_FAIL, fail,
                      TAG, "dns bind: %d", errno);

    s_run = true;
    ESP_GOTO_ON_FALSE(
        xTaskCreate(dns_task, "dns_hijack", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, fail, TAG, "dns task");
    return ESP_OK;

fail:
    s_run = false;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    vSemaphoreDelete(s_exited);
    s_exited = NULL;
    return ret;
}

void dns_hijack_stop(void)
{
    if (!s_run) {
        return;
    }
    s_run = false;
    if (xSemaphoreTake(s_exited, pdMS_TO_TICKS(DNS_JOIN_TIMEOUT_MS)) != pdTRUE) {
        /* The task still owns both the semaphore and the socket; freeing either is worse. */
        ESP_LOGE(TAG, "dns responder did not exit");
        return;
    }
    vSemaphoreDelete(s_exited);
    s_exited = NULL;
}
