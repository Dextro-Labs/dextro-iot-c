#include <dextro-iot/iot_port_esp.h>

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "lwip/sockets.h"

static const char *TAG = "dextro-iot";

/* Leitura que cabe dentro de um iot_step: espera no máximo isso por um
 * registro TLS que já começou a chegar. */
#define RECV_TIMEOUT_MS 50

/* ------------------------------------------------------------- transporte */

static int tls_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms)
{
    iot_esp_tls_t *t = ctx;
    if (t->tls != NULL) {
        esp_tls_conn_destroy(t->tls);
        t->tls = NULL;
    }
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        return -1;
    }
    const esp_tls_cfg_t cfg = {
        .cacert_buf = (const unsigned char *)t->ca_pem,
        .cacert_bytes = strlen(t->ca_pem) + 1,
        .clientcert_buf = (const unsigned char *)t->cert_pem,
        .clientcert_bytes = strlen(t->cert_pem) + 1,
        .clientkey_buf = (const unsigned char *)t->key_pem,
        .clientkey_bytes = strlen(t->key_pem) + 1,
        .timeout_ms = (int)timeout_ms,
    };
    if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, tls) != 1) {
        int sock_err = 0;
        esp_tls_error_handle_t h = NULL;
        if (esp_tls_get_error_handle(tls, &h) == ESP_OK && h != NULL) {
            esp_tls_get_and_clear_last_error(h, &sock_err, NULL);
        }
        ESP_LOGW(TAG, "TLS com %s:%u falhou (errno %d)", host, port, sock_err);
        esp_tls_conn_destroy(tls);
        return -1;
    }
    int fd = -1;
    esp_tls_get_conn_sockfd(tls, &fd);
    const struct timeval tv = {.tv_sec = 0, .tv_usec = RECV_TIMEOUT_MS * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    t->tls = tls;
    t->fd = fd;
    return 0;
}

static int32_t tls_send(void *ctx, const void *buf, size_t len)
{
    iot_esp_tls_t *t = ctx;
    if (t->tls == NULL) {
        return -1;
    }
    const ssize_t n = esp_tls_conn_write(t->tls, buf, len);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    return n < 0 ? -1 : (int32_t)n;
}

/* Não bloqueia: sem bytes decifrados pendentes nem nada no socket, volta 0. */
static int32_t tls_recv(void *ctx, void *buf, size_t len)
{
    iot_esp_tls_t *t = ctx;
    if (t->tls == NULL) {
        return -1;
    }
    if (esp_tls_get_bytes_avail(t->tls) <= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(t->fd, &rfds);
        struct timeval zero = {0};
        const int r = select(t->fd + 1, &rfds, NULL, NULL, &zero);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
    }
    const ssize_t n = esp_tls_conn_read(t->tls, buf, len);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (n == 0) {
        return -1; /* o broker fechou */
    }
    return n < 0 ? -1 : (int32_t)n;
}

static void tls_close(void *ctx)
{
    iot_esp_tls_t *t = ctx;
    if (t->tls != NULL) {
        esp_tls_conn_destroy(t->tls);
        t->tls = NULL;
        t->fd = -1;
    }
}

void iot_esp_transport(iot_esp_tls_t *t, iot_transport_t *out)
{
    t->tls = NULL;
    t->fd = -1;
    *out = (iot_transport_t){
        .ctx = t,
        .connect = tls_connect,
        .send = tls_send,
        .recv = tls_recv,
        .close = tls_close,
    };
}

/* --------------------------------------------------------- relógio e RNG */

static uint64_t mono_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static int64_t epoch_ms(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return ms > 1600000000000LL ? ms : 0; /* antes do SNTP o relógio está em 1970 */
}

void iot_esp_clock(iot_clock_t *out)
{
    *out = (iot_clock_t){.mono_ms = mono_ms, .epoch_ms = epoch_ms};
}

static void fill(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
}

void iot_esp_rng(iot_rng_t *out)
{
    *out = (iot_rng_t){.fill = fill};
}

static void log_write(void *ctx, iot_log_level_t level, const char *msg)
{
    (void)ctx;
    switch (level) {
    case IOT_LOG_ERROR:
        ESP_LOGE(TAG, "%s", msg);
        break;
    case IOT_LOG_WARN:
        ESP_LOGW(TAG, "%s", msg);
        break;
    case IOT_LOG_INFO:
        ESP_LOGI(TAG, "%s", msg);
        break;
    default:
        ESP_LOGD(TAG, "%s", msg);
        break;
    }
}

void iot_esp_log(iot_log_t *out)
{
    *out = (iot_log_t){.write = log_write};
}
