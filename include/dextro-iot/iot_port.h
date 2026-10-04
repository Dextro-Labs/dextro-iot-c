#ifndef DEXTRO_IOT_IOT_PORT_H
#define DEXTRO_IOT_IOT_PORT_H

/*
 * O que a plataforma entrega à lib (vtables). A lib não abre socket, não lê
 * relógio nem sorteia número por conta própria: tudo passa por aqui, o que
 * permite rodar o mesmo código no ESP32, no STM32 e no host (testes com
 * transporte e relógio falsos).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transporte de bytes já autenticado (TLS no device; nos testes, um roteiro).
 * send/recv NÃO bloqueiam além de alguns ms: devolvem 0 quando não há o que
 * fazer agora e < 0 quando a conexão caiu. */
typedef struct {
    void *ctx;
    int (*connect)(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms); /* 0 = ok */
    int32_t (*send)(void *ctx, const void *buf, size_t len);
    int32_t (*recv)(void *ctx, void *buf, size_t len);
    void (*close)(void *ctx);
} iot_transport_t;

typedef struct {
    void *ctx;
    /* relógio monotônico em ms (não volta, não pula) */
    uint64_t (*mono_ms)(void *ctx);
    /* hora de parede em epoch ms, ou 0 se ainda não sincronizada (SNTP). Sem
     * ela, a lib usa o horário dos servidores aprendido nas respostas. */
    int64_t (*epoch_ms)(void *ctx);
} iot_clock_t;

typedef struct {
    void *ctx;
    void (*fill)(void *ctx, void *buf, size_t len); /* RNG de hardware no MCU (spec §12) */
} iot_rng_t;

typedef enum { IOT_LOG_ERROR, IOT_LOG_WARN, IOT_LOG_INFO, IOT_LOG_DEBUG } iot_log_level_t;

typedef struct {
    void *ctx;
    void (*write)(void *ctx, iot_log_level_t level, const char *msg); /* opcional */
} iot_log_t;

#ifdef __cplusplus
}
#endif

#endif /* DEXTRO_IOT_IOT_PORT_H */
