// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- minimal read-only JSON parser.

#include <string.h>

#include "nexus/json.h"

// Parser state. Kept in one struct so the helpers stay small and the whole
// thing remains iterative -- untrusted input must not be able to drive
// unbounded recursion.
typedef struct {
    JsonDoc *doc;
    size_t   pos;
    bool     failed;
} JsonParser;

static bool at_end(const JsonParser *p) { return p->pos >= p->doc->text_len; }
static char peek(const JsonParser *p)   { return at_end(p) ? '\0' : p->doc->text[p->pos]; }

static void skip_ws(JsonParser *p)
{
    while (!at_end(p)) {
        const char c = p->doc->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

// Allocates a node. Index 0 is reserved as "no node", so the first real node
// is 1 and callers can treat 0 as absent.
static u32 node_alloc(JsonParser *p, JsonType type)
{
    if (p->doc->node_count >= JSON_MAX_NODES) {
        p->failed = true;
        return 0;
    }

    const u32 idx = p->doc->node_count++;
    JsonNode *n = &p->doc->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->type = (u8)type;
    return idx;
}

// Consumes a quoted string, leaving pos just past the closing quote. The
// returned span excludes the quotes and keeps escapes raw; decoding happens in
// jsonGetString.
static bool scan_string(JsonParser *p, u32 *out_start, u32 *out_len)
{
    if (peek(p) != '"') return false;
    p->pos++;

    const size_t start = p->pos;
    while (!at_end(p)) {
        const char c = p->doc->text[p->pos];

        if (c == '\\') {
            // Skip the escape and whatever it introduces, so an escaped quote
            // does not terminate the string early.
            p->pos += 2;
            if (p->pos > p->doc->text_len) return false;
            continue;
        }
        if (c == '"') {
            *out_start = (u32)start;
            *out_len   = (u32)(p->pos - start);
            p->pos++;
            return true;
        }
        p->pos++;
    }
    return false;   // unterminated
}

static bool is_number_char(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+'
        || c == '.' || c == 'e' || c == 'E';
}

static bool lit_matches(JsonParser *p, const char *lit)
{
    const size_t n = strlen(lit);
    if (p->doc->text_len - p->pos < n) return false;
    return memcmp(p->doc->text + p->pos, lit, n) == 0;
}

// Parses one value, iteratively for containers. depth guards nesting.
static u32 parse_value(JsonParser *p, u32 depth);

// Appends child to parent's sibling chain.
static void attach(JsonDoc *doc, u32 parent, u32 child, u32 *last)
{
    if (child == 0) return;

    if (*last == 0) doc->nodes[parent].first_child = child;
    else            doc->nodes[*last].next_sibling = child;

    *last = child;
    doc->nodes[parent].child_count++;
}

static u32 parse_object(JsonParser *p, u32 depth)
{
    const u32 me = node_alloc(p, JsonType_Object);
    if (me == 0 || p->failed) return 0;

    p->pos++;   // consume '{'
    skip_ws(p);

    if (peek(p) == '}') { p->pos++; return me; }

    u32 last = 0;
    for (;;) {
        skip_ws(p);

        // A member is "key": value. The key is stored as a String node whose
        // first child is the value, which keeps the node shape uniform.
        u32 kstart = 0, klen = 0;
        if (!scan_string(p, &kstart, &klen)) { p->failed = true; return 0; }

        const u32 key = node_alloc(p, JsonType_String);
        if (key == 0 || p->failed) return 0;
        p->doc->nodes[key].start = kstart;
        p->doc->nodes[key].len   = klen;

        skip_ws(p);
        if (peek(p) != ':') { p->failed = true; return 0; }
        p->pos++;

        const u32 value = parse_value(p, depth + 1);
        if (p->failed) return 0;

        p->doc->nodes[key].first_child = value;
        attach(p->doc, me, key, &last);

        skip_ws(p);
        const char c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == '}') { p->pos++; return me; }

        p->failed = true;
        return 0;
    }
}

static u32 parse_array(JsonParser *p, u32 depth)
{
    const u32 me = node_alloc(p, JsonType_Array);
    if (me == 0 || p->failed) return 0;

    p->pos++;   // consume '['
    skip_ws(p);

    if (peek(p) == ']') { p->pos++; return me; }

    u32 last = 0;
    for (;;) {
        const u32 value = parse_value(p, depth + 1);
        if (p->failed) return 0;
        attach(p->doc, me, value, &last);

        skip_ws(p);
        const char c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == ']') { p->pos++; return me; }

        p->failed = true;
        return 0;
    }
}

static u32 parse_value(JsonParser *p, u32 depth)
{
    if (depth > JSON_MAX_DEPTH) { p->failed = true; return 0; }

    skip_ws(p);
    if (at_end(p)) { p->failed = true; return 0; }

    const char c = peek(p);

    if (c == '{') return parse_object(p, depth);
    if (c == '[') return parse_array(p, depth);

    if (c == '"') {
        u32 start = 0, len = 0;
        if (!scan_string(p, &start, &len)) { p->failed = true; return 0; }

        const u32 me = node_alloc(p, JsonType_String);
        if (me == 0) return 0;
        p->doc->nodes[me].start = start;
        p->doc->nodes[me].len   = len;
        return me;
    }

    if (lit_matches(p, "true") || lit_matches(p, "false")) {
        const bool is_true = (c == 't');
        const u32 me = node_alloc(p, JsonType_Bool);
        if (me == 0) return 0;
        p->doc->nodes[me].start = (u32)p->pos;
        p->doc->nodes[me].len   = is_true ? 4 : 5;
        p->pos += is_true ? 4 : 5;
        return me;
    }

    if (lit_matches(p, "null")) {
        const u32 me = node_alloc(p, JsonType_Null);
        if (me == 0) return 0;
        p->pos += 4;
        return me;
    }

    if (is_number_char(c)) {
        const size_t start = p->pos;
        while (!at_end(p) && is_number_char(p->doc->text[p->pos])) p->pos++;

        const u32 me = node_alloc(p, JsonType_Number);
        if (me == 0) return 0;
        p->doc->nodes[me].start = (u32)start;
        p->doc->nodes[me].len   = (u32)(p->pos - start);
        return me;
    }

    p->failed = true;
    return 0;
}

NexusFmtResult jsonParse(JsonDoc *doc, const char *text, size_t len)
{
    if (doc == NULL || text == NULL) return NexusFmt_Truncated;

    memset(doc, 0, sizeof(*doc));
    doc->text     = text;
    doc->text_len = len;

    // Burn index 0 so it can mean "no node".
    doc->node_count = 1;
    memset(&doc->nodes[0], 0, sizeof(doc->nodes[0]));

    JsonParser p = { .doc = doc, .pos = 0, .failed = false };

    const u32 root = parse_value(&p, 0);
    if (p.failed || root == 0) {
        doc->node_count = 1;
        return NexusFmt_BadMagic;   // malformed document
    }

    // Trailing content after the top-level value means the document is not
    // what it claims to be; better to reject than half-accept.
    skip_ws(&p);
    if (!at_end(&p)) {
        doc->node_count = 1;
        return NexusFmt_BadMagic;
    }

    return NexusFmt_Ok;
}

u32 jsonRoot(const JsonDoc *doc)
{
    return (doc != NULL && doc->node_count > 1) ? 1 : 0;
}

JsonType jsonTypeOf(const JsonDoc *doc, u32 node)
{
    if (doc == NULL || node == 0 || node >= doc->node_count) return JsonType_Null;
    return (JsonType)doc->nodes[node].type;
}

// Compares a raw (still-escaped) key span against a plain string. Keys with
// escapes are rare enough that a decode-then-compare is not worth it; an
// escaped key simply will not match, which is safe.
static bool key_equals(const JsonDoc *doc, u32 key_node, const char *key)
{
    const JsonNode *n = &doc->nodes[key_node];
    const size_t klen = strlen(key);
    if (n->len != klen) return false;
    return memcmp(doc->text + n->start, key, klen) == 0;
}

u32 jsonObjectGet(const JsonDoc *doc, u32 object, const char *key)
{
    if (doc == NULL || key == NULL) return 0;
    if (jsonTypeOf(doc, object) != JsonType_Object) return 0;

    for (u32 k = doc->nodes[object].first_child; k != 0; k = doc->nodes[k].next_sibling) {
        if (key_equals(doc, k, key)) return doc->nodes[k].first_child;
    }
    return 0;
}

u32 jsonCount(const JsonDoc *doc, u32 node)
{
    if (doc == NULL || node == 0 || node >= doc->node_count) return 0;
    return doc->nodes[node].child_count;
}

u32 jsonAt(const JsonDoc *doc, u32 node, u32 index)
{
    if (doc == NULL || node == 0 || node >= doc->node_count) return 0;

    u32 child = doc->nodes[node].first_child;
    for (u32 i = 0; child != 0 && i < index; i++) child = doc->nodes[child].next_sibling;
    return child;
}

// Appends one code point as UTF-8. Returns false when it will not fit.
static bool append_utf8(char *out, size_t out_size, size_t *w, u32 cp)
{
    if (cp < 0x80) {
        if (*w + 1 >= out_size) return false;
        out[(*w)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*w + 2 >= out_size) return false;
        out[(*w)++] = (char)(0xC0 | (cp >> 6));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*w + 3 >= out_size) return false;
        out[(*w)++] = (char)(0xE0 | (cp >> 12));
        out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*w + 4 >= out_size) return false;
        out[(*w)++] = (char)(0xF0 | (cp >> 18));
        out[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    }
    return true;
}

static bool hex4(const char *s, u32 *out)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = s[i];
        u32 d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

bool jsonGetString(const JsonDoc *doc, u32 node, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return false;
    out[0] = '\0';

    if (jsonTypeOf(doc, node) != JsonType_String) return false;

    const JsonNode *n = &doc->nodes[node];
    const char *src = doc->text + n->start;
    const size_t len = n->len;

    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '\\') {
            if (w + 1 >= out_size) return false;
            out[w++] = src[i];
            continue;
        }

        if (i + 1 >= len) return false;
        const char e = src[++i];

        switch (e) {
            case '"':  if (w + 1 >= out_size) return false; out[w++] = '"';  break;
            case '\\': if (w + 1 >= out_size) return false; out[w++] = '\\'; break;
            case '/':  if (w + 1 >= out_size) return false; out[w++] = '/';  break;
            case 'b':  if (w + 1 >= out_size) return false; out[w++] = '\b'; break;
            case 'f':  if (w + 1 >= out_size) return false; out[w++] = '\f'; break;
            case 'n':  if (w + 1 >= out_size) return false; out[w++] = '\n'; break;
            case 'r':  if (w + 1 >= out_size) return false; out[w++] = '\r'; break;
            case 't':  if (w + 1 >= out_size) return false; out[w++] = '\t'; break;

            case 'u': {
                if (i + 4 >= len) return false;
                u32 cp = 0;
                if (!hex4(src + i + 1, &cp)) return false;
                i += 4;

                // Combine a surrogate pair into one code point.
                if (cp >= 0xD800 && cp <= 0xDBFF
                    && i + 6 < len && src[i + 1] == '\\' && src[i + 2] == 'u') {
                    u32 lo = 0;
                    if (hex4(src + i + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }

                if (!append_utf8(out, out_size, &w, cp)) return false;
                break;
            }

            default:
                return false;   // unknown escape
        }
    }

    out[w] = '\0';
    return true;
}

bool jsonGetU64(const JsonDoc *doc, u32 node, u64 *out)
{
    if (out == NULL) return false;
    if (jsonTypeOf(doc, node) != JsonType_Number) return false;

    const JsonNode *n = &doc->nodes[node];
    const char *src = doc->text + n->start;

    u64 v = 0;
    size_t i = 0;

    if (i < n->len && src[i] == '-') return false;   // negative sizes are nonsense
    if (i < n->len && src[i] == '+') i++;

    bool any = false;
    for (; i < n->len; i++) {
        const char c = src[i];
        if (c == '.' || c == 'e' || c == 'E') break;   // truncate at the fraction
        if (c < '0' || c > '9') return false;

        // Saturate rather than wrap on an absurd value.
        if (v > (UINT64_MAX - (u64)(c - '0')) / 10) return false;
        v = (v * 10) + (u64)(c - '0');
        any = true;
    }

    if (!any) return false;
    *out = v;
    return true;
}

bool jsonGetBool(const JsonDoc *doc, u32 node, bool *out)
{
    if (out == NULL) return false;
    if (jsonTypeOf(doc, node) != JsonType_Bool) return false;

    *out = (doc->nodes[node].len == 4);   // "true" is 4, "false" is 5
    return true;
}
