#ifndef DEXTRO_IOT_IOT_JSON_H
#define DEXTRO_IOT_IOT_JSON_H

/*
 * JSON estático para MCU (spec §12): leitor tokenizado sobre o jsmn, com
 * limite de tokens e de profundidade, e um escritor que monta direto num
 * buffer do chamador. Nenhum dos dois aloca memória.
 *
 * O leitor não copia a mensagem: os tokens apontam para o texto original,
 * que precisa continuar vivo enquanto o documento for usado.
 */

#include <dextro-iot/iot_config.h>
#include <dextro-iot/iot_types.h>

/* Só o tipo do token é usado aqui; a implementação do jsmn é compilada uma
 * vez em src/json/jsmn_impl.c. O JSMN_HEADER não vaza para quem inclui. */
#ifndef JSMN_H
#define JSMN_HEADER
#include "jsmn.h"
#undef JSMN_HEADER
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef jsmntok_t iot_json_tok_t;

typedef enum {
    IOT_JSON_INVALID = 0,
    IOT_JSON_OBJECT,
    IOT_JSON_ARRAY,
    IOT_JSON_STRING,
    IOT_JSON_NUMBER,
    IOT_JSON_BOOL,
    IOT_JSON_NULL,
} iot_json_type_t;

/* Índice de token; negativo = ausente. */
typedef int iot_json_ref_t;
#define IOT_JSON_NONE (-1)

typedef struct {
    const char *json;
    size_t len;
    iot_json_tok_t *toks;
    int ntoks;
} iot_json_doc_t;

/*
 * Tokeniza `json[0..len)` usando `toks[0..max_toks)` como armazenamento.
 * Exige um único valor raiz (objeto ou array) ocupando o texto inteiro, com
 * espaço em branco ao redor permitido.
 *
 * Erros: IOT_ERR_PARSE (malformado ou truncado), IOT_ERR_LIMIT (tokens ou
 * profundidade acima do limite, ou len > IOT_MAX_PAYLOAD).
 */
iot_err_t iot_json_parse(iot_json_doc_t *doc, const char *json, size_t len, iot_json_tok_t *toks,
                         size_t max_toks);

static inline iot_json_ref_t iot_json_root(const iot_json_doc_t *doc)
{
    return doc->ntoks > 0 ? 0 : IOT_JSON_NONE;
}

iot_json_type_t iot_json_type(const iot_json_doc_t *doc, iot_json_ref_t ref);

/* Número de membros de um objeto ou de elementos de um array; -1 se `ref`
 * não for nenhum dos dois. */
int iot_json_size(const iot_json_doc_t *doc, iot_json_ref_t ref);

/* Valor da chave `key` no objeto `obj`, ou IOT_JSON_NONE. A comparação é
 * byte a byte com a chave crua (sem decodificar escapes). */
iot_json_ref_t iot_json_get(const iot_json_doc_t *doc, iot_json_ref_t obj, const char *key);

/* i-ésimo elemento do array `arr`, ou IOT_JSON_NONE. */
iot_json_ref_t iot_json_at(const iot_json_doc_t *doc, iot_json_ref_t arr, int index);

/* Itera membros de um objeto: começa com *key = IOT_JSON_NONE e chama até
 * retornar false. Em cada passo, *key é o token da chave e *value o do valor. */
bool iot_json_next_member(const iot_json_doc_t *doc, iot_json_ref_t obj, iot_json_ref_t *key,
                          iot_json_ref_t *value);

/* Texto cru do valor (para strings, sem as aspas e sem decodificar). Serve
 * para repassar um subobjeto (ex.: o `payload`) adiante sem copiá-lo. */
iot_err_t iot_json_raw(const iot_json_doc_t *doc, iot_json_ref_t ref, const char **text, size_t *len);

/* true se `ref` é uma string cujo texto cru é exatamente `s`. */
bool iot_json_str_eq(const iot_json_doc_t *doc, iot_json_ref_t ref, const char *s);

/* Copia a string decodificada (escapes e \uXXXX → UTF-8) para `out`, sempre
 * terminada em '\0'. IOT_ERR_NO_SPACE se não couber; `out_len` (opcional)
 * recebe o tamanho sem o terminador. */
iot_err_t iot_json_get_str(const iot_json_doc_t *doc, iot_json_ref_t ref, char *out, size_t cap,
                           size_t *out_len);

/* Número inteiro (sem fração nem expoente) que caiba em int64. */
iot_err_t iot_json_get_i64(const iot_json_doc_t *doc, iot_json_ref_t ref, int64_t *out);

iot_err_t iot_json_get_bool(const iot_json_doc_t *doc, iot_json_ref_t ref, bool *out);

/* ---------------------------------------------------------------- escritor */

/*
 * Monta JSON em `buf`. As vírgulas são automáticas. Qualquer estouro (buffer
 * ou profundidade) ou uso fora de ordem marca o escritor como falho: as
 * chamadas seguintes viram no-op e iot_jw_finish retorna erro. Assim o
 * chamador encadeia tudo e confere uma vez só no fim.
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    uint8_t depth;
    bool after_key;    /* próximo item é o valor de uma chave */
    bool failed;
    iot_err_t err;
    uint8_t kind[IOT_JSON_MAX_DEPTH];  /* '{' ou '[' por nível */
    bool has_items[IOT_JSON_MAX_DEPTH];
} iot_json_writer_t;

void iot_jw_init(iot_json_writer_t *w, char *buf, size_t cap);
void iot_jw_obj_begin(iot_json_writer_t *w);
void iot_jw_obj_end(iot_json_writer_t *w);
void iot_jw_arr_begin(iot_json_writer_t *w);
void iot_jw_arr_end(iot_json_writer_t *w);
void iot_jw_key(iot_json_writer_t *w, const char *key);
void iot_jw_str(iot_json_writer_t *w, const char *s);
void iot_jw_str_n(iot_json_writer_t *w, const char *s, size_t n);
void iot_jw_i64(iot_json_writer_t *w, int64_t v);
void iot_jw_u64(iot_json_writer_t *w, uint64_t v);
void iot_jw_bool(iot_json_writer_t *w, bool v);
void iot_jw_null(iot_json_writer_t *w);
/* Insere JSON já pronto como um valor (ex.: payload recebido). Não valida. */
void iot_jw_raw(iot_json_writer_t *w, const char *json, size_t n);

/* Fecha o documento: exige todos os níveis fechados e escreve o '\0'.
 * `out_len` (opcional) recebe o tamanho sem o terminador. */
iot_err_t iot_jw_finish(iot_json_writer_t *w, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEXTRO_IOT_IOT_JSON_H */
