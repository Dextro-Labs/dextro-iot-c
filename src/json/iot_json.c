#include <dextro-iot/iot_json.h>

#include <string.h>

/*
 * Leitor
 *
 * O jsmn tokeniza, mas é permissivo: aceita primitivos como "tru" ou "01",
 * vírgula sobrando e mais de um valor na raiz. Depois dele, uma segunda
 * passada confere a gramática completa a partir dos tokens: separadores entre
 * membros, chaves sempre string, primitivos válidos e strings sem caractere de
 * controle cru. Payload malformado vira IOT_ERR_PARSE, nunca comportamento
 * indefinido (spec §9: "payload malformado nunca derruba o processo").
 */

/* Início/fim do token incluindo as aspas, no caso de string. */
static int outer_start(const iot_json_tok_t *t)
{
    return t->type == JSMN_STRING ? t->start - 1 : t->start;
}

static int outer_end(const iot_json_tok_t *t)
{
    return t->type == JSMN_STRING ? t->end + 1 : t->end;
}

/* Primeiro token depois da subárvore de `i`. */
static int skip(const iot_json_doc_t *doc, int i)
{
    const int end = outer_end(&doc->toks[i]);
    int j = i + 1;
    while (j < doc->ntoks && outer_start(&doc->toks[j]) < end) {
        j++;
    }
    return j;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* [from, to) só tem espaço em branco, com no máximo um `sep` (obrigatório se
 * sep != 0). */
static bool gap_ok(const char *s, int from, int to, char sep)
{
    bool seen = false;
    if (from > to) {
        return false;
    }
    for (int i = from; i < to; i++) {
        if (is_ws(s[i])) {
            continue;
        }
        if (sep != 0 && s[i] == sep && !seen) {
            seen = true;
            continue;
        }
        return false;
    }
    return sep == 0 || seen;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Gramática de número do RFC 8259: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)? */
static bool number_ok(const char *s, size_t n)
{
    size_t i = 0;
    if (i < n && s[i] == '-') {
        i++;
    }
    if (i >= n) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && is_digit(s[i])) {
            i++;
        }
    } else {
        return false;
    }
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || !is_digit(s[i])) {
            return false;
        }
        while (i < n && is_digit(s[i])) {
            i++;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (i >= n || !is_digit(s[i])) {
            return false;
        }
        while (i < n && is_digit(s[i])) {
            i++;
        }
    }
    return i == n;
}

static bool primitive_ok(const char *s, size_t n)
{
    if ((n == 4 && memcmp(s, "true", 4) == 0) || (n == 5 && memcmp(s, "false", 5) == 0) ||
        (n == 4 && memcmp(s, "null", 4) == 0)) {
        return true;
    }
    return number_ok(s, n);
}

static bool string_ok(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20) {
            return false;
        }
    }
    return true;
}

static bool value_ok(const iot_json_doc_t *doc, int i);

static bool container_ok(const iot_json_doc_t *doc, int i)
{
    const iot_json_tok_t *t = &doc->toks[i];
    const bool is_obj = t->type == JSMN_OBJECT;
    const char close = is_obj ? '}' : ']';
    const char *s = doc->json;

    if (t->end <= t->start || s[t->end - 1] != close) {
        return false;
    }
    int pos = t->start + 1;
    int j = i + 1;
    for (int m = 0; m < t->size; m++) {
        if (j >= doc->ntoks || !gap_ok(s, pos, outer_start(&doc->toks[j]), m > 0 ? ',' : 0)) {
            return false;
        }
        int value = j;
        if (is_obj) {
            const iot_json_tok_t *k = &doc->toks[j];
            if (k->type != JSMN_STRING || !string_ok(s + k->start, (size_t)(k->end - k->start))) {
                return false;
            }
            value = j + 1;
            if (value >= doc->ntoks || !gap_ok(s, outer_end(k), outer_start(&doc->toks[value]), ':')) {
                return false;
            }
        }
        if (!value_ok(doc, value)) {
            return false;
        }
        pos = outer_end(&doc->toks[value]);
        j = skip(doc, value);
    }
    return gap_ok(s, pos, t->end - 1, 0);
}

static bool value_ok(const iot_json_doc_t *doc, int i)
{
    const iot_json_tok_t *t = &doc->toks[i];
    const char *s = doc->json + t->start;
    const size_t n = (size_t)(t->end - t->start);

    switch (t->type) {
    case JSMN_OBJECT:
    case JSMN_ARRAY:
        return container_ok(doc, i);
    case JSMN_STRING:
        return string_ok(s, n);
    case JSMN_PRIMITIVE:
        return primitive_ok(s, n);
    default:
        return false;
    }
}

/* Profundidade de aninhamento, sem recursão: pilha com o fim de cada
 * container aberto. */
static bool depth_ok(const iot_json_doc_t *doc)
{
    int ends[IOT_JSON_MAX_DEPTH];
    int top = 0;
    for (int i = 0; i < doc->ntoks; i++) {
        const iot_json_tok_t *t = &doc->toks[i];
        while (top > 0 && ends[top - 1] <= outer_start(t)) {
            top--;
        }
        if (t->type == JSMN_OBJECT || t->type == JSMN_ARRAY) {
            if (top >= (int)IOT_JSON_MAX_DEPTH) {
                return false;
            }
            ends[top++] = t->end;
        }
    }
    return true;
}

iot_err_t iot_json_parse(iot_json_doc_t *doc, const char *json, size_t len, iot_json_tok_t *toks,
                         size_t max_toks)
{
    if (doc == NULL || json == NULL || toks == NULL || max_toks == 0) {
        return IOT_ERR_ARG;
    }
    memset(doc, 0, sizeof(*doc));
    if (len > IOT_MAX_PAYLOAD) {
        return IOT_ERR_LIMIT;
    }
    if (max_toks > IOT_JSON_MAX_TOKENS) {
        max_toks = IOT_JSON_MAX_TOKENS;
    }

    jsmn_parser p;
    jsmn_init(&p);
    const int n = jsmn_parse(&p, json, len, toks, (unsigned int)max_toks);
    if (n == JSMN_ERROR_NOMEM) {
        return IOT_ERR_LIMIT;
    }
    if (n <= 0) {
        return IOT_ERR_PARSE;
    }

    doc->json = json;
    doc->len = len;
    doc->toks = toks;
    doc->ntoks = n;

    const iot_json_tok_t *root = &toks[0];
    if ((root->type != JSMN_OBJECT && root->type != JSMN_ARRAY) || skip(doc, 0) != n ||
        !gap_ok(json, 0, root->start, 0) || !gap_ok(json, root->end, (int)len, 0)) {
        memset(doc, 0, sizeof(*doc));
        return IOT_ERR_PARSE;
    }
    if (!depth_ok(doc)) {
        memset(doc, 0, sizeof(*doc));
        return IOT_ERR_LIMIT;
    }
    if (!value_ok(doc, 0)) {
        memset(doc, 0, sizeof(*doc));
        return IOT_ERR_PARSE;
    }
    return IOT_OK;
}

static bool valid_ref(const iot_json_doc_t *doc, iot_json_ref_t ref)
{
    return doc != NULL && ref >= 0 && ref < doc->ntoks;
}

iot_json_type_t iot_json_type(const iot_json_doc_t *doc, iot_json_ref_t ref)
{
    if (!valid_ref(doc, ref)) {
        return IOT_JSON_INVALID;
    }
    const iot_json_tok_t *t = &doc->toks[ref];
    switch (t->type) {
    case JSMN_OBJECT:
        return IOT_JSON_OBJECT;
    case JSMN_ARRAY:
        return IOT_JSON_ARRAY;
    case JSMN_STRING:
        return IOT_JSON_STRING;
    case JSMN_PRIMITIVE: {
        const char c = doc->json[t->start];
        if (c == 't' || c == 'f') {
            return IOT_JSON_BOOL;
        }
        return c == 'n' ? IOT_JSON_NULL : IOT_JSON_NUMBER;
    }
    default:
        return IOT_JSON_INVALID;
    }
}

int iot_json_size(const iot_json_doc_t *doc, iot_json_ref_t ref)
{
    const iot_json_type_t type = iot_json_type(doc, ref);
    if (type != IOT_JSON_OBJECT && type != IOT_JSON_ARRAY) {
        return -1;
    }
    return doc->toks[ref].size;
}

bool iot_json_next_member(const iot_json_doc_t *doc, iot_json_ref_t obj, iot_json_ref_t *key,
                          iot_json_ref_t *value)
{
    if (iot_json_type(doc, obj) != IOT_JSON_OBJECT || key == NULL || value == NULL) {
        return false;
    }
    const int next = (*key == IOT_JSON_NONE) ? obj + 1 : skip(doc, *key + 1);
    if (next >= doc->ntoks || outer_start(&doc->toks[next]) >= doc->toks[obj].end) {
        return false;
    }
    *key = next;
    *value = next + 1;
    return true;
}

static bool raw_eq(const iot_json_doc_t *doc, iot_json_ref_t ref, const char *s)
{
    const iot_json_tok_t *t = &doc->toks[ref];
    const size_t n = strlen(s);
    return (size_t)(t->end - t->start) == n && memcmp(doc->json + t->start, s, n) == 0;
}

iot_json_ref_t iot_json_get(const iot_json_doc_t *doc, iot_json_ref_t obj, const char *key)
{
    if (key == NULL) {
        return IOT_JSON_NONE;
    }
    iot_json_ref_t k = IOT_JSON_NONE;
    iot_json_ref_t v = IOT_JSON_NONE;
    while (iot_json_next_member(doc, obj, &k, &v)) {
        if (raw_eq(doc, k, key)) {
            return v;
        }
    }
    return IOT_JSON_NONE;
}

iot_json_ref_t iot_json_at(const iot_json_doc_t *doc, iot_json_ref_t arr, int index)
{
    if (iot_json_type(doc, arr) != IOT_JSON_ARRAY || index < 0 || index >= doc->toks[arr].size) {
        return IOT_JSON_NONE;
    }
    int j = arr + 1;
    for (int i = 0; i < index; i++) {
        j = skip(doc, j);
    }
    return j;
}

iot_err_t iot_json_raw(const iot_json_doc_t *doc, iot_json_ref_t ref, const char **text, size_t *len)
{
    if (!valid_ref(doc, ref) || text == NULL || len == NULL) {
        return IOT_ERR_ARG;
    }
    const iot_json_tok_t *t = &doc->toks[ref];
    *text = doc->json + t->start;
    *len = (size_t)(t->end - t->start);
    return IOT_OK;
}

bool iot_json_str_eq(const iot_json_doc_t *doc, iot_json_ref_t ref, const char *s)
{
    return iot_json_type(doc, ref) == IOT_JSON_STRING && s != NULL && raw_eq(doc, ref, s);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool read_u16(const char *s, size_t n, size_t i, uint32_t *out)
{
    if (i + 4 > n) {
        return false;
    }
    uint32_t v = 0;
    for (size_t k = 0; k < 4; k++) {
        const int h = hex_val(s[i + k]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

/* Grava `cp` em UTF-8; false se não couber (reservando o '\0'). */
static bool put_utf8(char *out, size_t cap, size_t *o, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*o + n >= cap) {
        return false;
    }
    memcpy(out + *o, b, n);
    *o += n;
    return true;
}

iot_err_t iot_json_get_str(const iot_json_doc_t *doc, iot_json_ref_t ref, char *out, size_t cap,
                           size_t *out_len)
{
    if (out == NULL || cap == 0) {
        return IOT_ERR_ARG;
    }
    out[0] = '\0';
    if (!valid_ref(doc, ref)) {
        return IOT_ERR_NOT_FOUND;
    }
    if (iot_json_type(doc, ref) != IOT_JSON_STRING) {
        return IOT_ERR_TYPE;
    }
    const iot_json_tok_t *t = &doc->toks[ref];
    const char *s = doc->json + t->start;
    const size_t n = (size_t)(t->end - t->start);
    size_t o = 0;

    for (size_t i = 0; i < n; i++) {
        uint32_t cp = (unsigned char)s[i];
        if (s[i] == '\\') {
            if (++i >= n) {
                return IOT_ERR_PARSE;
            }
            switch (s[i]) {
            case '"':
            case '\\':
            case '/':
                cp = (unsigned char)s[i];
                break;
            case 'b':
                cp = '\b';
                break;
            case 'f':
                cp = '\f';
                break;
            case 'n':
                cp = '\n';
                break;
            case 'r':
                cp = '\r';
                break;
            case 't':
                cp = '\t';
                break;
            case 'u': {
                if (!read_u16(s, n, i + 1, &cp)) {
                    return IOT_ERR_PARSE;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (i + 2 >= n || s[i + 1] != '\\' || s[i + 2] != 'u' || !read_u16(s, n, i + 3, &lo) ||
                        lo < 0xDC00 || lo > 0xDFFF) {
                        return IOT_ERR_PARSE;
                    }
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return IOT_ERR_PARSE;
                }
                break;
            }
            default:
                return IOT_ERR_PARSE;
            }
            if (!put_utf8(out, cap, &o, cp)) {
                out[0] = '\0';
                return IOT_ERR_NO_SPACE;
            }
            continue;
        }
        if (o + 1 >= cap) {
            out[0] = '\0';
            return IOT_ERR_NO_SPACE;
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    if (out_len != NULL) {
        *out_len = o;
    }
    return IOT_OK;
}

iot_err_t iot_json_get_i64(const iot_json_doc_t *doc, iot_json_ref_t ref, int64_t *out)
{
    if (out == NULL) {
        return IOT_ERR_ARG;
    }
    if (!valid_ref(doc, ref)) {
        return IOT_ERR_NOT_FOUND;
    }
    if (iot_json_type(doc, ref) != IOT_JSON_NUMBER) {
        return IOT_ERR_TYPE;
    }
    const iot_json_tok_t *t = &doc->toks[ref];
    const char *s = doc->json + t->start;
    const size_t n = (size_t)(t->end - t->start);
    size_t i = 0;
    const bool neg = s[0] == '-';
    if (neg) {
        i++;
    }
    /* Acumula negativo: cobre INT64_MIN sem overflow. */
    int64_t acc = 0;
    for (; i < n; i++) {
        if (!is_digit(s[i])) {
            return IOT_ERR_TYPE; /* fração ou expoente */
        }
        const int d = s[i] - '0';
        if (acc < (INT64_MIN + d) / 10) {
            return IOT_ERR_LIMIT;
        }
        acc = acc * 10 - d;
    }
    if (!neg) {
        if (acc == INT64_MIN) {
            return IOT_ERR_LIMIT;
        }
        acc = -acc;
    }
    *out = acc;
    return IOT_OK;
}

iot_err_t iot_json_get_bool(const iot_json_doc_t *doc, iot_json_ref_t ref, bool *out)
{
    if (out == NULL) {
        return IOT_ERR_ARG;
    }
    if (!valid_ref(doc, ref)) {
        return IOT_ERR_NOT_FOUND;
    }
    if (iot_json_type(doc, ref) != IOT_JSON_BOOL) {
        return IOT_ERR_TYPE;
    }
    *out = doc->json[doc->toks[ref].start] == 't';
    return IOT_OK;
}

/*
 * Escritor
 */

static void fail(iot_json_writer_t *w, iot_err_t err)
{
    if (!w->failed) {
        w->failed = true;
        w->err = err;
    }
}

/* Mantém sempre um byte livre para o '\0' do finish. */
static void put(iot_json_writer_t *w, const char *s, size_t n)
{
    if (w->failed) {
        return;
    }
    if (n >= w->cap - w->len) {
        fail(w, IOT_ERR_NO_SPACE);
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void put_c(iot_json_writer_t *w, char c)
{
    put(w, &c, 1);
}

/* Abre espaço para um valor: vírgula quando preciso e checagem de ordem. */
static void begin_value(iot_json_writer_t *w)
{
    if (w->failed) {
        return;
    }
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth == 0) {
        if (w->len > 0) {
            fail(w, IOT_ERR_STATE); /* uma raiz só */
        }
        return;
    }
    if (w->kind[w->depth - 1] == '{') {
        fail(w, IOT_ERR_STATE); /* valor de objeto sem chave */
        return;
    }
    if (w->has_items[w->depth - 1]) {
        put_c(w, ',');
    }
    w->has_items[w->depth - 1] = true;
}

void iot_jw_init(iot_json_writer_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (buf == NULL || cap == 0) {
        fail(w, IOT_ERR_ARG);
    }
}

static void open_level(iot_json_writer_t *w, char kind)
{
    begin_value(w);
    if (w->failed) {
        return;
    }
    if (w->depth >= IOT_JSON_MAX_DEPTH) {
        fail(w, IOT_ERR_LIMIT);
        return;
    }
    put_c(w, kind);
    w->kind[w->depth] = (uint8_t)kind;
    w->has_items[w->depth] = false;
    w->depth++;
}

static void close_level(iot_json_writer_t *w, char kind, char close)
{
    if (w->failed) {
        return;
    }
    if (w->depth == 0 || w->kind[w->depth - 1] != (uint8_t)kind || w->after_key) {
        fail(w, IOT_ERR_STATE);
        return;
    }
    put_c(w, close);
    w->depth--;
}

void iot_jw_obj_begin(iot_json_writer_t *w)
{
    open_level(w, '{');
}

void iot_jw_obj_end(iot_json_writer_t *w)
{
    close_level(w, '{', '}');
}

void iot_jw_arr_begin(iot_json_writer_t *w)
{
    open_level(w, '[');
}

void iot_jw_arr_end(iot_json_writer_t *w)
{
    close_level(w, '[', ']');
}

static void put_escaped(iot_json_writer_t *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    put_c(w, '"');
    for (size_t i = 0; i < n && !w->failed; i++) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            put(w, "\\\"", 2);
            break;
        case '\\':
            put(w, "\\\\", 2);
            break;
        case '\b':
            put(w, "\\b", 2);
            break;
        case '\f':
            put(w, "\\f", 2);
            break;
        case '\n':
            put(w, "\\n", 2);
            break;
        case '\r':
            put(w, "\\r", 2);
            break;
        case '\t':
            put(w, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                const char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xF]};
                put(w, u, sizeof(u));
            } else {
                put_c(w, (char)c);
            }
        }
    }
    put_c(w, '"');
}

void iot_jw_key(iot_json_writer_t *w, const char *key)
{
    if (w->failed) {
        return;
    }
    if (key == NULL) {
        fail(w, IOT_ERR_ARG);
        return;
    }
    if (w->depth == 0 || w->kind[w->depth - 1] != '{' || w->after_key) {
        fail(w, IOT_ERR_STATE);
        return;
    }
    if (w->has_items[w->depth - 1]) {
        put_c(w, ',');
    }
    w->has_items[w->depth - 1] = true;
    put_escaped(w, key, strlen(key));
    put_c(w, ':');
    w->after_key = true;
}

void iot_jw_str_n(iot_json_writer_t *w, const char *s, size_t n)
{
    if (s == NULL) {
        fail(w, IOT_ERR_ARG);
        return;
    }
    begin_value(w);
    put_escaped(w, s, n);
}

void iot_jw_str(iot_json_writer_t *w, const char *s)
{
    iot_jw_str_n(w, s, s != NULL ? strlen(s) : 0);
}

static void put_u64(iot_json_writer_t *w, uint64_t v)
{
    char tmp[20];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    char out[20];
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    put(w, out, n);
}

void iot_jw_u64(iot_json_writer_t *w, uint64_t v)
{
    begin_value(w);
    put_u64(w, v);
}

void iot_jw_i64(iot_json_writer_t *w, int64_t v)
{
    begin_value(w);
    if (v < 0) {
        put_c(w, '-');
        /* -(v+1)+1 evita overflow em INT64_MIN */
        put_u64(w, (uint64_t)(-(v + 1)) + 1u);
    } else {
        put_u64(w, (uint64_t)v);
    }
}

void iot_jw_bool(iot_json_writer_t *w, bool v)
{
    begin_value(w);
    if (v) {
        put(w, "true", 4);
    } else {
        put(w, "false", 5);
    }
}

void iot_jw_null(iot_json_writer_t *w)
{
    begin_value(w);
    put(w, "null", 4);
}

void iot_jw_raw(iot_json_writer_t *w, const char *json, size_t n)
{
    if (json == NULL || n == 0) {
        fail(w, IOT_ERR_ARG);
        return;
    }
    begin_value(w);
    put(w, json, n);
}

iot_err_t iot_jw_finish(iot_json_writer_t *w, size_t *out_len)
{
    if (!w->failed && (w->depth != 0 || w->after_key || w->len == 0)) {
        fail(w, IOT_ERR_STATE);
    }
    if (w->failed) {
        if (w->buf != NULL && w->cap > 0) {
            w->buf[0] = '\0';
        }
        return w->err;
    }
    w->buf[w->len] = '\0';
    if (out_len != NULL) {
        *out_len = w->len;
    }
    return IOT_OK;
}
