#include "envelope/iot_envelope.h"

#include <stdio.h>
#include <string.h>

#include <dextro-iot/iot_types.h>

static bool get_str(const iot_json_doc_t *doc, iot_json_ref_t obj, const char *key, char *out, size_t cap)
{
    return iot_json_get_str(doc, iot_json_get(doc, obj, key), out, cap, NULL) == IOT_OK && out[0] != '\0';
}

iot_env_status_t iot_envelope_read(const iot_json_doc_t *doc, bool expect_response, iot_envelope_t *e)
{
    memset(e, 0, sizeof(*e));
    e->rescode = -1;
    const iot_json_ref_t root = iot_json_root(doc);
    if (iot_json_type(doc, root) != IOT_JSON_OBJECT) {
        return IOT_ENV_NO_RELATION;
    }
    e->metadata = iot_json_get(doc, root, "metadata");
    if (iot_json_type(doc, e->metadata) != IOT_JSON_OBJECT ||
        !get_str(doc, e->metadata, "relationId", e->relation_id, sizeof(e->relation_id))) {
        return IOT_ENV_NO_RELATION;
    }
    /* instance diz para onde vai a resposta de device-procedure */
    if (!get_str(doc, e->metadata, "instance", e->instance, sizeof(e->instance))) {
        return expect_response ? IOT_ENV_INVALID : IOT_ENV_NO_RELATION;
    }
    get_str(doc, e->metadata, "traceparent", e->traceparent, sizeof(e->traceparent));

    /* raiz: só metadata, op, payload e (na resposta) rescode */
    iot_json_ref_t k = IOT_JSON_NONE;
    iot_json_ref_t v = IOT_JSON_NONE;
    bool extra = false;
    while (iot_json_next_member(doc, root, &k, &v)) {
        if (!iot_json_str_eq(doc, k, "metadata") && !iot_json_str_eq(doc, k, "op") &&
            !iot_json_str_eq(doc, k, "payload") && !(expect_response && iot_json_str_eq(doc, k, "rescode"))) {
            extra = true;
        }
    }
    e->payload = iot_json_get(doc, root, "payload");
    e->rescode_ref = iot_json_get(doc, root, "rescode");
    char msgid[IOT_ENV_UUID_LEN + 8];
    if (extra || e->payload == IOT_JSON_NONE || !get_str(doc, root, "op", e->op, sizeof(e->op)) ||
        !get_str(doc, e->metadata, "messageId", msgid, sizeof(msgid)) ||
        !get_str(doc, e->metadata, "serialNumber", e->serial, sizeof(e->serial)) ||
        !get_str(doc, e->metadata, "v", e->version, sizeof(e->version)) ||
        iot_json_get_i64(doc, iot_json_get(doc, e->metadata, "timestamp"), &e->timestamp_ms) != IOT_OK) {
        return IOT_ENV_INVALID;
    }
    if (e->timestamp_ms < 1000000000000LL) {
        e->timestamp_ms *= 1000; /* segundos (compat atrio-library) */
    }
    if (expect_response) {
        int64_t rc = 0;
        if (iot_json_get_i64(doc, e->rescode_ref, &rc) != IOT_OK || rc < 100 || rc > 599) {
            return IOT_ENV_INVALID;
        }
        e->rescode = (int)rc;
    } else if (e->rescode_ref != IOT_JSON_NONE) {
        return IOT_ENV_INVALID;
    }
    if (strncmp(e->version, "0.", 2) != 0) {
        return IOT_ENV_BAD_VERSION;
    }
    return IOT_ENV_OK;
}

void iot_uuid_v4(const uint8_t rnd[16], char out[IOT_ENV_UUID_LEN + 1])
{
    uint8_t b[16];
    memcpy(b, rnd, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* versão 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* variante RFC 4122 */
    snprintf(out, IOT_ENV_UUID_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
             b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void iot_envelope_write_metadata(iot_json_writer_t *w, const char *message_id, const char *relation_id,
                                 int64_t timestamp_ms, const char *serial, const char *instance,
                                 const char *traceparent)
{
    iot_jw_key(w, "metadata");
    iot_jw_obj_begin(w);
    iot_jw_key(w, "v");
    iot_jw_str(w, IOT_PROTOCOL_VERSION);
    iot_jw_key(w, "messageId");
    iot_jw_str(w, message_id);
    iot_jw_key(w, "relationId");
    iot_jw_str(w, relation_id);
    iot_jw_key(w, "timestamp");
    iot_jw_i64(w, timestamp_ms);
    iot_jw_key(w, "serialNumber");
    iot_jw_str(w, serial);
    iot_jw_key(w, "instance");
    iot_jw_str(w, instance);
    if (traceparent != NULL && traceparent[0] != '\0') {
        iot_jw_key(w, "traceparent");
        iot_jw_str(w, traceparent);
    }
    iot_jw_obj_end(w);
}
