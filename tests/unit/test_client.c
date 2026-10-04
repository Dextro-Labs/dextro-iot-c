/*
 * Sessão e RPC de ponta a ponta contra um broker falso: o transporte é um
 * roteiro (o teste injeta CONNACK, SUBACK, PUBLISH...) e captura os pacotes
 * que a lib envia; o relógio é controlado pelo teste.
 */
#include <dextro-iot/iot.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

#define SN "6809479F5758"

/* ------------------------------------------------------- transporte falso */

static uint8_t rx[16384];
static size_t rx_len, rx_pos;
static uint8_t tx[32768];
static size_t tx_len, tx_read;
static bool link_down, connect_fails;
static int connects;

static int t_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms)
{
    (void)ctx;
    (void)host;
    (void)port;
    (void)timeout_ms;
    connects++;
    if (connect_fails) {
        return -1;
    }
    link_down = false;
    /* o broker responde CONNACK aceito assim que recebe o CONNECT */
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    memcpy(rx + rx_len, connack, sizeof(connack));
    rx_len += sizeof(connack);
    return 0;
}

static int32_t t_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (link_down) {
        return -1;
    }
    TEST_ASSERT_TRUE(tx_len + len <= sizeof(tx));
    memcpy(tx + tx_len, buf, len);
    tx_len += len;
    return (int32_t)len;
}

static int32_t t_recv(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    if (link_down) {
        return -1;
    }
    const size_t avail = rx_len - rx_pos;
    const size_t n = avail < len ? avail : len;
    memcpy(buf, rx + rx_pos, n);
    rx_pos += n;
    return (int32_t)n;
}

static void t_close(void *ctx)
{
    (void)ctx;
    rx_len = rx_pos = 0;
}

/* ----------------------------------------------------- relógio e RNG falsos */

static uint64_t now_ms;
static int64_t wall_ms; /* 0 = sem hora de parede */

static uint64_t c_mono(void *ctx)
{
    (void)ctx;
    return now_ms;
}

static int64_t c_epoch(void *ctx)
{
    (void)ctx;
    return wall_ms == 0 ? 0 : wall_ms + (int64_t)now_ms;
}

static uint32_t rng_state = 12345;

static void r_fill(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++) {
        rng_state = rng_state * 1103515245u + 12345u;
        b[i] = (uint8_t)(rng_state >> 16);
    }
}

/* -------------------------------------------------------------- cliente */

static uint8_t net_buf[IOT_MAX_PAYLOAD + 512];
static char out_buf[IOT_MAX_PAYLOAD];
static char in_buf[IOT_MAX_PAYLOAD];
static iot_json_tok_t toks[IOT_JSON_MAX_TOKENS];
static iot_client_t client;
static int events[8];
static int handler_calls;

static void on_event(iot_client_t *c, iot_event_t ev, void *user)
{
    (void)c;
    (void)user;
    events[ev]++;
}

static void status_fill(iot_client_t *c, iot_json_writer_t *w, void *user)
{
    (void)c;
    (void)user;
    iot_jw_key(w, "uptimeS");
    iot_jw_u64(w, 42);
}

static int relay_set(iot_client_t *c, const iot_request_t *req, iot_json_writer_t *out, void *user)
{
    (void)c;
    (void)req;
    (void)user;
    handler_calls++;
    iot_jw_obj_begin(out);
    iot_jw_key(out, "state");
    iot_jw_str(out, "on");
    iot_jw_obj_end(out);
    return IOT_RES_OK;
}

static int outbox_calls;
static char outbox_last_cmd[32];

static bool on_outbox(iot_client_t *c, const iot_json_doc_t *doc, iot_json_ref_t item, const char *command,
                      char *reason, size_t cap, void *user)
{
    (void)c;
    (void)doc;
    (void)item;
    (void)user;
    outbox_calls++;
    snprintf(outbox_last_cmd, sizeof(outbox_last_cmd), "%s", command);
    if (strcmp(command, "settings-write") == 0) {
        return true;
    }
    snprintf(reason, cap, "UNKNOWN_COMMAND");
    return false;
}

static const iot_procedure_t app_ops[] = {
    {.op = "relay-set", .version = 2, .handler = relay_set, .flags = IOT_OP_IDEMPOTENCY_KEY},
};

static void client_init(void)
{
    const iot_config_t cfg = {
        .serial = SN,
        .host = "mqtt.test",
        .port = 8883,
        .transport = {.connect = t_connect, .send = t_send, .recv = t_recv, .close = t_close},
        .clock = {.mono_ms = c_mono, .epoch_ms = c_epoch},
        .rng = {.fill = r_fill},
        .net_buf = net_buf,
        .net_len = sizeof(net_buf),
        .out_buf = out_buf,
        .out_len = sizeof(out_buf),
        .in_buf = in_buf,
        .in_len = sizeof(in_buf),
        .toks = toks,
        .ntoks = IOT_JSON_MAX_TOKENS,
        .procedures = app_ops,
        .procedure_count = 1,
        .device_type = "gateway-g1",
        .contract_version = 1,
        .status_fill = status_fill,
        .on_event = on_event,
        .on_outbox_item = on_outbox,
    };
    TEST_ASSERT_EQUAL(IOT_OK, iot_init(&client, &cfg));
    TEST_ASSERT_EQUAL(IOT_OK, iot_start(&client));
}

void setUp(void)
{
    rx_len = rx_pos = tx_len = tx_read = 0;
    link_down = connect_fails = false;
    connects = 0;
    now_ms = 1000;
    wall_ms = 1790000000000LL;
    memset(events, 0, sizeof(events));
    handler_calls = 0;
    outbox_calls = 0;
    client_init();
}

void tearDown(void)
{
}

/* --------------------------------------------- pacotes MQTT (lado broker) */

static void rx_put(const uint8_t *b, size_t n)
{
    memcpy(rx + rx_len, b, n);
    rx_len += n;
}

static size_t put_varint(uint8_t *out, size_t v)
{
    size_t n = 0;
    do {
        uint8_t byte = (uint8_t)(v % 128u);
        v /= 128u;
        if (v > 0) {
            byte |= 0x80u;
        }
        out[n++] = byte;
    } while (v > 0);
    return n;
}

static void rx_suback(uint16_t pid, uint8_t code)
{
    const uint8_t p[] = {0x90, 0x04, (uint8_t)(pid >> 8), (uint8_t)pid, code, code};
    rx_put(p, sizeof(p));
}

static void rx_puback(uint16_t pid)
{
    const uint8_t p[] = {0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)pid};
    rx_put(p, sizeof(p));
}

static uint16_t server_pid = 100;

static void rx_publish(const char *topic, const char *payload)
{
    uint8_t p[8192];
    const size_t tl = strlen(topic);
    const size_t pl = strlen(payload);
    size_t n = 0;
    p[n++] = 0x32; /* PUBLISH QoS 1 */
    n += put_varint(p + n, 2 + tl + 2 + pl);
    p[n++] = (uint8_t)(tl >> 8);
    p[n++] = (uint8_t)tl;
    memcpy(p + n, topic, tl);
    n += tl;
    server_pid++;
    p[n++] = (uint8_t)(server_pid >> 8);
    p[n++] = (uint8_t)server_pid;
    memcpy(p + n, payload, pl);
    n += pl;
    rx_put(p, n);
}

/* --------------------------------------------- pacotes MQTT (lado device) */

typedef struct {
    uint8_t type; /* nibble alto */
    uint8_t flags;
    const uint8_t *body;
    size_t len;
} pkt_t;

static bool tx_next(pkt_t *p)
{
    if (tx_read >= tx_len) {
        return false;
    }
    const uint8_t b0 = tx[tx_read];
    size_t i = tx_read + 1, mult = 1, rem = 0;
    uint8_t byte;
    do {
        byte = tx[i++];
        rem += (byte & 0x7Fu) * mult;
        mult *= 128u;
    } while ((byte & 0x80u) != 0);
    p->type = (uint8_t)(b0 & 0xF0u);
    p->flags = (uint8_t)(b0 & 0x0Fu);
    p->body = tx + i;
    p->len = rem;
    tx_read = i + rem;
    return true;
}

/* próximo PUBLISH enviado; devolve tópico e payload como strings */
static bool tx_next_publish(char *topic, size_t tcap, char *payload, size_t pcap, uint8_t *flags)
{
    pkt_t p;
    while (tx_next(&p)) {
        if (p.type != 0x30) {
            continue;
        }
        const size_t tl = (size_t)((p.body[0] << 8) | p.body[1]);
        TEST_ASSERT_TRUE(tl < tcap);
        memcpy(topic, p.body + 2, tl);
        topic[tl] = '\0';
        const size_t off = 2 + tl + (((p.flags >> 1) & 3u) ? 2 : 0);
        const size_t pl = p.len - off;
        TEST_ASSERT_TRUE(pl < pcap);
        memcpy(payload, p.body + off, pl);
        payload[pl] = '\0';
        if (flags != NULL) {
            *flags = p.flags;
        }
        return true;
    }
    return false;
}

static uint16_t last_publish_pid(void)
{
    /* o último PUBLISH QoS1 enviado: pid logo após o tópico */
    size_t save = tx_read;
    tx_read = 0;
    pkt_t p;
    uint16_t pid = 0;
    while (tx_next(&p)) {
        if (p.type == 0x30 && ((p.flags >> 1) & 3u) == 1) {
            const size_t tl = (size_t)((p.body[0] << 8) | p.body[1]);
            pid = (uint16_t)((p.body[2 + tl] << 8) | p.body[3 + tl]);
        }
    }
    tx_read = save;
    return pid;
}

/* conecta e fica online; deixa tx_read depois do "online" */
static void bring_online(void)
{
    iot_step(&client); /* CONNECT + SUBSCRIBE */
    TEST_ASSERT_EQUAL(IOT_STATE_SUBSCRIBING, iot_state(&client));
    pkt_t p;
    TEST_ASSERT_TRUE(tx_next(&p));
    TEST_ASSERT_EQUAL_HEX8(0x10, p.type); /* CONNECT */
    TEST_ASSERT_TRUE(tx_next(&p));
    TEST_ASSERT_EQUAL_HEX8(0x80, p.type); /* SUBSCRIBE */
    const uint16_t pid = (uint16_t)((p.body[0] << 8) | p.body[1]);
    rx_suback(pid, 0x01);
    iot_step(&client);
    TEST_ASSERT_EQUAL(IOT_STATE_ONLINE, iot_state(&client));
    char topic[128], payload[256];
    uint8_t flags = 0;
    TEST_ASSERT_TRUE(tx_next_publish(topic, sizeof(topic), payload, sizeof(payload), &flags));
    TEST_ASSERT_EQUAL_STRING("atrio/" SN "/status", topic);
    TEST_ASSERT_EQUAL_HEX8(0x03, flags & 0x07); /* QoS1 + retain */
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"online\":true"));
    rx_puback(last_publish_pid());
    iot_step(&client);
}

static char request[1024];

static const char *make_request(const char *op, const char *payload, int64_t ts, const char *v)
{
    snprintf(request, sizeof(request),
             "{\"metadata\":{\"v\":\"%s\",\"messageId\":\"m-1\",\"relationId\":\"rel-42\",\"timestamp\":%lld,"
             "\"serialNumber\":\"" SN "\",\"instance\":\"svc-iot-devices-pod-1\",\"traceparent\":\"00-abc-def-01\"},"
             "\"op\":\"%s\",\"payload\":%s}",
             v, (long long)ts, op, payload);
    return request;
}

static int64_t wall_now(void)
{
    return wall_ms + (int64_t)now_ms;
}

/* manda uma device-procedure e devolve a resposta publicada */
static void call_procedure(const char *req, char *reply, size_t cap)
{
    rx_publish("atrio/" SN "/procedure", req);
    iot_step(&client);
    char topic[128];
    TEST_ASSERT_TRUE(tx_next_publish(topic, sizeof(topic), reply, cap, NULL));
    TEST_ASSERT_EQUAL_STRING("atrio/" SN "/procedure/reply/svc-iot-devices-pod-1", topic);
}

/* responde o último request publicado em service com `payload` */
static void reply_service(const char *request_json, const char *op, int rescode, const char *payload)
{
    const char *rel = strstr(request_json, "\"relationId\":\"");
    TEST_ASSERT_NOT_NULL(rel);
    rel += 14;
    char relation[40];
    memcpy(relation, rel, 36);
    relation[36] = '\0';
    char reply[2048];
    snprintf(reply, sizeof(reply),
             "{\"metadata\":{\"v\":\"0.5\",\"messageId\":\"x\",\"relationId\":\"%s\",\"timestamp\":%lld,"
             "\"serialNumber\":\"" SN "\",\"instance\":\"svc-iot-devices-pod-1\"},\"op\":\"%s\","
             "\"payload\":%s,\"rescode\":%d}",
             relation, (long long)(wall_ms + (int64_t)now_ms), op, payload, rescode);
    rx_publish("atrio/" SN "/service/reply", reply);
}

/* próximo request de service com essa op (pula os outros, ex.: keepalive) */
static bool next_service(const char *op, char *payload, size_t cap)
{
    char topic[128];
    char needle[64];
    snprintf(needle, sizeof(needle), "\"op\":\"%s\"", op);
    while (tx_next_publish(topic, sizeof(topic), payload, cap, NULL)) {
        if (strcmp(topic, "atrio/" SN "/service") == 0 && strstr(payload, needle) != NULL) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ testes */

static void test_connect_publishes_will_and_online(void)
{
    iot_step(&client);
    pkt_t p;
    TEST_ASSERT_TRUE(tx_next(&p));
    TEST_ASSERT_EQUAL_HEX8(0x10, p.type);
    /* flags do CONNECT: clean session, will QoS1, will retain */
    const uint8_t cflags = p.body[7];
    TEST_ASSERT_TRUE((cflags & 0x02) != 0);
    TEST_ASSERT_TRUE((cflags & 0x04) != 0);
    TEST_ASSERT_EQUAL_HEX8(0x08, cflags & 0x18);
    TEST_ASSERT_TRUE((cflags & 0x20) != 0);
    TEST_ASSERT_TRUE(memmem(p.body, p.len, SN, 12) != NULL);
    TEST_ASSERT_TRUE(memmem(p.body, p.len, "{\"online\":false", 15) != NULL);
    /* assina exatamente procedure e service/reply */
    TEST_ASSERT_TRUE(tx_next(&p));
    TEST_ASSERT_EQUAL_HEX8(0x80, p.type);
    TEST_ASSERT_TRUE(memmem(p.body, p.len, "atrio/" SN "/procedure", 24) != NULL);
    TEST_ASSERT_TRUE(memmem(p.body, p.len, "atrio/" SN "/service/reply", 28) != NULL);
    TEST_ASSERT_NULL(memmem(p.body, p.len, "#", 1));

    tx_read = 0;
    tx_len = 0;
    setUp();
    bring_online();
    TEST_ASSERT_EQUAL_INT(1, events[IOT_EVENT_ONLINE]);
}

static void test_get_status_echoes_routing(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("get-status", "{}", wall_now(), "0.5"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"relationId\":\"rel-42\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"instance\":\"svc-iot-devices-pod-1\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"traceparent\":\"00-abc-def-01\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"payload\":{\"uptimeS\":42}"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"rescode\":200"));
    TEST_ASSERT_NULL(strstr(reply, "\"messageId\":\"m-1\"")); /* messageId é novo */
}

static void test_list_procedures(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("list-procedures", "{}", wall_now(), "0.5"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "{\"op\":\"get-status\",\"version\":1}"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "{\"op\":\"relay-set\",\"version\":2}"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"deviceType\":\"gateway-g1\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"contractVersion\":1"));
}

static void test_outbox_notification_is_204(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("outbox-notification", "{\"entities\":[\"x\"],\"size\":1}", wall_now(), "0.5"),
                   reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"payload\":{},\"rescode\":204"));
    TEST_ASSERT_EQUAL_INT(1, events[IOT_EVENT_OUTBOX_NOTIFIED]);
}

static void test_unknown_op_is_404(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("nao-existe", "{}", wall_now(), "0.5"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"UNKNOWN_OP\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"rescode\":404"));
}

static void test_expired_request_is_504_not_silent(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("get-status", "{}", wall_now() - 70000, "0.5"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"TTL_EXPIRED\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"rescode\":504"));
}

static void test_timestamp_in_seconds_is_accepted(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("get-status", "{}", wall_now() / 1000, "0.5"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"rescode\":200"));
}

static void test_bad_version_and_envelope_are_400(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("get-status", "{}", wall_now(), "1.0"), reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "UNSUPPORTED_VERSION"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"rescode\":400"));

    /* chave extra na raiz */
    char req[1024];
    snprintf(req, sizeof(req), "%.*s,\"extra\":1}", (int)strlen(make_request("get-status", "{}", wall_now(), "0.5")) - 1,
             request);
    call_procedure(req, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "INVALID_ENVELOPE"));
}

static void test_without_relation_is_dropped(void)
{
    bring_online();
    rx_publish("atrio/" SN "/procedure", "{\"metadata\":{\"v\":\"0.5\"},\"op\":\"get-status\",\"payload\":{}}");
    iot_step(&client);
    char topic[128], payload[256];
    TEST_ASSERT_FALSE(tx_next_publish(topic, sizeof(topic), payload, sizeof(payload), NULL));
    rx_publish("atrio/" SN "/procedure", "isto nao e json");
    iot_step(&client);
    TEST_ASSERT_FALSE(tx_next_publish(topic, sizeof(topic), payload, sizeof(payload), NULL));
    TEST_ASSERT_EQUAL(IOT_STATE_ONLINE, iot_state(&client));
}

static void test_idempotency_key_runs_once(void)
{
    bring_online();
    char reply1[2048], reply2[2048];
    call_procedure(make_request("relay-set", "{\"channel\":1,\"state\":\"on\",\"idempotencyKey\":\"k-1\"}",
                                wall_now(), "0.5"),
                   reply1, sizeof(reply1));
    call_procedure(make_request("relay-set", "{\"channel\":1,\"state\":\"on\",\"idempotencyKey\":\"k-1\"}",
                                wall_now(), "0.5"),
                   reply2, sizeof(reply2));
    TEST_ASSERT_EQUAL_INT(1, handler_calls);
    TEST_ASSERT_NOT_NULL(strstr(reply2, "\"payload\":{\"state\":\"on\"},\"rescode\":200"));
    call_procedure(make_request("relay-set", "{\"channel\":1,\"state\":\"on\",\"idempotencyKey\":\"k-2\"}",
                                wall_now(), "0.5"),
                   reply2, sizeof(reply2));
    TEST_ASSERT_EQUAL_INT(2, handler_calls);
}

static void test_keepalive_learns_server_clock(void)
{
    wall_ms = 0; /* sem SNTP */
    client_init();
    bring_online();
    TEST_ASSERT_EQUAL_INT64(0, iot_now_epoch_ms(&client));
    now_ms += 2500; /* primeiro keepalive sai em até 2 s */
    iot_step(&client);
    char payload[1024];
    TEST_ASSERT_TRUE(next_service("keepalive", payload, sizeof(payload)));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"intervalMs\":60000"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"instance\":\"" SN "\""));
    /* resposta com o relationId do request */
    const char *rel = strstr(payload, "\"relationId\":\"") + 14;
    char relation[40];
    memcpy(relation, rel, 36);
    relation[36] = '\0';
    char reply[512];
    snprintf(reply, sizeof(reply),
             "{\"metadata\":{\"v\":\"0.5\",\"messageId\":\"x\",\"relationId\":\"%s\",\"timestamp\":1790000000000,"
             "\"serialNumber\":\"" SN "\",\"instance\":\"svc-iot-devices-pod-1\"},\"op\":\"keepalive\","
             "\"payload\":{\"serverTime\":1790000000500,\"settingsVersion\":1,\"settingsResync\":true},\"rescode\":200}",
             relation);
    rx_publish("atrio/" SN "/service/reply", reply);
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(1, events[IOT_EVENT_KEEPALIVE_OK]);
    TEST_ASSERT_EQUAL_INT(1, events[IOT_EVENT_SETTINGS_RESYNC]);
    TEST_ASSERT_INT64_WITHIN(5, 1790000000500LL, iot_now_epoch_ms(&client));
}

static int rpc_rescode;
static void rpc_cb(iot_client_t *c, int rescode, const iot_json_doc_t *doc, iot_json_ref_t payload, void *user)
{
    (void)c;
    (void)doc;
    (void)payload;
    (void)user;
    rpc_rescode = rescode;
}

static void test_service_timeout_is_504(void)
{
    bring_online();
    rpc_rescode = 0;
    TEST_ASSERT_EQUAL(IOT_OK, iot_call_service(&client, "list-services", NULL, NULL, rpc_cb, NULL, 5000));
    now_ms += 4000;
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(0, rpc_rescode);
    now_ms += 1001;
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(504, rpc_rescode);
}

static void test_link_drop_backs_off_and_reconnects(void)
{
    bring_online();
    rpc_rescode = 0;
    TEST_ASSERT_EQUAL(IOT_OK, iot_call_service(&client, "list-services", NULL, NULL, rpc_cb, NULL, 0));
    link_down = true;
    iot_step(&client);
    TEST_ASSERT_EQUAL(IOT_STATE_BACKOFF, iot_state(&client));
    TEST_ASSERT_EQUAL_INT(1, events[IOT_EVENT_OFFLINE]);
    TEST_ASSERT_EQUAL_INT(503, rpc_rescode); /* pendente falha na hora, sem esperar 30 s */

    const int before = connects;
    now_ms += 400; /* backoff mínimo sorteado é >= 500 ms */
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(before, connects);
    now_ms += 1000;
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(before + 1, connects);
    TEST_ASSERT_EQUAL(IOT_STATE_SUBSCRIBING, iot_state(&client));
}

static void test_backoff_grows_while_connect_fails(void)
{
    connect_fails = true;
    uint64_t last_gap = 0;
    for (int i = 0; i < 8; i++) {
        iot_step(&client);
        TEST_ASSERT_EQUAL(IOT_STATE_BACKOFF, iot_state(&client));
        const uint64_t gap = client.next_attempt_ms - now_ms;
        TEST_ASSERT_TRUE(gap <= 60000);
        if (i >= 6) {
            TEST_ASSERT_TRUE(gap >= 30000); /* chegou perto do teto */
        }
        last_gap = gap;
        now_ms = client.next_attempt_ms;
    }
    TEST_ASSERT_TRUE(last_gap > 0);
}

static void test_suback_refused_drops(void)
{
    iot_step(&client);
    pkt_t p;
    tx_next(&p);
    tx_next(&p);
    const uint16_t pid = (uint16_t)((p.body[0] << 8) | p.body[1]);
    rx_suback(pid, 0x80);
    iot_step(&client);
    TEST_ASSERT_EQUAL(IOT_STATE_BACKOFF, iot_state(&client));
    TEST_ASSERT_EQUAL_INT(0, events[IOT_EVENT_ONLINE]);
}

static void test_stop_publishes_offline(void)
{
    bring_online();
    iot_stop(&client);
    char topic[128], payload[256];
    uint8_t flags = 0;
    TEST_ASSERT_TRUE(tx_next_publish(topic, sizeof(topic), payload, sizeof(payload), &flags));
    TEST_ASSERT_EQUAL_STRING("atrio/" SN "/status", topic);
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"online\":false"));
    TEST_ASSERT_EQUAL_HEX8(0x01, flags & 0x01);
    TEST_ASSERT_EQUAL(IOT_STATE_STOPPED, iot_state(&client));
}

static void test_outbox_drained_on_connect_with_ack_and_reject(void)
{
    bring_online();
    now_ms += 4000; /* dreno sorteado em até 3,5 s */
    iot_step(&client);
    char req[2048];
    TEST_ASSERT_TRUE(next_service("outbox-read", req, sizeof(req)));
    TEST_ASSERT_NOT_NULL(strstr(req, "\"limit\":5"));
    reply_service(req, "outbox-read", 200,
                  "{\"count\":2,\"items\":["
                  "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"settings-write\",\"entity\":\"settings\","
                  "\"entityId\":\"d\",\"payload\":{\"version\":2,\"settings\":[]},\"createdAt\":1,\"leaseUntil\":2},"
                  "{\"id\":\"22222222-2222-4222-8222-222222222222\",\"command\":\"plans-put\",\"entity\":\"plans\","
                  "\"entityId\":\"d\",\"payload\":{},\"createdAt\":1,\"leaseUntil\":2}]}");
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(2, outbox_calls);
    TEST_ASSERT_TRUE(next_service("outbox-ack", req, sizeof(req)));
    TEST_ASSERT_NOT_NULL(strstr(req, "\"ids\":[\"11111111-1111-4111-8111-111111111111\"]"));
    TEST_ASSERT_NOT_NULL(strstr(req, "\"rejects\":[{\"id\":\"22222222-2222-4222-8222-222222222222\",\"reason\":\"UNKNOWN_COMMAND\"}]"));
    reply_service(req, "outbox-ack", 200, "{\"acknowledged\":1,\"rejected\":1}");
    iot_step(&client);
    /* lote menor que o limite: não lê de novo agora */
    now_ms += 10;
    iot_step(&client);
    TEST_ASSERT_FALSE(next_service("outbox-read", req, sizeof(req)));
}

static void test_outbox_notification_triggers_drain(void)
{
    bring_online();
    char reply[2048];
    call_procedure(make_request("outbox-notification", "{\"entities\":[\"settings\"],\"size\":1}", wall_now(), "0.5"),
                   reply, sizeof(reply));
    iot_step(&client);
    char req[2048];
    TEST_ASSERT_TRUE(next_service("outbox-read", req, sizeof(req)));
    reply_service(req, "outbox-read", 200, "{\"count\":0,\"items\":[]}");
    iot_step(&client);
    TEST_ASSERT_EQUAL_INT(0, outbox_calls);
}

static void test_outbox_waits_for_clock(void)
{
    wall_ms = 0; /* sem SNTP: só a hora do servidor serve */
    client_init();
    bring_online();
    now_ms += 4000;
    iot_step(&client);
    char req[2048];
    /* só o keepalive sai; nada de outbox-read sem hora */
    char ka[2048] = "";
    char topic[128];
    while (tx_next_publish(topic, sizeof(topic), req, sizeof(req), NULL)) {
        TEST_ASSERT_NULL(strstr(req, "\"op\":\"outbox-read\""));
        if (strstr(req, "\"op\":\"keepalive\"") != NULL) {
            strcpy(ka, req);
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, ka[0]);
    const char *rel = strstr(ka, "\"relationId\":\"") + 14;
    char relation[40];
    memcpy(relation, rel, 36);
    relation[36] = '\0';
    char reply[512];
    snprintf(reply, sizeof(reply),
             "{\"metadata\":{\"v\":\"0.5\",\"messageId\":\"x\",\"relationId\":\"%s\",\"timestamp\":1790000000000,"
             "\"serialNumber\":\"" SN "\",\"instance\":\"svc-iot-devices-pod-1\"},\"op\":\"keepalive\","
             "\"payload\":{\"serverTime\":1790000000500},\"rescode\":200}",
             relation);
    rx_publish("atrio/" SN "/service/reply", reply);
    iot_step(&client);
    iot_step(&client);
    TEST_ASSERT_TRUE(next_service("outbox-read", req, sizeof(req)));
}

static void test_outbox_read_failure_retries_soon(void)
{
    bring_online();
    now_ms += 4000;
    iot_step(&client);
    char req[2048];
    TEST_ASSERT_TRUE(next_service("outbox-read", req, sizeof(req)));
    reply_service(req, "outbox-read", 504, "{\"error\":\"TTL_EXPIRED\"}");
    iot_step(&client);
    now_ms += 5000;
    iot_step(&client);
    TEST_ASSERT_FALSE(next_service("outbox-read", req, sizeof(req)));
    now_ms += 11000; /* 10 s + até 5 s de sorteio, bem antes do poll de 5 min */
    iot_step(&client);
    TEST_ASSERT_TRUE(next_service("outbox-read", req, sizeof(req)));
}

static void applied_fill(iot_client_t *c, iot_json_writer_t *w, void *user)
{
    (void)c;
    (void)user;
    iot_jw_key(w, "appliedVersion");
    iot_jw_u64(w, 7);
}

static void test_inbox_write_shape(void)
{
    bring_online();
    TEST_ASSERT_EQUAL(IOT_OK, iot_inbox_write(&client, "settings-applied", "settings", 7, applied_fill, NULL, NULL, NULL));
    char req[2048];
    TEST_ASSERT_TRUE(next_service("inbox-write", req, sizeof(req)));
    TEST_ASSERT_NOT_NULL(strstr(req, "\"payload\":{\"entity\":\"settings-applied\",\"entityId\":\"settings\",\"version\":7,"
                                      "\"payload\":{\"appliedVersion\":7}}"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_connect_publishes_will_and_online);
    RUN_TEST(test_get_status_echoes_routing);
    RUN_TEST(test_list_procedures);
    RUN_TEST(test_outbox_notification_is_204);
    RUN_TEST(test_unknown_op_is_404);
    RUN_TEST(test_expired_request_is_504_not_silent);
    RUN_TEST(test_timestamp_in_seconds_is_accepted);
    RUN_TEST(test_bad_version_and_envelope_are_400);
    RUN_TEST(test_without_relation_is_dropped);
    RUN_TEST(test_idempotency_key_runs_once);
    RUN_TEST(test_keepalive_learns_server_clock);
    RUN_TEST(test_service_timeout_is_504);
    RUN_TEST(test_link_drop_backs_off_and_reconnects);
    RUN_TEST(test_backoff_grows_while_connect_fails);
    RUN_TEST(test_suback_refused_drops);
    RUN_TEST(test_stop_publishes_offline);
    RUN_TEST(test_outbox_drained_on_connect_with_ack_and_reject);
    RUN_TEST(test_outbox_notification_triggers_drain);
    RUN_TEST(test_inbox_write_shape);
    RUN_TEST(test_outbox_waits_for_clock);
    RUN_TEST(test_outbox_read_failure_retries_soon);
    return UNITY_END();
}
