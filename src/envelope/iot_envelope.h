#ifndef DEXTRO_IOT_IOT_ENVELOPE_H
#define DEXTRO_IOT_IOT_ENVELOPE_H

/* Envelope do protocolo (docs/PROTOCOL.md §2): leitura/validação de
 * {metadata, op, payload[, rescode]} e montagem do metadata. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dextro-iot/iot_json.h>
#include <dextro-iot/iot_port.h>

#define IOT_ENV_UUID_LEN 36
#define IOT_ENV_OP_MAX 64
#define IOT_ENV_INSTANCE_MAX 96
#define IOT_ENV_TRACE_MAX 64

typedef struct {
    iot_json_ref_t metadata;
    iot_json_ref_t payload;
    iot_json_ref_t rescode_ref; /* IOT_JSON_NONE em request */
    char op[IOT_ENV_OP_MAX + 1];
    char relation_id[IOT_ENV_UUID_LEN + 8];
    char instance[IOT_ENV_INSTANCE_MAX + 1];
    char traceparent[IOT_ENV_TRACE_MAX + 1]; /* vazio se ausente */
    char serial[24];
    char version[8];
    int64_t timestamp_ms; /* normalizado: < 1e12 era segundos */
    int rescode;          /* -1 em request */
} iot_envelope_t;

typedef enum {
    IOT_ENV_OK = 0,
    IOT_ENV_NO_RELATION, /* sem relationId legível: não há para onde responder */
    IOT_ENV_INVALID,     /* envelope inválido, mas dá para responder 400 */
    IOT_ENV_BAD_VERSION, /* major diferente: 400 UNSUPPORTED_VERSION */
} iot_env_status_t;

/* Lê e valida. `expect_response` exige rescode; sem ele, rescode é proibido
 * (request). Raiz sem chaves extras. */
iot_env_status_t iot_envelope_read(const iot_json_doc_t *doc, bool expect_response, iot_envelope_t *out);

/* UUID v4 em texto (36 + '\0') a partir de 16 bytes aleatórios. */
void iot_uuid_v4(const uint8_t rnd[16], char out[IOT_ENV_UUID_LEN + 1]);

/* Escreve a chave "metadata" e o objeto. `traceparent` pode ser NULL. */
void iot_envelope_write_metadata(iot_json_writer_t *w, const char *message_id, const char *relation_id,
                                 int64_t timestamp_ms, const char *serial, const char *instance,
                                 const char *traceparent);

#endif
