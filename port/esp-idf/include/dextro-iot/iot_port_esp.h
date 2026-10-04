#ifndef DEXTRO_IOT_IOT_PORT_ESP_H
#define DEXTRO_IOT_IOT_PORT_ESP_H

/*
 * Port ESP-IDF da dextro-iot-c: transporte mTLS por esp-tls, relógio
 * (esp_timer + hora de parede do SNTP), RNG de hardware e log.
 *
 * Os PEMs precisam continuar vivos enquanto o transporte existir (o esp-tls
 * os lê a cada conexão) e terminados em '\0'.
 */

#include <dextro-iot/iot_port.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ca_pem;   /* CA que assina o broker (ex.: Dextro Labs Device CA) */
    const char *cert_pem; /* certificado do device (CN = SN) */
    const char *key_pem;  /* chave privada do device */
    void *tls;            /* esp_tls_t*, interno */
    int fd;
} iot_esp_tls_t;

void iot_esp_transport(iot_esp_tls_t *t, iot_transport_t *out);
void iot_esp_clock(iot_clock_t *out);
void iot_esp_rng(iot_rng_t *out);
void iot_esp_log(iot_log_t *out);

#ifdef __cplusplus
}
#endif

#endif
