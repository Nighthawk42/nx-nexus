// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- minimal read-only JSON parser.
//
// Source indexes are JSON, and they arrive over the network from wherever the
// user pointed the tool. That makes this parser an attack surface, so it is
// written the same way as the container parsers: no allocation, no recursion
// on untrusted depth, every read bounds-checked, and unit-tested on the host
// under sanitizers.
//
// It parses into a flat array of nodes that borrow the source text, so the
// document buffer must outlive the parse.
#pragma once

#include "nexus/format.h"

#define JSON_MAX_NODES  4096
#define JSON_MAX_DEPTH  32

typedef enum {
    JsonType_Null = 0,
    JsonType_Bool,
    JsonType_Number,
    JsonType_String,
    JsonType_Array,
    JsonType_Object,
} JsonType;

typedef struct {
    u8  type;
    u32 start;        // offset into the source text
    u32 len;          // raw length at that offset (strings exclude the quotes)
    u32 first_child;  // node index, 0 when none
    u32 next_sibling; // node index, 0 when none
    u32 child_count;
} JsonNode;

typedef struct {
    const char *text;
    size_t      text_len;

    JsonNode nodes[JSON_MAX_NODES];
    u32      node_count;   // node 0 is a reserved sentinel, so the root is 1
} JsonDoc;

/// Parses a whole document. The text is borrowed, not copied.
NexusFmtResult jsonParse(JsonDoc *doc, const char *text, size_t len);

/// Root node index, or 0 for an empty document.
u32 jsonRoot(const JsonDoc *doc);

JsonType jsonTypeOf(const JsonDoc *doc, u32 node);

/// Looks up a key on an object. Returns 0 when absent or not an object.
u32 jsonObjectGet(const JsonDoc *doc, u32 object, const char *key);

/// Number of elements in an array (or members of an object).
u32 jsonCount(const JsonDoc *doc, u32 node);

/// Element by index. Returns 0 when out of range.
u32 jsonAt(const JsonDoc *doc, u32 node, u32 index);

/// Copies a string node into out, decoding \\" \\\\ \\/ \\n \\r \\t \\b \\f and
/// \\uXXXX (as UTF-8; surrogate pairs are combined). Always NUL terminates.
/// Returns false for a non-string node or when out is too small.
bool jsonGetString(const JsonDoc *doc, u32 node, char *out, size_t out_size);

/// Reads a number node as an unsigned integer. Fractions are truncated.
bool jsonGetU64(const JsonDoc *doc, u32 node, u64 *out);

bool jsonGetBool(const JsonDoc *doc, u32 node, bool *out);
