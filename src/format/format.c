// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- shared helpers for the container parsers.

#include <string.h>

#include "nexus/format.h"

const char *nexusFmtStr(NexusFmtResult r)
{
    switch (r) {
        case NexusFmt_Ok:           return "ok";
        case NexusFmt_BadMagic:     return "bad magic";
        case NexusFmt_Truncated:    return "truncated";
        case NexusFmt_TooLarge:     return "too large";
        case NexusFmt_Overflow:     return "size overflow";
        case NexusFmt_BadNameTable: return "bad name table";
        case NexusFmt_OutOfRange:   return "index out of range";
        case NexusFmt_Unsupported:  return "unsupported";
        case NexusFmt_NotFound:     return "not found";
        default:                    return "unknown";
    }
}

u8 nexusRdU8(const void *base, size_t off)
{
    return ((const u8 *)base)[off];
}

u16 nexusRdU16(const void *base, size_t off)
{
    u16 v;
    memcpy(&v, (const u8 *)base + off, sizeof(v));
    return v;
}

u32 nexusRdU32(const void *base, size_t off)
{
    u32 v;
    memcpy(&v, (const u8 *)base + off, sizeof(v));
    return v;
}

u64 nexusRdU64(const void *base, size_t off)
{
    u64 v;
    memcpy(&v, (const u8 *)base + off, sizeof(v));
    return v;
}

u64 nexusRdU40(const void *base, size_t off)
{
    const u8 *p = (const u8 *)base + off;
    return  (u64)p[0]
         | ((u64)p[1] <<  8)
         | ((u64)p[2] << 16)
         | ((u64)p[3] << 24)
         | ((u64)p[4] << 32);
}

bool nexusAddOverflows(u64 a, u64 b, u64 *out)
{
    if (a > UINT64_MAX - b) return true;
    *out = a + b;
    return false;
}

bool nexusMulOverflows(u64 a, u64 b, u64 *out)
{
    if (a != 0 && b > UINT64_MAX / a) return true;
    *out = a * b;
    return false;
}

size_t nexusUtf8SeqLen(unsigned char c)
{
    if (c < 0x80)          return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;   // continuation byte, or not valid UTF-8 at all
}

void nexusSanitiseUtf8(const char *in, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (in == NULL) return;

    size_t w = 0;
    size_t i = 0;

    while (in[i] != '\0') {
        const unsigned char lead = (unsigned char)in[i];
        const size_t seq = nexusUtf8SeqLen(lead);

        // A stray continuation byte means the input is not valid UTF-8; drop it
        // rather than pass it through and poison the whole string.
        if (seq == 0) { i++; continue; }

        // Every continuation byte of the sequence must actually be present and
        // well formed, or the input is truncated or corrupt.
        bool valid = true;
        for (size_t k = 1; k < seq; k++) {
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) break;   // nothing sensible left to copy

        if (seq == 1) {
            const char c = in[i];
            // Control characters and the characters Windows forbids in names.
            if ((unsigned char)c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL) {
                i++;
                continue;
            }
        }

        // Stop before writing a partial character.
        if (w + seq + 1 > out_size) break;

        for (size_t k = 0; k < seq; k++) out[w++] = in[i + k];
        i += seq;
    }

    // Windows will not accept a name ending in a space or a dot. Only ASCII
    // can be trimmed this way, which is fine -- both are single-byte.
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '.')) w--;

    out[w] = '\0';
}
