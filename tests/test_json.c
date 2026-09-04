// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- JSON parser tests.
//
// This parser reads documents fetched over the network from a user-supplied
// URL, so the malformed-input cases matter as much as the happy path.

#include <string.h>

#include "nexus_test.h"
#include "nexus/json.h"

static JsonDoc g_doc;

static void t_scalars(void)
{
    static const char *src = "{\"a\":1,\"b\":\"hi\",\"c\":true,\"d\":false,\"e\":null}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root = jsonRoot(&g_doc);
    CHECK_U64(jsonTypeOf(&g_doc, root), JsonType_Object);
    CHECK_U64(jsonCount(&g_doc, root), 5);

    u64 n = 0;
    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "a"), &n), "a should be a number");
    CHECK_U64(n, 1);

    char str[32];
    CHECK(jsonGetString(&g_doc, jsonObjectGet(&g_doc, root, "b"), str, sizeof(str)),
          "b should be a string");
    CHECK_STR(str, "hi");

    bool b = false;
    CHECK(jsonGetBool(&g_doc, jsonObjectGet(&g_doc, root, "c"), &b), "c should be a bool");
    CHECK(b, "c should be true");
    CHECK(jsonGetBool(&g_doc, jsonObjectGet(&g_doc, root, "d"), &b), "d should be a bool");
    CHECK(!b, "d should be false");

    CHECK_U64(jsonTypeOf(&g_doc, jsonObjectGet(&g_doc, root, "e")), JsonType_Null);

    // A key that is not there must read as absent, not as node 0 being valid.
    CHECK_U64(jsonObjectGet(&g_doc, root, "missing"), 0);
}

static void t_shop_index_shape(void)
{
    // The shape a self-hosted source index actually has.
    static const char *src =
        "{\n"
        "  \"files\": [\n"
        "    {\"url\": \"https://example.invalid/a.nsp\", \"size\": 1234567890},\n"
        "    {\"url\": \"https://example.invalid/b.nsp\", \"size\": 42}\n"
        "  ],\n"
        "  \"success\": \"welcome\"\n"
        "}";

    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root  = jsonRoot(&g_doc);
    const u32 files = jsonObjectGet(&g_doc, root, "files");
    CHECK_U64(jsonTypeOf(&g_doc, files), JsonType_Array);
    CHECK_U64(jsonCount(&g_doc, files), 2);

    const u32 first = jsonAt(&g_doc, files, 0);
    char url[128];
    CHECK(jsonGetString(&g_doc, jsonObjectGet(&g_doc, first, "url"), url, sizeof(url)),
          "url should read");
    CHECK_STR(url, "https://example.invalid/a.nsp");

    u64 size = 0;
    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, first, "size"), &size), "size should read");
    CHECK_U64(size, 1234567890ull);

    const u32 second = jsonAt(&g_doc, files, 1);
    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, second, "size"), &size), "size 2");
    CHECK_U64(size, 42);

    CHECK_U64(jsonAt(&g_doc, files, 2), 0);   // past the end
}

static void t_escapes(void)
{
    static const char *src =
        "{\"s\":\"quote:\\\" back:\\\\ slash:\\/ nl:\\n tab:\\t u:\\u0041 utf:\\u00e9\"}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    char out[128];
    CHECK(jsonGetString(&g_doc, jsonObjectGet(&g_doc, jsonRoot(&g_doc), "s"),
                        out, sizeof(out)), "escaped string should decode");
    CHECK_STR(out, "quote:\" back:\\ slash:/ nl:\n tab:\t u:A utf:\xc3\xa9");
}

static void t_surrogate_pair(void)
{
    // U+1F600, which only encodes as a surrogate pair in JSON.
    static const char *src = "{\"s\":\"\\ud83d\\ude00\"}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    char out[16];
    CHECK(jsonGetString(&g_doc, jsonObjectGet(&g_doc, jsonRoot(&g_doc), "s"),
                        out, sizeof(out)), "surrogate pair should decode");
    CHECK_STR(out, "\xf0\x9f\x98\x80");
}

static void t_escaped_quote_does_not_terminate(void)
{
    // The classic parser bug: a backslash-escaped quote ending the string early
    // would make everything after it parse as garbage.
    static const char *src = "{\"a\":\"x\\\"y\",\"b\":7}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root = jsonRoot(&g_doc);
    CHECK_U64(jsonCount(&g_doc, root), 2);

    u64 n = 0;
    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "b"), &n), "b must still parse");
    CHECK_U64(n, 7);
}

static void t_nested(void)
{
    static const char *src = "{\"a\":{\"b\":{\"c\":[1,2,[3,{\"d\":4}]]}}}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 a = jsonObjectGet(&g_doc, jsonRoot(&g_doc), "a");
    const u32 b = jsonObjectGet(&g_doc, a, "b");
    const u32 c = jsonObjectGet(&g_doc, b, "c");
    CHECK_U64(jsonTypeOf(&g_doc, c), JsonType_Array);
    CHECK_U64(jsonCount(&g_doc, c), 3);

    const u32 inner = jsonAt(&g_doc, c, 2);
    CHECK_U64(jsonCount(&g_doc, inner), 2);

    u64 d = 0;
    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, jsonAt(&g_doc, inner, 1), "d"), &d), "d");
    CHECK_U64(d, 4);
}

static void t_empty_containers(void)
{
    static const char *src = "{\"a\":[],\"b\":{}}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root = jsonRoot(&g_doc);
    CHECK_U64(jsonCount(&g_doc, jsonObjectGet(&g_doc, root, "a")), 0);
    CHECK_U64(jsonCount(&g_doc, jsonObjectGet(&g_doc, root, "b")), 0);
}

static void t_malformed(void)
{
    const char *bad[] = {
        "",                       // empty
        "{",                      // unterminated object
        "[",                      // unterminated array
        "{\"a\":}",               // missing value
        "{\"a\" 1}",              // missing colon
        "{\"a\":1,}",             // trailing comma
        "{\"a\":\"unterminated}", // unterminated string
        "{}{}",                   // trailing content
        "{\"a\":1} junk",         // trailing junk
        "nope",                   // bare word
        "[1,2",                   // unterminated
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const NexusFmtResult r = jsonParse(&g_doc, bad[i], strlen(bad[i]));
        CHECK(r != NexusFmt_Ok, "case %zu (\"%s\") should have been rejected", i, bad[i]);
    }
}

static void t_depth_limit(void)
{
    // Deep nesting must be refused rather than blowing the stack.
    static char deep[JSON_MAX_DEPTH * 4 + 64];
    size_t w = 0;
    for (size_t i = 0; i < JSON_MAX_DEPTH + 8; i++) deep[w++] = '[';
    for (size_t i = 0; i < JSON_MAX_DEPTH + 8; i++) deep[w++] = ']';
    deep[w] = '\0';

    CHECK(jsonParse(&g_doc, deep, w) != NexusFmt_Ok, "over-deep nesting must be rejected");
}

static void t_number_edges(void)
{
    static const char *src = "{\"big\":18446744073709551615,\"neg\":-5,\"frac\":3.9}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root = jsonRoot(&g_doc);
    u64 v = 0;

    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "big"), &v), "u64 max should read");
    CHECK_U64(v, 18446744073709551615ull);

    // Negative sizes are meaningless for this use and must be refused rather
    // than wrapping into a huge positive.
    CHECK(!jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "neg"), &v),
          "negative should be rejected");

    CHECK(jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "frac"), &v), "fraction truncates");
    CHECK_U64(v, 3);
}

static void t_string_buffer_too_small(void)
{
    static const char *src = "{\"s\":\"abcdefghij\"}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    char small[4];
    CHECK(!jsonGetString(&g_doc, jsonObjectGet(&g_doc, jsonRoot(&g_doc), "s"),
                         small, sizeof(small)),
          "a too-small buffer must fail, not truncate silently");
}

static void t_wrong_type_accessors(void)
{
    static const char *src = "{\"s\":\"text\",\"n\":5}";
    CHECK_FMT(jsonParse(&g_doc, src, strlen(src)), NexusFmt_Ok);

    const u32 root = jsonRoot(&g_doc);
    u64 v = 0;
    char buf[16];
    bool b = false;

    CHECK(!jsonGetU64(&g_doc, jsonObjectGet(&g_doc, root, "s"), &v), "string is not a number");
    CHECK(!jsonGetString(&g_doc, jsonObjectGet(&g_doc, root, "n"), buf, sizeof(buf)),
          "number is not a string");
    CHECK(!jsonGetBool(&g_doc, jsonObjectGet(&g_doc, root, "n"), &b), "number is not a bool");

    // Accessors on node 0 must be safe.
    CHECK(!jsonGetU64(&g_doc, 0, &v), "node 0 is not a value");
    CHECK_U64(jsonCount(&g_doc, 0), 0);
    CHECK_U64(jsonAt(&g_doc, 0, 0), 0);
}

void test_json(void)
{
    nexusTestRun("scalars",                  t_scalars);
    nexusTestRun("shop index shape",         t_shop_index_shape);
    nexusTestRun("escape decoding",          t_escapes);
    nexusTestRun("surrogate pair",           t_surrogate_pair);
    nexusTestRun("escaped quote in string",  t_escaped_quote_does_not_terminate);
    nexusTestRun("nested containers",        t_nested);
    nexusTestRun("empty containers",         t_empty_containers);
    nexusTestRun("malformed documents",      t_malformed);
    nexusTestRun("depth limit",              t_depth_limit);
    nexusTestRun("number edges",             t_number_edges);
    nexusTestRun("string buffer too small",  t_string_buffer_too_small);
    nexusTestRun("wrong-type accessors",     t_wrong_type_accessors);
}
