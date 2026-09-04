// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP container (packet) build/parse helpers.
//
// MTP is little-endian on the wire, which matches AArch64, so scalars are
// copied rather than byte-swapped. Strings are length-prefixed UTF-16LE:
// one u8 character count (including the NUL) followed by that many u16 units.
// An empty string is a single 0x00 byte with no characters.
#pragma once

#include <switch.h>
#include <stddef.h>
#include "nexus/mtp_types.h"

// ---------------------------------------------------------------------------
// Command / response block
// ---------------------------------------------------------------------------
typedef struct {
    u32 length;                    // total bytes including this header
    u16 type;                      // MtpContainerType_*
    u16 code;                      // operation, response or event code
    u32 transaction_id;
    u32 params[MTP_MAX_PARAMS];
    u32 param_count;               // parsed from length; 0..MTP_MAX_PARAMS
} MtpContainer;

/// Parses a command/response block out of a received buffer.
/// Returns false when the buffer is too small or the declared length is bogus.
bool mtpContainerParse(const void *buffer, size_t size, MtpContainer *out);

/// Serialises a command/response block. Writes
/// MTP_CONTAINER_HEADER_SIZE + 4*param_count bytes.
/// Returns the number of bytes written, or 0 if out_size is insufficient.
size_t mtpContainerSerialize(const MtpContainer *in, void *out, size_t out_size);

// ---------------------------------------------------------------------------
// Dataset writer -- appends primitives to a growing payload buffer.
// Every append is bounds-checked; once an overflow occurs the writer latches
// into a failed state and mtpWriterOk() returns false, so callers can build a
// whole dataset and check once at the end.
// ---------------------------------------------------------------------------
typedef struct {
    u8    *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} MtpWriter;

void mtpWriterInit(MtpWriter *w, void *buffer, size_t capacity);
bool mtpWriterOk(const MtpWriter *w);
size_t mtpWriterLength(const MtpWriter *w);

void mtpWriteU8(MtpWriter *w, u8 v);
void mtpWriteU16(MtpWriter *w, u16 v);
void mtpWriteU32(MtpWriter *w, u32 v);
void mtpWriteU64(MtpWriter *w, u64 v);
void mtpWriteU128(MtpWriter *w, u64 lo, u64 hi);
void mtpWriteBytes(MtpWriter *w, const void *data, size_t size);

/// Writes a length-prefixed UTF-16LE string from a UTF-8 source.
/// Passing NULL or "" writes the single-byte empty-string form.
void mtpWriteString(MtpWriter *w, const char *utf8);

/// Writes a u32 array as an MTP AUINT32 (u32 count followed by elements).
void mtpWriteU32Array(MtpWriter *w, const u32 *values, u32 count);

/// Writes a u16 array as an MTP AUINT16.
void mtpWriteU16Array(MtpWriter *w, const u16 *values, u32 count);

// ---------------------------------------------------------------------------
// Dataset reader -- for parsing host-supplied datasets (SendObjectInfo).
// Reads past the end latch into a failed state; check mtpReaderOk() at the end.
// ---------------------------------------------------------------------------
typedef struct {
    const u8 *buf;
    size_t    len;
    size_t    pos;
    bool      underflow;
} MtpReader;

void mtpReaderInit(MtpReader *r, const void *buffer, size_t size);
bool mtpReaderOk(const MtpReader *r);
size_t mtpReaderRemaining(const MtpReader *r);

u8  mtpReadU8(MtpReader *r);
u16 mtpReadU16(MtpReader *r);
u32 mtpReadU32(MtpReader *r);
u64 mtpReadU64(MtpReader *r);
void mtpReadSkip(MtpReader *r, size_t count);

/// Reads a length-prefixed UTF-16LE string into a UTF-8 buffer.
/// out is always NUL terminated when out_size > 0. Characters that do not fit
/// are discarded, but the reader still advances over the whole field.
void mtpReadString(MtpReader *r, char *out, size_t out_size);
