#ifndef DEXTRO_IOT_IOT_H
#define DEXTRO_IOT_IOT_H

/*
 * Cliente Dextro IoT (spec v0.5, contrato do dextrolabs-device). Sem malloc e
 * sem task própria: a aplicação chama iot_step() periodicamente (ex.: a cada
 * 20 ms) numa task só, que é a dona do cliente. A lib não é thread-safe.
 *
 * Sessão: MQTT 3.1.1 (coreMQTT) sobre o transporte da aplicação, clientId =
 * SN, LWT retido {"online":false} em atrio/{sn}/status, re-subscribe e
 * {"online":true} retido a cada conexão, reconexão com backoff exponencial e
 * jitter, keepalive de aplicação com disciplina de relógio (serverTime).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dextro-iot/iot_config.h>
#include <dextro-iot/iot_json.h>
#include <dextro-iot/iot_port.h>
#include <dextro-iot/iot_procedure.h>
#include <dextro-iot/iot_types.h>

#include "core_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IOT_MAX_PENDING
#define IOT_MAX_PENDING 8u /* remote-procedures aguardando resposta */
#endif
#ifndef IOT_IDEM_SLOTS
#define IOT_IDEM_SLOTS 4u
#endif
#ifndef IOT_IDEM_RESP_MAX
#define IOT_IDEM_RESP_MAX 384u
#endif

#define IOT_SN_LEN 12
#define IOT_UUID_LEN 36
#define IOT_TOPIC_MAX 128

typedef enum {
    IOT_STATE_STOPPED,
    IOT_STATE_BACKOFF,    /* esperando para tentar de novo */
    IOT_STATE_CONNECTING, /* transporte + CONNECT */
    IOT_STATE_SUBSCRIBING,
    IOT_STATE_ONLINE,
} iot_state_t;

typedef enum {
    IOT_EVENT_ONLINE,       /* sessão de pé (assinaturas confirmadas, presença publicada) */
    IOT_EVENT_OFFLINE,      /* sessão caiu; a lib reconecta sozinha */
    IOT_EVENT_KEEPALIVE_OK, /* keepalive respondido: relógio disciplinado */
    IOT_EVENT_SETTINGS_RESYNC,
    IOT_EVENT_OUTBOX_NOTIFIED, /* outbox-notification recebido: drenar a outbox */
} iot_event_t;

struct iot_client;
typedef struct iot_client iot_client_t;

/* Resposta de remote-procedure. `doc` é NULL quando não houve resposta
 * (timeout local: rescode 504) ou quando a sessão caiu (503). */
typedef void (*iot_reply_cb)(iot_client_t *c, int rescode, const iot_json_doc_t *doc, iot_json_ref_t payload,
                             void *user);

/* Escreve membros adicionais (chave + valor) dentro de um objeto já aberto. */
typedef void (*iot_fill_cb)(iot_client_t *c, iot_json_writer_t *w, void *user);

typedef struct {
    const char *serial; /* 12 hex maiúsculos */
    const char *host;
    uint16_t port;

    iot_transport_t transport;
    iot_clock_t clock;
    iot_rng_t rng;
    iot_log_t log;

    /* Buffers da aplicação (sem malloc): rede (pacote MQTT inteiro, entrada e
     * saída) ≥ IOT_MAX_PAYLOAD + 256; mensagem de saída ≥ IOT_MAX_PAYLOAD;
     * mensagem de entrada copiada para processar fora do callback do MQTT. */
    uint8_t *net_buf;
    size_t net_len;
    char *out_buf;
    size_t out_len;
    char *in_buf;
    size_t in_len;
    iot_json_tok_t *toks;
    size_t ntoks;

    const iot_procedure_t *procedures; /* da aplicação; podem substituir as do core */
    size_t procedure_count;
    const char *device_type;  /* list-procedures.deviceType (opcional) */
    uint16_t contract_version;/* list-procedures.contractVersion (0 = omite) */

    uint16_t keepalive_s;        /* MQTT; 0 = 15 */
    uint32_t app_keepalive_ms;   /* op keepalive; 0 = 60000 */
    uint32_t connect_timeout_ms; /* 0 = 10000 */

    iot_fill_cb keepalive_fill; /* campos do keepalive (uptimeS, fwVersion, ...) */
    iot_fill_cb status_fill;    /* get-status padrão */
    void (*on_event)(iot_client_t *c, iot_event_t ev, void *user);
    void *user;
} iot_config_t;

typedef struct {
    char relation_id[IOT_UUID_LEN + 1];
    uint64_t deadline_ms;
    iot_reply_cb cb;
    void *user;
    bool used;
} iot_pending_t;

typedef struct {
    char key[65];
    char response[IOT_IDEM_RESP_MAX]; /* payload JSON já escrito */
    int rescode;
    uint64_t at_ms;
    bool used;
} iot_idem_t;

struct iot_client {
    iot_config_t cfg;
    iot_state_t state;
    MQTTContext_t mqtt;
    MQTTPubAckInfo_t out_records[IOT_MAX_PENDING + 4];
    MQTTPubAckInfo_t in_records[4];
    struct NetworkContext *net_ctx;
    uint64_t next_attempt_ms;
    uint32_t backoff_ms;
    uint64_t phase_started_ms;
    uint16_t sub_packet_id;
    uint64_t next_keepalive_ms;
    int64_t clock_offset_ms; /* hora do servidor - monotônico, quando aprendida */
    bool clock_known;
    bool in_pending; /* há mensagem copiada em in_buf para processar */
    size_t in_size;
    int in_kind;     /* 1 = procedure, 2 = service/reply */
    iot_pending_t pending[IOT_MAX_PENDING];
    iot_idem_t idem[IOT_IDEM_SLOTS];
    char topic_procedure[IOT_TOPIC_MAX];
    char topic_service[IOT_TOPIC_MAX];
    char topic_service_reply[IOT_TOPIC_MAX];
    char topic_status[IOT_TOPIC_MAX];
    uint32_t connects;  /* sessões abertas desde o init (diagnóstico) */
};

iot_err_t iot_init(iot_client_t *c, const iot_config_t *cfg);
/* Começa a conectar (no próximo iot_step). */
iot_err_t iot_start(iot_client_t *c);
/* Bomba: conexão, MQTT, mensagens, timeouts, keepalive. Não bloqueia além do
 * timeout de conexão (só durante CONNECTING). */
void iot_step(iot_client_t *c);
/* Publica online:false retido, DISCONNECT e para de reconectar. */
void iot_stop(iot_client_t *c);
iot_state_t iot_state(const iot_client_t *c);
const char *iot_state_str(iot_state_t st);

/* Hora para o envelope: parede se houver, senão a do servidor aprendida;
 * 0 se nenhuma das duas. */
int64_t iot_now_epoch_ms(const iot_client_t *c);

/* Remote-procedure (device → backend), não bloqueante. `fill` escreve os
 * membros do payload (pode ser NULL: payload {}). timeout 0 = 30000 ms. */
iot_err_t iot_call_service(iot_client_t *c, const char *op, iot_fill_cb fill, void *fill_user, iot_reply_cb cb,
                           void *user, uint32_t timeout_ms);

/* Força um keepalive agora (ex.: logo após trocar o certificado). */
void iot_keepalive_now(iot_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* DEXTRO_IOT_IOT_H */
