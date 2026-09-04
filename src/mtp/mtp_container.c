// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP container and dataset (de)serialisation.

#include <string.h>

#include "nexus/mtp_container.h"

// MTP strings are capped by their u8 length prefix, which counts UTF-16 code
// units including the terminating NUL.
#define MTP_STRING_MAX_UNITS 255

// ---------------------------------------------------------------------------
// Command / response block
// ---------------------------------------------------------------------------

bool mtpContainerParse(const void *buffer, size_t size, MtpContainer *out)
{
    if (buffer == NULL || out == NULL || size < MTP_CONTAINER_HEADER_SIZE) return false;

    const u8 *p = (const u8 *)buffer;
    memset(out, 0, sizeof(*out));

    memcpy(&out->length,         p + 0, 4);
    memcpy(&out->type,           p + 4, 2);
    memcpy(&out->code,           p + 6, 2);
    memcpy(&out->transaction_id, p + 8, 4);

    if (out->length < MTP_CONTAINER_HEADER_SIZE) return false;

    // Trust the smaller of the declared length and what actually arrived, so a
    // truncated or over-long packet cannot walk off the buffer.
    size_t usable = out->length;
    if (usable > size) usable = size;

    size_t param_bytes = usable - MTP_CONTAINER_HEADER_SIZE;
    u32 count = (u32)(param_bytes / 4);
    if (count > MTP_MAX_PARAMS) count = MTP_MAX_PARAMS;

    for (u32 i = 0; i < count; i++) {
        memcpy(&out->params[i], p + MTP_CONTAINER_HEADER_SIZE + (i * 4), 4);
    }
    out->param_count = count;
    return true;
}

size_t mtpContainerSerialize(const MtpContainer *in, void *out, size_t out_size)
{
    if (in == NULL || out == NULL) return 0;

    u32 count = in->param_count;
    if (count > MTP_MAX_PARAMS) count = MTP_MAX_PARAMS;

    const size_t total = MTP_CONTAINER_HEADER_SIZE + (size_t)count * 4;
    if (out_size < total) return 0;

    u8 *p = (u8 *)out;
    const u32 length = (u32)total;

    memcpy(p + 0, &length,             4);
    memcpy(p + 4, &in->type,           2);
    memcpy(p + 6, &in->code,           2);
    memcpy(p + 8, &in->transaction_id, 4);

    for (u32 i = 0; i < count; i++) {
        memcpy(p + MTP_CONTAINER_HEADER_SIZE + (i * 4), &in->params[i], 4);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void mtpWriterInit(MtpWriter *w, void *buffer, size_t capacity)
{
    w->buf      = (u8 *)buffer;
    w->cap      = capacity;
    w->len      = 0;
    w->overflow = false;
}

bool mtpWriterOk(const MtpWriter *w)       { return !w->overflow; }
size_t mtpWriterLength(const MtpWriter *w) { return w->len; }

// Reserves n bytes and returns a pointer to them, or NULL once the writer has
// overflowed. The overflow flag latches so callers only need one final check.
static u8 *writer_claim(MtpWriter *w, size_t n)
{
    if (w->overflow) return NULL;
    if (n > w->cap - w->len) {
        w->overflow = true;
        return NULL;
    }
    u8 *p = w->buf + w->len;
    w->len += n;
    return p;
}

void mtpWriteU8(MtpWriter *w, u8 v)
{
    u8 *p = writer_claim(w, 1);
    if (p) *p = v;
}

void mtpWriteU16(MtpWriter *w, u16 v)
{
    u8 *p = writer_claim(w, 2);
    if (p) memcpy(p, &v, 2);
}

void mtpWriteU32(MtpWriter *w, u32 v)
{
    u8 *p = writer_claim(w, 4);
    if (p) memcpy(p, &v, 4);
}

void mtpWriteU64(MtpWriter *w, u64 v)
{
    u8 *p = writer_claim(w, 8);
    if (p) memcpy(p, &v, 8);
}

void mtpWriteU128(MtpWriter *w, u64 lo, u64 hi)
{
    mtpWriteU64(w, lo);
    mtpWriteU64(w, hi);
}

void mtpWriteBytes(MtpWriter *w, const void *data, size_t size)
{
    u8 *p = writer_claim(w, size);
    if (p) memcpy(p, data, size);
}

void mtpWriteString(MtpWriter *w, const char *utf8)
{
    if (utf8 == NULL || utf8[0] == '\0') {
        // The empty string is encoded as a zero count with no character data.
        mtpWriteU8(w, 0);
        return;
    }

    u16 utf16[MTP_STRING_MAX_UNITS];
    ssize_t units = utf8_to_utf16(utf16, (const u8 *)utf8, MTP_STRING_MAX_UNITS - 1);

    if (units < 0) {
        // The input was not valid UTF-8. Emitting an empty string here is what
        // makes a file manager show an unnamed folder, which is far more
        // confusing than a name with a few characters replaced -- so fall back
        // to an ASCII transliteration instead of giving up.
        units = 0;
        for (const char *p = utf8; *p != '\0' && units < MTP_STRING_MAX_UNITS - 1; p++) {
            const unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7F) utf16[units++] = c;
            else if (c >= 0x80)        utf16[units++] = '?';   // one per byte
        }
        if (units == 0) { mtpWriteU8(w, 0); return; }
    }
    if (units > MTP_STRING_MAX_UNITS - 1) units = MTP_STRING_MAX_UNITS - 1;

    // The count includes the terminating NUL.
    mtpWriteU8(w, (u8)(units + 1));
    for (ssize_t i = 0; i < units; i++) mtpWriteU16(w, utf16[i]);
    mtpWriteU16(w, 0);
}

void mtpWriteU32Array(MtpWriter *w, const u32 *values, u32 count)
{
    mtpWriteU32(w, count);
    for (u32 i = 0; i < count; i++) mtpWriteU32(w, values[i]);
}

void mtpWriteU16Array(MtpWriter *w, const u16 *values, u32 count)
{
    mtpWriteU32(w, count);
    for (u32 i = 0; i < count; i++) mtpWriteU16(w, values[i]);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

void mtpReaderInit(MtpReader *r, const void *buffer, size_t size)
{
    r->buf       = (const u8 *)buffer;
    r->len       = size;
    r->pos       = 0;
    r->underflow = false;
}

bool mtpReaderOk(const MtpReader *r) { return !r->underflow; }

size_t mtpReaderRemaining(const MtpReader *r)
{
    return (r->pos < r->len) ? (r->len - r->pos) : 0;
}

// Consumes n bytes and returns a pointer to them, or NULL past the end. Like
// the writer, the underflow flag latches.
static const u8 *reader_take(MtpReader *r, size_t n)
{
    if (r->underflow) return NULL;
    if (n > r->len - r->pos) {
        r->underflow = true;
        return NULL;
    }
    const u8 *p = r->buf + r->pos;
    r->pos += n;
    return p;
}

u8 mtpReadU8(MtpReader *r)
{
    const u8 *p = reader_take(r, 1);
    return p ? *p : 0;
}

u16 mtpReadU16(MtpReader *r)
{
    const u8 *p = reader_take(r, 2);
    u16 v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

u32 mtpReadU32(MtpReader *r)
{
    const u8 *p = reader_take(r, 4);
    u32 v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

u64 mtpReadU64(MtpReader *r)
{
    const u8 *p = reader_take(r, 8);
    u64 v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}

void mtpReadSkip(MtpReader *r, size_t count)
{
    reader_take(r, count);
}

void mtpReadString(MtpReader *r, char *out, size_t out_size)
{
    if (out_size > 0) out[0] = '\0';

    u8 units = mtpReadU8(r);
    if (units == 0) return;

    // Copy into a NUL-terminated staging buffer; utf16_to_utf8 requires one.
    u16 utf16[MTP_STRING_MAX_UNITS + 1];
    for (u8 i = 0; i < units; i++) utf16[i] = mtpReadU16(r);
    utf16[units] = 0;

    if (!mtpReaderOk(r) || out_size == 0) return;

    // The declared count includes a NUL, but do not trust the host to have
    // sent one -- the explicit terminator above guarantees it.
    ssize_t written = utf16_to_utf8((u8 *)out, utf16, out_size - 1);
    if (written < 0) {
        out[0] = '\0';
        return;
    }
    if ((size_t)written >= out_size) written = (ssize_t)out_size - 1;
    out[written] = '\0';
}
