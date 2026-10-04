#include <string.h>

#include "envelope/iot_envelope.h"
#include "session/iot_backoff.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_backoff_range_and_growth(void)
{
    uint32_t cur = 1000;
    for (uint32_t r = 0; r < 50; r++) {
        uint32_t c = 1000;
        const uint32_t w = iot_backoff_next(&c, 1000, 60000, r * 7919u);
        TEST_ASSERT_TRUE(w >= 500 && w <= 1000);
        TEST_ASSERT_EQUAL_UINT32(2000, c);
    }
    for (int i = 0; i < 10; i++) {
        iot_backoff_next(&cur, 1000, 60000, 0);
    }
    TEST_ASSERT_EQUAL_UINT32(60000, cur);
    const uint32_t w = iot_backoff_next(&cur, 1000, 60000, 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(w >= 30000 && w <= 60000);
}

static void test_uuid_v4_format(void)
{
    uint8_t rnd[16];
    memset(rnd, 0xFF, sizeof(rnd));
    char out[IOT_ENV_UUID_LEN + 1];
    iot_uuid_v4(rnd, out);
    TEST_ASSERT_EQUAL_size_t(36, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('-', out[8]);
    TEST_ASSERT_EQUAL_CHAR('4', out[14]);
    TEST_ASSERT_TRUE(out[19] == '8' || out[19] == '9' || out[19] == 'a' || out[19] == 'b');
}

static iot_json_tok_t toks[64];

static iot_env_status_t read_env(const char *s, bool resp, iot_envelope_t *e)
{
    iot_json_doc_t doc;
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_parse(&doc, s, strlen(s), toks, 64));
    return iot_envelope_read(&doc, resp, e);
}

static void test_envelope_read(void)
{
    iot_envelope_t e;
    TEST_ASSERT_EQUAL(IOT_ENV_OK,
                      read_env("{\"metadata\":{\"v\":\"0.4\",\"messageId\":\"a\",\"relationId\":\"r\",\"timestamp\":"
                               "1751982573,\"serialNumber\":\"S\",\"instance\":\"i\"},\"op\":\"x\",\"payload\":{}}",
                               false, &e));
    TEST_ASSERT_EQUAL_INT64(1751982573000LL, e.timestamp_ms);
    TEST_ASSERT_EQUAL_STRING("r", e.relation_id);
    TEST_ASSERT_EQUAL_STRING("", e.traceparent);

    /* request com rescode é inválido; resposta sem rescode também */
    TEST_ASSERT_EQUAL(IOT_ENV_INVALID,
                      read_env("{\"metadata\":{\"v\":\"0.5\",\"messageId\":\"a\",\"relationId\":\"r\",\"timestamp\":1,"
                               "\"serialNumber\":\"S\",\"instance\":\"i\"},\"op\":\"x\",\"payload\":{},\"rescode\":200}",
                               false, &e));
    TEST_ASSERT_EQUAL(IOT_ENV_INVALID,
                      read_env("{\"metadata\":{\"v\":\"0.5\",\"messageId\":\"a\",\"relationId\":\"r\",\"timestamp\":1,"
                               "\"serialNumber\":\"S\",\"instance\":\"i\"},\"op\":\"x\",\"payload\":{}}",
                               true, &e));
    TEST_ASSERT_EQUAL(IOT_ENV_NO_RELATION, read_env("{\"metadata\":{},\"op\":\"x\",\"payload\":{}}", false, &e));
    TEST_ASSERT_EQUAL(IOT_ENV_BAD_VERSION,
                      read_env("{\"metadata\":{\"v\":\"1.0\",\"messageId\":\"a\",\"relationId\":\"r\",\"timestamp\":1,"
                               "\"serialNumber\":\"S\",\"instance\":\"i\"},\"op\":\"x\",\"payload\":{}}",
                               false, &e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_backoff_range_and_growth);
    RUN_TEST(test_uuid_v4_format);
    RUN_TEST(test_envelope_read);
    return UNITY_END();
}
