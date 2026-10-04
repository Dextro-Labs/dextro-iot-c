#ifndef DEXTRO_IOT_IOT_PROCEDURE_H
#define DEXTRO_IOT_IOT_PROCEDURE_H

/*
 * Device-procedures (backend → device). A aplicação registra uma tabela
 * estática; as ops obrigatórias do core já vêm na lib e podem ser
 * substituídas por uma entrada da aplicação com o mesmo nome.
 *
 * O handler escreve exatamente UM valor JSON em `out` (o payload da
 * resposta, normalmente um objeto) e devolve o rescode. Em erro, use
 * iot_reply_error para escrever {error, detail}.
 */

#include <dextro-iot/iot_json.h>
#include <dextro-iot/iot_types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iot_client;

typedef struct {
    const char *op;
    const iot_json_doc_t *doc;  /* a mensagem inteira, já validada */
    iot_json_ref_t payload;     /* o objeto payload do request */
    const char *relation_id;
    int64_t timestamp_ms;       /* do request, normalizado para ms */
} iot_request_t;

typedef int (*iot_procedure_fn)(struct iot_client *c, const iot_request_t *req, iot_json_writer_t *out, void *user);

/* Ops com efeito físico: o device guarda a resposta por idempotencyKey e,
 * na repetição, devolve a mesma sem reexecutar (spec §5, at-most-once). */
#define IOT_OP_IDEMPOTENCY_KEY (1u << 0)

typedef struct {
    const char *op;
    uint16_t version;  /* aparece em list-procedures */
    iot_procedure_fn handler;
    void *user;
    uint32_t flags;
} iot_procedure_t;

/* Escreve {"error": code, "detail": detail?} e devolve `rescode`. */
int iot_reply_error(iot_json_writer_t *out, int rescode, const char *code, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* DEXTRO_IOT_IOT_PROCEDURE_H */
