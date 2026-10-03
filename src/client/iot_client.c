#include <dextro-iot/iot.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "envelope/iot_envelope.h"
#include "session/iot_backoff.h"

/* O transporte do coreMQTT aponta para o da aplicação. Uma instância de
 * cliente por processo (a função de tempo do coreMQTT não recebe contexto). */
struct NetworkContext {
    iot_transport_t *t;
};

static struct NetworkContext s_net;
static iot_client_t *s_owner;

#define BACKOFF_MIN_MS 1000u
#define BACKOFF_MAX_MS 60000u
#define SUBACK_TIMEOUT_MS 10000u
#define DEFAULT_RPC_TIMEOUT_MS 30000u
#define DEVICE_TTL_MS 60000
#define CLOCK_SANE_MS 1600000000000LL /* 2020: abaixo disso não é hora de parede */

enum { IN_NONE = 0, IN_PROCEDURE = 1, IN_SERVICE_REPLY = 2 };

/* --------------------------------------------------------------- suporte */

static void logf_(iot_client_t *c, iot_log_level_t lvl, const char *fmt, ...)
{
    if (c->cfg.log.write == NULL) {
        return;
    }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    c->cfg.log.write(c->cfg.log.ctx, lvl, msg);
}

static uint64_t mono(const iot_client_t *c)
{
    return c->cfg.clock.mono_ms(c->cfg.clock.ctx);
}

static uint32_t mqtt_time(void)
{
    return s_owner != NULL ? (uint32_t)mono(s_owner) : 0u;
}

static uint32_t rnd32(iot_client_t *c)
{
    uint32_t r = 0;
    c->cfg.rng.fill(c->cfg.rng.ctx, &r, sizeof(r));
    return r;
}

static void new_uuid(iot_client_t *c, char out[IOT_UUID_LEN + 1])
{
    uint8_t b[16];
    c->cfg.rng.fill(c->cfg.rng.ctx, b, sizeof(b));
    iot_uuid_v4(b, out);
}

static void emit(iot_client_t *c, iot_event_t ev)
{
    if (c->cfg.on_event != NULL) {
        c->cfg.on_event(c, ev, c->cfg.user);
    }
}

int64_t iot_now_epoch_ms(const iot_client_t *c)
{
    if (c->cfg.clock.epoch_ms != NULL) {
        const int64_t wall = c->cfg.clock.epoch_ms(c->cfg.clock.ctx);
        if (wall > CLOCK_SANE_MS) {
            return wall;
        }
    }
    return c->clock_known ? (int64_t)mono(c) + c->clock_offset_ms : 0;
}

/* Sem hora de parede, a do servidor vale (qualquer mensagem dele tem
 * timestamp); o keepalive refina com serverTime. */
static void learn_clock(iot_client_t *c, int64_t server_ms)
{
    if (server_ms > CLOCK_SANE_MS) {
        c->clock_offset_ms = server_ms - (int64_t)mono(c);
        c->clock_known = true;
    }
}

const char *iot_state_str(iot_state_t st)
{
    switch (st) {
    case IOT_STATE_STOPPED:
        return "stopped";
    case IOT_STATE_BACKOFF:
        return "backoff";
    case IOT_STATE_CONNECTING:
        return "connecting";
    case IOT_STATE_SUBSCRIBING:
        return "subscribing";
    case IOT_STATE_ONLINE:
        return "online";
    }
    return "?";
}

iot_state_t iot_state(const iot_client_t *c)
{
    return c->state;
}

int iot_reply_error(iot_json_writer_t *out, int rescode, const char *code, const char *detail)
{
    iot_jw_obj_begin(out);
    iot_jw_key(out, "error");
    iot_jw_str(out, code);
    if (detail != NULL && detail[0] != '\0') {
        iot_jw_key(out, "detail");
        iot_jw_str(out, detail);
    }
    iot_jw_obj_end(out);
    return rescode;
}

/* --------------------------------------------------------- transporte MQTT */

static int32_t net_recv(struct NetworkContext *n, void *buf, size_t len)
{
    return n->t->recv(n->t->ctx, buf, len);
}

static int32_t net_send(struct NetworkContext *n, const void *buf, size_t len)
{
    return n->t->send(n->t->ctx, buf, len);
}

static bool publish(iot_client_t *c, const char *topic, const char *payload, size_t len, bool retain)
{
    MQTTPublishInfo_t p = {
        .qos = MQTTQoS1,
        .retain = retain,
        .pTopicName = topic,
        .topicNameLength = (uint16_t)strlen(topic),
        .pPayload = payload,
        .payloadLength = len,
    };
    return MQTT_Publish(&c->mqtt, &p, MQTT_GetPacketId(&c->mqtt)) == MQTTSuccess;
}

static size_t presence_json(iot_client_t *c, bool online, char *buf, size_t cap)
{
    const int64_t ts = iot_now_epoch_ms(c);
    int n;
    if (ts > 0) {
        n = snprintf(buf, cap, "{\"online\":%s,\"ts\":%lld}", online ? "true" : "false", (long long)ts);
    } else {
        n = snprintf(buf, cap, "{\"online\":%s}", online ? "true" : "false");
    }
    return n > 0 ? (size_t)n : 0;
}

/* --------------------------------------------------- entrada (callback MQTT) */

static bool topic_is(const MQTTPublishInfo_t *p, const char *topic)
{
    const size_t n = strlen(topic);
    return p->topicNameLength == n && memcmp(p->pTopicName, topic, n) == 0;
}

/* O pacote mora no buffer de rede, que o próximo publish reaproveita: copia
 * para in_buf e processa depois do MQTT_ProcessLoop. Uma mensagem por vez
 * (o loop recebe um pacote por chamada). */
static void on_mqtt(MQTTContext_t *ctx, MQTTPacketInfo_t *pkt, MQTTDeserializedInfo_t *info)
{
    (void)ctx;
    iot_client_t *c = s_owner;
    if (c == NULL) {
        return;
    }
    const uint8_t type = (uint8_t)(pkt->type & 0xF0U);
    if (type == MQTT_PACKET_TYPE_SUBACK) {
        if (c->state == IOT_STATE_SUBSCRIBING && info->packetIdentifier == c->sub_packet_id) {
            uint8_t *codes = NULL;
            size_t n = 0;
            bool ok = MQTT_GetSubAckStatusCodes(pkt, &codes, &n) == MQTTSuccess;
            for (size_t i = 0; ok && i < n; i++) {
                ok = codes[i] != 0x80U;
            }
            c->sub_packet_id = ok ? 0 : UINT16_MAX; /* UINT16_MAX = recusado */
        }
        return;
    }
    if (type != MQTT_PACKET_TYPE_PUBLISH || info->pPublishInfo == NULL) {
        return;
    }
    const MQTTPublishInfo_t *p = info->pPublishInfo;
    int kind = IN_NONE;
    if (topic_is(p, c->topic_procedure)) {
        kind = IN_PROCEDURE;
    } else if (topic_is(p, c->topic_service_reply)) {
        kind = IN_SERVICE_REPLY;
    }
    if (kind == IN_NONE) {
        return; /* inclusive o próprio status retido */
    }
    if (c->in_pending || p->payloadLength >= c->cfg.in_len) {
        logf_(c, IOT_LOG_WARN, "mensagem descartada (%s, %u bytes)", c->in_pending ? "fila cheia" : "grande demais",
              (unsigned)p->payloadLength);
        return;
    }
    memcpy(c->cfg.in_buf, p->pPayload, p->payloadLength);
    c->cfg.in_buf[p->payloadLength] = '\0';
    c->in_size = p->payloadLength;
    c->in_kind = kind;
    c->in_pending = true;
}

/* ------------------------------------------------------- device-procedures */

static const iot_procedure_t *find_app_op(iot_client_t *c, const char *op)
{
    for (size_t i = 0; i < c->cfg.procedure_count; i++) {
        if (strcmp(c->cfg.procedures[i].op, op) == 0) {
            return &c->cfg.procedures[i];
        }
    }
    return NULL;
}

static const char *const k_core_ops[] = {"list-procedures", "get-status", "outbox-notification",
                                         "ota-notification", "settings-read", "settings-write"};
#define CORE_OPS (sizeof(k_core_ops) / sizeof(k_core_ops[0]))

static int core_list_procedures(iot_client_t *c, iot_json_writer_t *out)
{
    iot_jw_obj_begin(out);
    iot_jw_key(out, "procedures");
    iot_jw_arr_begin(out);
    for (size_t i = 0; i < CORE_OPS; i++) {
        if (find_app_op(c, k_core_ops[i]) != NULL) {
            continue; /* listado abaixo, com a versão da aplicação */
        }
        iot_jw_obj_begin(out);
        iot_jw_key(out, "op");
        iot_jw_str(out, k_core_ops[i]);
        iot_jw_key(out, "version");
        iot_jw_u64(out, 1);
        iot_jw_obj_end(out);
    }
    for (size_t i = 0; i < c->cfg.procedure_count; i++) {
        iot_jw_obj_begin(out);
        iot_jw_key(out, "op");
        iot_jw_str(out, c->cfg.procedures[i].op);
        iot_jw_key(out, "version");
        iot_jw_u64(out, c->cfg.procedures[i].version > 0 ? c->cfg.procedures[i].version : 1);
        iot_jw_obj_end(out);
    }
    iot_jw_arr_end(out);
    if (c->cfg.device_type != NULL) {
        iot_jw_key(out, "deviceType");
        iot_jw_str(out, c->cfg.device_type);
    }
    if (c->cfg.contract_version > 0) {
        iot_jw_key(out, "contractVersion");
        iot_jw_u64(out, c->cfg.contract_version);
    }
    iot_jw_obj_end(out);
    return IOT_RES_OK;
}

static int core_dispatch(iot_client_t *c, const iot_request_t *req, iot_json_writer_t *out, bool *found)
{
    *found = true;
    if (strcmp(req->op, "list-procedures") == 0) {
        return core_list_procedures(c, out);
    }
    if (strcmp(req->op, "get-status") == 0) {
        iot_jw_obj_begin(out);
        if (c->cfg.status_fill != NULL) {
            c->cfg.status_fill(c, out, c->cfg.user);
        }
        iot_jw_obj_end(out);
        return IOT_RES_OK;
    }
    if (strcmp(req->op, "outbox-notification") == 0) {
        /* best-effort: só avisa a aplicação para drenar (contracts A.4.4) */
        emit(c, IOT_EVENT_OUTBOX_NOTIFIED);
        iot_jw_obj_begin(out);
        iot_jw_obj_end(out);
        return IOT_RES_NO_CONTENT;
    }
    if (strcmp(req->op, "ota-notification") == 0 || strcmp(req->op, "settings-write") == 0) {
        return iot_reply_error(out, IOT_RES_BAD_REQUEST, "NOT_SUPPORTED", "op sem implementação neste firmware");
    }
    if (strcmp(req->op, "settings-read") == 0) {
        iot_jw_obj_begin(out);
        iot_jw_key(out, "version");
        iot_jw_u64(out, 0);
        iot_jw_key(out, "settings");
        iot_jw_arr_begin(out);
        iot_jw_arr_end(out);
        iot_jw_obj_end(out);
        return IOT_RES_OK;
    }
    *found = false;
    return IOT_RES_NOT_FOUND;
}

static iot_idem_t *idem_find(iot_client_t *c, const char *key)
{
    for (size_t i = 0; i < IOT_IDEM_SLOTS; i++) {
        if (c->idem[i].used && strcmp(c->idem[i].key, key) == 0) {
            return &c->idem[i];
        }
    }
    return NULL;
}

static void idem_store(iot_client_t *c, const char *key, int rescode, const char *payload, size_t len)
{
    if (len >= IOT_IDEM_RESP_MAX) {
        return; /* resposta grande demais para guardar: a repetição reexecuta */
    }
    iot_idem_t *slot = &c->idem[0];
    for (size_t i = 0; i < IOT_IDEM_SLOTS; i++) {
        if (!c->idem[i].used) {
            slot = &c->idem[i];
            break;
        }
        if (c->idem[i].at_ms < slot->at_ms) {
            slot = &c->idem[i];
        }
    }
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    memcpy(slot->response, payload, len);
    slot->response[len] = '\0';
    slot->rescode = rescode;
    slot->at_ms = mono(c);
    slot->used = true;
}

/* Monta e publica a resposta; o payload vem do handler (ou de um erro). */
static void handle_procedure(iot_client_t *c, const iot_json_doc_t *doc)
{
    iot_envelope_t env;
    const iot_env_status_t st = iot_envelope_read(doc, false, &env);
    if (st == IOT_ENV_NO_RELATION) {
        logf_(c, IOT_LOG_WARN, "procedure sem relationId/instance: descartada");
        return;
    }
    if (env.timestamp_ms > 0) {
        learn_clock(c, env.timestamp_ms);
    }

    char topic[IOT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "atrio/%s/procedure/reply/%s", c->cfg.serial, env.instance);
    char msg_id[IOT_UUID_LEN + 1];
    new_uuid(c, msg_id);

    iot_json_writer_t w;
    iot_jw_init(&w, c->cfg.out_buf, c->cfg.out_len);
    iot_jw_obj_begin(&w);
    iot_envelope_write_metadata(&w, msg_id, env.relation_id, iot_now_epoch_ms(c), c->cfg.serial, env.instance,
                                env.traceparent);
    iot_jw_key(&w, "op");
    iot_jw_str(&w, env.op[0] != '\0' ? env.op : "?");
    iot_jw_key(&w, "payload");
    const size_t payload_start = w.len;

    int rescode;
    const iot_procedure_t *app = NULL;
    char idem_key[65] = "";
    if (st == IOT_ENV_BAD_VERSION) {
        rescode = iot_reply_error(&w, IOT_RES_BAD_REQUEST, "UNSUPPORTED_VERSION", env.version);
    } else if (st != IOT_ENV_OK || iot_json_type(doc, env.payload) != IOT_JSON_OBJECT) {
        rescode = iot_reply_error(&w, IOT_RES_BAD_REQUEST, "INVALID_ENVELOPE", NULL);
    } else if (strcmp(env.serial, c->cfg.serial) != 0) {
        rescode = iot_reply_error(&w, IOT_RES_FORBIDDEN, "SN_MISMATCH", NULL);
    } else if (iot_now_epoch_ms(c) > 0 && iot_now_epoch_ms(c) - env.timestamp_ms > DEVICE_TTL_MS) {
        /* request velho (backlog pós-reconexão) não é executado — nem descartado em silêncio */
        rescode = iot_reply_error(&w, IOT_RES_TIMEOUT, "TTL_EXPIRED", NULL);
    } else {
        const iot_request_t req = {
            .op = env.op,
            .doc = doc,
            .payload = env.payload,
            .relation_id = env.relation_id,
            .timestamp_ms = env.timestamp_ms,
        };
        app = find_app_op(c, env.op);
        const iot_idem_t *cached = NULL;
        if (app != NULL && (app->flags & IOT_OP_IDEMPOTENCY_KEY) != 0 &&
            iot_json_get_str(doc, iot_json_get(doc, env.payload, "idempotencyKey"), idem_key, sizeof(idem_key),
                             NULL) == IOT_OK &&
            idem_key[0] != '\0') {
            cached = idem_find(c, idem_key);
        }
        if (cached != NULL) {
            iot_jw_raw(&w, cached->response, strlen(cached->response));
            rescode = cached->rescode;
        } else if (app != NULL) {
            rescode = app->handler(c, &req, &w, app->user);
        } else {
            bool found = false;
            rescode = core_dispatch(c, &req, &w, &found);
            if (!found) {
                rescode = iot_reply_error(&w, IOT_RES_NOT_FOUND, "UNKNOWN_OP", env.op);
            }
        }
    }
    const size_t payload_end = w.len;
    iot_jw_key(&w, "rescode");
    iot_jw_i64(&w, rescode);
    iot_jw_obj_end(&w);
    size_t len = 0;
    if (iot_jw_finish(&w, &len) != IOT_OK) {
        /* handler escreveu errado ou não coube: responde 500 mínimo */
        iot_jw_init(&w, c->cfg.out_buf, c->cfg.out_len);
        iot_jw_obj_begin(&w);
        iot_envelope_write_metadata(&w, msg_id, env.relation_id, iot_now_epoch_ms(c), c->cfg.serial,
                                    env.instance, env.traceparent);
        iot_jw_key(&w, "op");
        iot_jw_str(&w, env.op);
        iot_jw_key(&w, "payload");
        iot_reply_error(&w, IOT_RES_SERVER_ERROR, "INTERNAL", "resposta inválida ou grande demais");
        iot_jw_key(&w, "rescode");
        iot_jw_i64(&w, IOT_RES_SERVER_ERROR);
        iot_jw_obj_end(&w);
        rescode = IOT_RES_SERVER_ERROR;
        if (iot_jw_finish(&w, &len) != IOT_OK) {
            return;
        }
    } else if (idem_key[0] != '\0' && app != NULL && idem_find(c, idem_key) == NULL &&
               iot_rescode_is_success(rescode)) {
        idem_store(c, idem_key, rescode, c->cfg.out_buf + payload_start, payload_end - payload_start);
    }
    logf_(c, IOT_LOG_INFO, "procedure %s -> %d", env.op, rescode);
    if (!publish(c, topic, c->cfg.out_buf, len, false)) {
        logf_(c, IOT_LOG_WARN, "resposta de %s não publicada", env.op);
    }
}

/* ----------------------------------------------------- remote-procedures */

static void handle_service_reply(iot_client_t *c, const iot_json_doc_t *doc)
{
    iot_envelope_t env;
    if (iot_envelope_read(doc, true, &env) != IOT_ENV_OK) {
        logf_(c, IOT_LOG_WARN, "resposta de service inválida");
        if (env.relation_id[0] == '\0') {
            return;
        }
    }
    learn_clock(c, env.timestamp_ms);
    for (size_t i = 0; i < IOT_MAX_PENDING; i++) {
        iot_pending_t *p = &c->pending[i];
        if (p->used && strcmp(p->relation_id, env.relation_id) == 0) {
            p->used = false;
            if (p->cb != NULL) {
                p->cb(c, env.rescode > 0 ? env.rescode : IOT_RES_BAD_GATEWAY, doc, env.payload, p->user);
            }
            return;
        }
    }
    /* resposta atrasada (já expirou localmente): ignora */
}

iot_err_t iot_call_service(iot_client_t *c, const char *op, iot_fill_cb fill, void *fill_user, iot_reply_cb cb,
                           void *user, uint32_t timeout_ms)
{
    if (c == NULL || op == NULL) {
        return IOT_ERR_ARG;
    }
    if (c->state != IOT_STATE_ONLINE) {
        return IOT_ERR_STATE;
    }
    iot_pending_t *slot = NULL;
    for (size_t i = 0; i < IOT_MAX_PENDING && slot == NULL; i++) {
        if (!c->pending[i].used) {
            slot = &c->pending[i];
        }
    }
    if (slot == NULL) {
        return IOT_ERR_NO_SPACE;
    }
    char msg_id[IOT_UUID_LEN + 1];
    new_uuid(c, msg_id);
    new_uuid(c, slot->relation_id);

    iot_json_writer_t w;
    iot_jw_init(&w, c->cfg.out_buf, c->cfg.out_len);
    iot_jw_obj_begin(&w);
    /* instance = o próprio SN nos requests do device (contracts A.1) */
    iot_envelope_write_metadata(&w, msg_id, slot->relation_id, iot_now_epoch_ms(c), c->cfg.serial, c->cfg.serial,
                                NULL);
    iot_jw_key(&w, "op");
    iot_jw_str(&w, op);
    iot_jw_key(&w, "payload");
    iot_jw_obj_begin(&w);
    if (fill != NULL) {
        fill(c, &w, fill_user);
    }
    iot_jw_obj_end(&w);
    iot_jw_obj_end(&w);
    size_t len = 0;
    if (iot_jw_finish(&w, &len) != IOT_OK) {
        return IOT_ERR_LIMIT;
    }
    if (!publish(c, c->topic_service, c->cfg.out_buf, len, false)) {
        return IOT_ERR_TRANSPORT;
    }
    slot->cb = cb;
    slot->user = user;
    slot->deadline_ms = mono(c) + (timeout_ms > 0 ? timeout_ms : DEFAULT_RPC_TIMEOUT_MS);
    slot->used = true;
    return IOT_OK;
}

static void expire_pending(iot_client_t *c, bool all, int rescode)
{
    const uint64_t now = mono(c);
    for (size_t i = 0; i < IOT_MAX_PENDING; i++) {
        iot_pending_t *p = &c->pending[i];
        if (p->used && (all || now >= p->deadline_ms)) {
            p->used = false;
            if (p->cb != NULL) {
                p->cb(c, rescode, NULL, IOT_JSON_NONE, p->user);
            }
        }
    }
}

/* -------------------------------------------------------------- keepalive */

static void keepalive_payload(iot_client_t *c, iot_json_writer_t *w, void *user)
{
    (void)user;
    iot_jw_key(w, "intervalMs");
    iot_jw_u64(w, c->cfg.app_keepalive_ms);
    if (c->cfg.keepalive_fill != NULL) {
        c->cfg.keepalive_fill(c, w, c->cfg.user);
    }
}

static void keepalive_reply(iot_client_t *c, int rescode, const iot_json_doc_t *doc, iot_json_ref_t payload,
                            void *user)
{
    (void)user;
    if (doc == NULL) {
        logf_(c, IOT_LOG_WARN, "keepalive sem resposta (%d)", rescode);
        return;
    }
    if (rescode == IOT_RES_TIMEOUT && !c->clock_known) {
        /* era a hora errada; o 504 trouxe a do servidor: tenta já */
        c->next_keepalive_ms = mono(c) + 500;
        return;
    }
    if (!iot_rescode_is_success(rescode)) {
        logf_(c, IOT_LOG_WARN, "keepalive recusado (%d)", rescode);
        return;
    }
    int64_t server_time = 0;
    if (iot_json_get_i64(doc, iot_json_get(doc, payload, "serverTime"), &server_time) == IOT_OK) {
        learn_clock(c, server_time);
    }
    bool resync = false;
    if (iot_json_get_bool(doc, iot_json_get(doc, payload, "settingsResync"), &resync) == IOT_OK && resync) {
        emit(c, IOT_EVENT_SETTINGS_RESYNC);
    }
    emit(c, IOT_EVENT_KEEPALIVE_OK);
}

void iot_keepalive_now(iot_client_t *c)
{
    c->next_keepalive_ms = 0;
}

/* --------------------------------------------------------------- sessão */

static void drop(iot_client_t *c, const char *why)
{
    const bool was_online = c->state == IOT_STATE_ONLINE;
    c->cfg.transport.close(c->cfg.transport.ctx);
    c->in_pending = false;
    expire_pending(c, true, IOT_RES_UNAVAILABLE);
    const uint32_t wait = iot_backoff_next(&c->backoff_ms, BACKOFF_MIN_MS, BACKOFF_MAX_MS, rnd32(c));
    c->next_attempt_ms = mono(c) + wait;
    c->state = IOT_STATE_BACKOFF;
    logf_(c, IOT_LOG_WARN, "sessão caiu (%s); nova tentativa em %u ms", why, (unsigned)wait);
    if (was_online) {
        emit(c, IOT_EVENT_OFFLINE);
    }
}

static void try_connect(iot_client_t *c)
{
    c->state = IOT_STATE_CONNECTING;
    const uint32_t timeout = c->cfg.connect_timeout_ms > 0 ? c->cfg.connect_timeout_ms : 10000u;
    if (c->cfg.transport.connect(c->cfg.transport.ctx, c->cfg.host, c->cfg.port, timeout) != 0) {
        drop(c, "transporte");
        return;
    }

    /* Contexto do coreMQTT novo a cada conexão: clean session (spec §8). */
    const TransportInterface_t transport = {
        .recv = net_recv,
        .send = net_send,
        .writev = NULL,
        .pNetworkContext = &s_net,
    };
    const MQTTFixedBuffer_t buf = {.pBuffer = c->cfg.net_buf, .size = c->cfg.net_len};
    if (MQTT_Init(&c->mqtt, &transport, mqtt_time, on_mqtt, &buf) != MQTTSuccess ||
        MQTT_InitStatefulQoS(&c->mqtt, c->out_records, sizeof(c->out_records) / sizeof(c->out_records[0]),
                             c->in_records, sizeof(c->in_records) / sizeof(c->in_records[0])) != MQTTSuccess) {
        drop(c, "init mqtt");
        return;
    }

    char will[48];
    const size_t will_len = presence_json(c, false, will, sizeof(will));
    const MQTTPublishInfo_t will_info = {
        .qos = MQTTQoS1,
        .retain = true,
        .pTopicName = c->topic_status,
        .topicNameLength = (uint16_t)strlen(c->topic_status),
        .pPayload = will,
        .payloadLength = will_len,
    };
    const MQTTConnectInfo_t info = {
        .cleanSession = true,
        .keepAliveSeconds = c->cfg.keepalive_s,
        .pClientIdentifier = c->cfg.serial, /* clientId = SN; usuário vem do CN do certificado */
        .clientIdentifierLength = (uint16_t)strlen(c->cfg.serial),
    };
    bool session_present = false;
    const MQTTStatus_t st = MQTT_Connect(&c->mqtt, &info, &will_info, timeout, &session_present);
    if (st != MQTTSuccess) {
        logf_(c, IOT_LOG_WARN, "CONNECT recusado: %s", MQTT_Status_strerror(st));
        drop(c, "connect");
        return;
    }

    /* Assina exatamente os dois tópicos que o ACL permite (curinga derruba). */
    MQTTSubscribeInfo_t subs[2] = {
        {.qos = MQTTQoS1, .pTopicFilter = c->topic_procedure, .topicFilterLength = (uint16_t)strlen(c->topic_procedure)},
        {.qos = MQTTQoS1,
         .pTopicFilter = c->topic_service_reply,
         .topicFilterLength = (uint16_t)strlen(c->topic_service_reply)},
    };
    c->sub_packet_id = MQTT_GetPacketId(&c->mqtt);
    if (MQTT_Subscribe(&c->mqtt, subs, 2, c->sub_packet_id) != MQTTSuccess) {
        drop(c, "subscribe");
        return;
    }
    c->state = IOT_STATE_SUBSCRIBING;
    c->phase_started_ms = mono(c);
}

static void become_online(iot_client_t *c)
{
    char online[48];
    const size_t len = presence_json(c, true, online, sizeof(online));
    if (!publish(c, c->topic_status, online, len, true)) {
        drop(c, "presença");
        return;
    }
    c->state = IOT_STATE_ONLINE;
    c->backoff_ms = BACKOFF_MIN_MS;
    c->connects++;
    /* primeiro keepalive logo, com um pequeno sorteio (evita a manada) */
    c->next_keepalive_ms = mono(c) + (rnd32(c) % 2000u);
    logf_(c, IOT_LOG_INFO, "online em %s:%u como %s", c->cfg.host, c->cfg.port, c->cfg.serial);
    emit(c, IOT_EVENT_ONLINE);
}

static void process_input(iot_client_t *c)
{
    if (!c->in_pending) {
        return;
    }
    iot_json_doc_t doc;
    const iot_err_t err = iot_json_parse(&doc, c->cfg.in_buf, c->in_size, c->cfg.toks, c->cfg.ntoks);
    const int kind = c->in_kind;
    if (err != IOT_OK) {
        logf_(c, IOT_LOG_WARN, "mensagem inválida descartada (%s)", iot_err_str(err));
    } else if (kind == IN_PROCEDURE) {
        handle_procedure(c, &doc);
    } else {
        handle_service_reply(c, &doc);
    }
    c->in_pending = false;
}

iot_err_t iot_init(iot_client_t *c, const iot_config_t *cfg)
{
    if (c == NULL || cfg == NULL || cfg->serial == NULL || strlen(cfg->serial) != IOT_SN_LEN || cfg->host == NULL ||
        cfg->transport.connect == NULL || cfg->transport.send == NULL || cfg->transport.recv == NULL ||
        cfg->transport.close == NULL || cfg->clock.mono_ms == NULL || cfg->rng.fill == NULL ||
        cfg->net_buf == NULL || cfg->out_buf == NULL || cfg->in_buf == NULL || cfg->toks == NULL) {
        return IOT_ERR_ARG;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    if (c->cfg.keepalive_s == 0) {
        c->cfg.keepalive_s = 15;
    }
    if (c->cfg.app_keepalive_ms == 0) {
        c->cfg.app_keepalive_ms = 60000;
    }
    snprintf(c->topic_procedure, IOT_TOPIC_MAX, "atrio/%s/procedure", cfg->serial);
    snprintf(c->topic_service, IOT_TOPIC_MAX, "atrio/%s/service", cfg->serial);
    snprintf(c->topic_service_reply, IOT_TOPIC_MAX, "atrio/%s/service/reply", cfg->serial);
    snprintf(c->topic_status, IOT_TOPIC_MAX, "atrio/%s/status", cfg->serial);
    s_net.t = &c->cfg.transport;
    s_owner = c;
    c->net_ctx = &s_net;
    c->backoff_ms = BACKOFF_MIN_MS;
    c->state = IOT_STATE_STOPPED;
    return IOT_OK;
}

iot_err_t iot_start(iot_client_t *c)
{
    if (c == NULL) {
        return IOT_ERR_ARG;
    }
    c->state = IOT_STATE_BACKOFF;
    c->next_attempt_ms = 0;
    return IOT_OK;
}

void iot_step(iot_client_t *c)
{
    const uint64_t now = mono(c);
    switch (c->state) {
    case IOT_STATE_STOPPED:
    case IOT_STATE_CONNECTING:
        return;
    case IOT_STATE_BACKOFF:
        if (now >= c->next_attempt_ms) {
            try_connect(c);
        }
        return;
    case IOT_STATE_SUBSCRIBING:
    case IOT_STATE_ONLINE:
        break;
    }

    /* recebe (um pacote por vez), PINGREQ/PINGRESP, retransmissões */
    const MQTTStatus_t st = MQTT_ProcessLoop(&c->mqtt);
    if (st != MQTTSuccess && st != MQTTNeedMoreBytes) {
        drop(c, MQTT_Status_strerror(st));
        return;
    }

    if (c->state == IOT_STATE_SUBSCRIBING) {
        if (c->sub_packet_id == UINT16_MAX) {
            drop(c, "assinatura recusada (ACL)");
        } else if (c->sub_packet_id == 0) {
            become_online(c);
        } else if (now - c->phase_started_ms > SUBACK_TIMEOUT_MS) {
            drop(c, "sem SUBACK");
        }
        return;
    }

    process_input(c);
    if (c->state != IOT_STATE_ONLINE) {
        return; /* a resposta derrubou a sessão */
    }
    expire_pending(c, false, IOT_RES_TIMEOUT);
    if (now >= c->next_keepalive_ms) {
        c->next_keepalive_ms = now + c->cfg.app_keepalive_ms;
        const iot_err_t err = iot_call_service(c, "keepalive", keepalive_payload, NULL, keepalive_reply, NULL, 0);
        if (err == IOT_ERR_TRANSPORT) {
            drop(c, "keepalive");
        }
    }
}

void iot_stop(iot_client_t *c)
{
    if (c->state == IOT_STATE_ONLINE || c->state == IOT_STATE_SUBSCRIBING) {
        /* o DISCONNECT cancela o LWT: publica offline antes (spec §8) */
        char off[48];
        const size_t len = presence_json(c, false, off, sizeof(off));
        publish(c, c->topic_status, off, len, true);
        MQTT_Disconnect(&c->mqtt);
        c->cfg.transport.close(c->cfg.transport.ctx);
        expire_pending(c, true, IOT_RES_UNAVAILABLE);
        emit(c, IOT_EVENT_OFFLINE);
    }
    c->state = IOT_STATE_STOPPED;
}
