#ifndef DEXTRO_IOT_IOT_TYPES_H
#define DEXTRO_IOT_IOT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versão do protocolo falada no fio (metadata.v). O backend aceita "0.x"
 * com x >= 4 e rejeita major diferente com 400. */
#define IOT_PROTOCOL_VERSION "0.5"

/* Erros locais da lib (não confundir com rescode, que viaja no fio). */
typedef enum {
    IOT_OK = 0,
    IOT_ERR_ARG,        /* parâmetro inválido */
    IOT_ERR_NO_SPACE,   /* buffer/tabela estática cheia */
    IOT_ERR_PARSE,      /* JSON malformado */
    IOT_ERR_LIMIT,      /* passou de IOT_JSON_MAX_TOKENS/DEPTH ou IOT_MAX_PAYLOAD */
    IOT_ERR_NOT_FOUND,  /* chave/elemento ausente */
    IOT_ERR_TYPE,       /* valor com tipo diferente do pedido */
    IOT_ERR_STATE,      /* chamada fora do estado esperado */
    IOT_ERR_TRANSPORT,  /* falha de rede */
    IOT_ERR_TIMEOUT,
} iot_err_t;

/* Tabela única de rescode (spec §3, atrio-core/src/rescode.rs). */
typedef enum {
    IOT_RES_OK = 200,
    IOT_RES_ACCEPTED = 202,
    IOT_RES_NO_CONTENT = 204,
    IOT_RES_BAD_REQUEST = 400,
    IOT_RES_UNAUTHORIZED = 401,
    IOT_RES_FORBIDDEN = 403,
    IOT_RES_NOT_FOUND = 404,
    IOT_RES_CONFLICT = 409,
    IOT_RES_PAYLOAD_TOO_LARGE = 413,
    IOT_RES_TOO_MANY_REQUESTS = 429,
    IOT_RES_SERVER_ERROR = 500,
    IOT_RES_BAD_GATEWAY = 502,
    IOT_RES_UNAVAILABLE = 503,
    IOT_RES_TIMEOUT = 504,
} iot_rescode_t;

static inline bool iot_rescode_is_success(int rescode)
{
    return rescode >= 200 && rescode < 300;
}

const char *iot_err_str(iot_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* DEXTRO_IOT_IOT_TYPES_H */
