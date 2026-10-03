#include <dextro-iot/iot_json.h>

#include <string.h>

#include "unity.h"

static iot_json_tok_t toks[IOT_JSON_MAX_TOKENS];
static iot_json_doc_t doc;

void setUp(void)
{
    memset(&doc, 0, sizeof(doc));
}

void tearDown(void)
{
}

static iot_err_t parse(const char *s)
{
    return iot_json_parse(&doc, s, strlen(s), toks, IOT_JSON_MAX_TOKENS);
}

/* ------------------------------------------------------------------ leitor */

static void test_parse_envelope(void)
{
    const char *s = "{\"metadata\":{\"v\":\"0.5\",\"timestamp\":1751982573000},"
                    "\"op\":\"get-status\",\"payload\":{\"sections\":[\"all\",\"io\"]}}";
    TEST_ASSERT_EQUAL(IOT_OK, parse(s));

    const iot_json_ref_t root = iot_json_root(&doc);
    TEST_ASSERT_EQUAL(IOT_JSON_OBJECT, iot_json_type(&doc, root));
    TEST_ASSERT_EQUAL_INT(3, iot_json_size(&doc, root));

    TEST_ASSERT_TRUE(iot_json_str_eq(&doc, iot_json_get(&doc, root, "op"), "get-status"));

    const iot_json_ref_t meta = iot_json_get(&doc, root, "metadata");
    int64_t ts = 0;
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_i64(&doc, iot_json_get(&doc, meta, "timestamp"), &ts));
    TEST_ASSERT_EQUAL_INT64(1751982573000LL, ts);

    const iot_json_ref_t sections = iot_json_get(&doc, iot_json_get(&doc, root, "payload"), "sections");
    TEST_ASSERT_EQUAL(IOT_JSON_ARRAY, iot_json_type(&doc, sections));
    TEST_ASSERT_EQUAL_INT(2, iot_json_size(&doc, sections));
    TEST_ASSERT_TRUE(iot_json_str_eq(&doc, iot_json_at(&doc, sections, 1), "io"));
    TEST_ASSERT_EQUAL(IOT_JSON_NONE, iot_json_at(&doc, sections, 2));
}

static void test_get_skips_nested_values(void)
{
    /* "op" dentro do payload não pode ser confundido com o "op" da raiz */
    TEST_ASSERT_EQUAL(IOT_OK, parse("{\"payload\":{\"op\":\"x\",\"a\":[{\"op\":1}]},\"op\":\"real\"}"));
    TEST_ASSERT_TRUE(iot_json_str_eq(&doc, iot_json_get(&doc, 0, "op"), "real"));
    TEST_ASSERT_EQUAL(IOT_JSON_NONE, iot_json_get(&doc, 0, "missing"));
    TEST_ASSERT_EQUAL(IOT_JSON_NONE, iot_json_get(&doc, IOT_JSON_NONE, "op"));
}

static void test_iterate_members(void)
{
    TEST_ASSERT_EQUAL(IOT_OK, parse("{\"a\":1,\"b\":{\"c\":[1,2]},\"d\":true}"));
    const char *keys[] = {"a", "b", "d"};
    iot_json_ref_t k = IOT_JSON_NONE;
    iot_json_ref_t v = IOT_JSON_NONE;
    int n = 0;
    while (iot_json_next_member(&doc, 0, &k, &v)) {
        TEST_ASSERT_LESS_THAN_INT(3, n);
        TEST_ASSERT_TRUE(iot_json_str_eq(&doc, k, keys[n]));
        n++;
    }
    TEST_ASSERT_EQUAL_INT(3, n);

    TEST_ASSERT_EQUAL(IOT_OK, parse("{}"));
    k = IOT_JSON_NONE;
    TEST_ASSERT_FALSE(iot_json_next_member(&doc, 0, &k, &v));
}

static void test_raw_returns_subdocument(void)
{
    TEST_ASSERT_EQUAL(IOT_OK, parse("{\"payload\": {\"x\": [1, 2]} , \"s\":\"a\\nb\"}"));
    const char *text = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_raw(&doc, iot_json_get(&doc, 0, "payload"), &text, &len));
    TEST_ASSERT_EQUAL_STRING_LEN("{\"x\": [1, 2]}", text, len);
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_raw(&doc, iot_json_get(&doc, 0, "s"), &text, &len));
    TEST_ASSERT_EQUAL_STRING_LEN("a\\nb", text, len);
}

static void test_types(void)
{
    TEST_ASSERT_EQUAL(IOT_OK, parse("[1, -2.5e3, true, false, null, \"s\", {}, []]"));
    const iot_json_type_t expected[] = {IOT_JSON_NUMBER, IOT_JSON_NUMBER, IOT_JSON_BOOL,   IOT_JSON_BOOL,
                                        IOT_JSON_NULL,   IOT_JSON_STRING, IOT_JSON_OBJECT, IOT_JSON_ARRAY};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(expected[i], iot_json_type(&doc, iot_json_at(&doc, 0, i)));
    }
    bool b = false;
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_bool(&doc, iot_json_at(&doc, 0, 2), &b));
    TEST_ASSERT_TRUE(b);
    TEST_ASSERT_EQUAL(IOT_ERR_TYPE, iot_json_get_bool(&doc, iot_json_at(&doc, 0, 0), &b));
    int64_t v = 0;
    TEST_ASSERT_EQUAL(IOT_ERR_TYPE, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 1), &v)); /* fração */
    TEST_ASSERT_EQUAL(IOT_ERR_TYPE, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 5), &v));
    TEST_ASSERT_EQUAL(IOT_ERR_NOT_FOUND, iot_json_get_i64(&doc, IOT_JSON_NONE, &v));
}

static void test_i64_limits(void)
{
    int64_t v = 0;
    TEST_ASSERT_EQUAL(IOT_OK, parse("[9223372036854775807,-9223372036854775808,0,-0]"));
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 0), &v));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, v);
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 1), &v));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, v);
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 3), &v));
    TEST_ASSERT_EQUAL_INT64(0, v);

    TEST_ASSERT_EQUAL(IOT_OK, parse("[9223372036854775808,-9223372036854775809]"));
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 0), &v));
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, iot_json_get_i64(&doc, iot_json_at(&doc, 0, 1), &v));
}

static void test_get_str_decodes_escapes(void)
{
    char out[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IOT_OK, parse("[\"a\\\"b\\\\c\\/d\\n\\t\", \"\\u00e7\\u20ac\", \"\\ud83d\\ude00\"]"));
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_str(&doc, iot_json_at(&doc, 0, 0), out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\n\t", out);
    TEST_ASSERT_EQUAL_size_t(9, len);
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_str(&doc, iot_json_at(&doc, 0, 1), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("\xC3\xA7\xE2\x82\xAC", out); /* ç€ */
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_str(&doc, iot_json_at(&doc, 0, 2), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", out); /* emoji, par de surrogates */
}

static void test_get_str_rejects_lone_surrogate(void)
{
    char out[16];
    TEST_ASSERT_EQUAL(IOT_OK, parse("[\"\\ud83d\", \"\\ude00\", \"\\ud83dx\"]"));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(IOT_ERR_PARSE, iot_json_get_str(&doc, iot_json_at(&doc, 0, i), out, sizeof(out), NULL));
    }
}

static void test_get_str_no_space(void)
{
    char out[4];
    TEST_ASSERT_EQUAL(IOT_OK, parse("[\"abc\", \"abcd\", \"\\u20ac\\u20ac\"]"));
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_str(&doc, iot_json_at(&doc, 0, 0), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("abc", out);
    TEST_ASSERT_EQUAL(IOT_ERR_NO_SPACE, iot_json_get_str(&doc, iot_json_at(&doc, 0, 1), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(IOT_ERR_NO_SPACE, iot_json_get_str(&doc, iot_json_at(&doc, 0, 2), out, sizeof(out), NULL));
}

static void test_rejects_malformed(void)
{
    static const char *const bad[] = {
        "",
        "   ",
        "{",
        "{\"a\":1",
        "{\"a\":}",
        "{\"a\" 1}",
        "{\"a\"::1}",
        "{\"a\":1,}",
        "{,\"a\":1}",
        "{\"a\":1 \"b\":2}",
        "{a:1}",
        "{1:1}",
        "[1,,2]",
        "[1,]",
        "[,1]",
        "[1 2]",
        "{\"a\":tru}",
        "{\"a\":nul}",
        "{\"a\":01}",
        "{\"a\":1.}",
        "{\"a\":.5}",
        "{\"a\":1e}",
        "{\"a\":-}",
        "{\"a\":+1}",
        "{\"a\":\"x\ny\"}",
        "{\"a\":\"\\x\"}",
        "{\"a\":\"\\u12\"}",
        "{} {}",
        "{}x",
        "\"solta\"",
        "42",
        "[1]]",
        "{\"a\":[1}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const iot_err_t err = parse(bad[i]);
        if (err != IOT_ERR_PARSE) {
            TEST_PRINTF("aceitou ou errou o código: [%s] -> %s", bad[i], iot_err_str(err));
        }
        TEST_ASSERT_EQUAL(IOT_ERR_PARSE, err);
        TEST_ASSERT_EQUAL(IOT_JSON_NONE, iot_json_root(&doc));
    }
}

static void test_accepts_valid_edge_cases(void)
{
    static const char *const good[] = {
        "{}",
        "[]",
        " \t\r\n{ \"a\" : [ ] , \"b\" : { } } \n",
        "[0,-0,1.5,-1.5e-3,2E+10,true,false,null]",
        "{\"\":\"\"}",
        "[\"\\u0000\"]",
        "{\"utf8\":\"a\xC3\xA7\xC3\xA3o\"}",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        const iot_err_t err = parse(good[i]);
        if (err != IOT_OK) {
            TEST_PRINTF("recusou: [%s] -> %s", good[i], iot_err_str(err));
        }
        TEST_ASSERT_EQUAL(IOT_OK, err);
    }
}

static void test_token_limit(void)
{
    iot_json_tok_t small[4];
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, iot_json_parse(&doc, "[1,2,3,4]", 9, small, 4)); /* 5 tokens */
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_parse(&doc, "[1,2,3]", 7, small, 4));          /* 4 tokens */
    TEST_ASSERT_EQUAL_INT(4, doc.ntoks);
}

static void test_depth_limit(void)
{
    char s[2 * (IOT_JSON_MAX_DEPTH + 1) + 1];
    /* exatamente no limite: aceita */
    for (size_t i = 0; i < IOT_JSON_MAX_DEPTH; i++) {
        s[i] = '[';
        s[2 * IOT_JSON_MAX_DEPTH - 1 - i] = ']';
    }
    s[2 * IOT_JSON_MAX_DEPTH] = '\0';
    TEST_ASSERT_EQUAL(IOT_OK, parse(s));

    /* um nível acima: recusa */
    for (size_t i = 0; i < IOT_JSON_MAX_DEPTH + 1; i++) {
        s[i] = '[';
        s[2 * (IOT_JSON_MAX_DEPTH + 1) - 1 - i] = ']';
    }
    s[2 * (IOT_JSON_MAX_DEPTH + 1)] = '\0';
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, parse(s));
}

static void test_payload_limit(void)
{
    static char big[IOT_MAX_PAYLOAD + 2];
    memset(big, ' ', sizeof(big));
    big[0] = '{';
    big[IOT_MAX_PAYLOAD - 1] = '}';
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_parse(&doc, big, IOT_MAX_PAYLOAD, toks, IOT_JSON_MAX_TOKENS));
    big[IOT_MAX_PAYLOAD - 1] = ' ';
    big[IOT_MAX_PAYLOAD] = '}';
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, iot_json_parse(&doc, big, IOT_MAX_PAYLOAD + 1, toks, IOT_JSON_MAX_TOKENS));
}

static void test_parse_args(void)
{
    TEST_ASSERT_EQUAL(IOT_ERR_ARG, iot_json_parse(&doc, NULL, 0, toks, IOT_JSON_MAX_TOKENS));
    TEST_ASSERT_EQUAL(IOT_ERR_ARG, iot_json_parse(&doc, "{}", 2, NULL, 1));
    TEST_ASSERT_EQUAL(IOT_ERR_ARG, iot_json_parse(&doc, "{}", 2, toks, 0));
}

/* ---------------------------------------------------------------- escritor */

static void test_writer_builds_response(void)
{
    char buf[256];
    iot_json_writer_t w;
    size_t len = 0;
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "op");
    iot_jw_str(&w, "list-procedures");
    iot_jw_key(&w, "payload");
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "procedures");
    iot_jw_arr_begin(&w);
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "op");
    iot_jw_str(&w, "get-status");
    iot_jw_obj_end(&w);
    iot_jw_obj_begin(&w);
    iot_jw_obj_end(&w);
    iot_jw_arr_end(&w);
    iot_jw_obj_end(&w);
    iot_jw_key(&w, "rescode");
    iot_jw_i64(&w, 200);
    iot_jw_key(&w, "neg");
    iot_jw_i64(&w, INT64_MIN);
    iot_jw_key(&w, "big");
    iot_jw_u64(&w, UINT64_MAX);
    iot_jw_key(&w, "flags");
    iot_jw_arr_begin(&w);
    iot_jw_bool(&w, true);
    iot_jw_bool(&w, false);
    iot_jw_null(&w);
    iot_jw_raw(&w, "{\"x\":1}", 7);
    iot_jw_arr_end(&w);
    iot_jw_obj_end(&w);
    TEST_ASSERT_EQUAL(IOT_OK, iot_jw_finish(&w, &len));
    TEST_ASSERT_EQUAL_STRING("{\"op\":\"list-procedures\",\"payload\":{\"procedures\":[{\"op\":\"get-status\"},{}]},"
                             "\"rescode\":200,\"neg\":-9223372036854775808,\"big\":18446744073709551615,"
                             "\"flags\":[true,false,null,{\"x\":1}]}",
                             buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), len);
}

static void test_writer_escapes_and_round_trips(void)
{
    char buf[128];
    char out[64];
    iot_json_writer_t w;
    const char *nasty = "aspas\" barra\\ \n\t\x01 fim";
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "k\"ey");
    iot_jw_str(&w, nasty);
    iot_jw_obj_end(&w);
    TEST_ASSERT_EQUAL(IOT_OK, iot_jw_finish(&w, NULL));
    TEST_ASSERT_EQUAL_STRING("{\"k\\\"ey\":\"aspas\\\" barra\\\\ \\n\\t\\u0001 fim\"}", buf);

    TEST_ASSERT_EQUAL(IOT_OK, parse(buf));
    TEST_ASSERT_EQUAL(IOT_OK, iot_json_get_str(&doc, iot_json_get(&doc, 0, "k\\\"ey"), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING(nasty, out);
}

static void test_writer_str_n_embeds_nul(void)
{
    char buf[32];
    iot_json_writer_t w;
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    iot_jw_str_n(&w, "a\0b", 3);
    iot_jw_arr_end(&w);
    TEST_ASSERT_EQUAL(IOT_OK, iot_jw_finish(&w, NULL));
    TEST_ASSERT_EQUAL_STRING("[\"a\\u0000b\"]", buf);
}

static void test_writer_overflow_is_sticky(void)
{
    char buf[8];
    iot_json_writer_t w;
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "abcdef"); /* não cabe */
    iot_jw_str(&w, "x");
    iot_jw_obj_end(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_NO_SPACE, iot_jw_finish(&w, NULL));
    TEST_ASSERT_EQUAL_STRING("", buf);

    /* cabe exatamente com o '\0' */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    iot_jw_str(&w, "abc");
    iot_jw_arr_end(&w);
    TEST_ASSERT_EQUAL(IOT_OK, iot_jw_finish(&w, NULL));
    TEST_ASSERT_EQUAL_STRING("[\"abc\"]", buf);

    /* um byte a mais não cabe */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    iot_jw_str(&w, "abcd");
    iot_jw_arr_end(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_NO_SPACE, iot_jw_finish(&w, NULL));
}

static void test_writer_rejects_misuse(void)
{
    char buf[64];
    iot_json_writer_t w;

    /* valor de objeto sem chave */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_i64(&w, 1);
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* chave dentro de array */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    iot_jw_key(&w, "a");
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* chave sem valor */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_key(&w, "a");
    iot_jw_obj_end(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* fechamento trocado */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_arr_end(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* nível aberto no finish */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* duas raízes */
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_obj_begin(&w);
    iot_jw_obj_end(&w);
    iot_jw_obj_begin(&w);
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* documento vazio */
    iot_jw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(IOT_ERR_STATE, iot_jw_finish(&w, NULL));

    /* argumentos nulos */
    iot_jw_init(&w, NULL, 0);
    TEST_ASSERT_EQUAL(IOT_ERR_ARG, iot_jw_finish(&w, NULL));
    iot_jw_init(&w, buf, sizeof(buf));
    iot_jw_arr_begin(&w);
    iot_jw_str(&w, NULL);
    TEST_ASSERT_EQUAL(IOT_ERR_ARG, iot_jw_finish(&w, NULL));
}

static void test_writer_depth_limit(void)
{
    char buf[128];
    iot_json_writer_t w;
    iot_jw_init(&w, buf, sizeof(buf));
    for (size_t i = 0; i < IOT_JSON_MAX_DEPTH; i++) {
        iot_jw_arr_begin(&w);
    }
    for (size_t i = 0; i < IOT_JSON_MAX_DEPTH; i++) {
        iot_jw_arr_end(&w);
    }
    TEST_ASSERT_EQUAL(IOT_OK, iot_jw_finish(&w, NULL));

    iot_jw_init(&w, buf, sizeof(buf));
    for (size_t i = 0; i < IOT_JSON_MAX_DEPTH + 1; i++) {
        iot_jw_arr_begin(&w);
    }
    TEST_ASSERT_EQUAL(IOT_ERR_LIMIT, iot_jw_finish(&w, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_envelope);
    RUN_TEST(test_get_skips_nested_values);
    RUN_TEST(test_iterate_members);
    RUN_TEST(test_raw_returns_subdocument);
    RUN_TEST(test_types);
    RUN_TEST(test_i64_limits);
    RUN_TEST(test_get_str_decodes_escapes);
    RUN_TEST(test_get_str_rejects_lone_surrogate);
    RUN_TEST(test_get_str_no_space);
    RUN_TEST(test_rejects_malformed);
    RUN_TEST(test_accepts_valid_edge_cases);
    RUN_TEST(test_token_limit);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_payload_limit);
    RUN_TEST(test_parse_args);
    RUN_TEST(test_writer_builds_response);
    RUN_TEST(test_writer_escapes_and_round_trips);
    RUN_TEST(test_writer_str_n_embeds_nul);
    RUN_TEST(test_writer_overflow_is_sticky);
    RUN_TEST(test_writer_rejects_misuse);
    RUN_TEST(test_writer_depth_limit);
    return UNITY_END();
}
