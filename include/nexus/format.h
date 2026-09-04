// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- shared result type for the container parsers.
//
// These parsers consume data that arrives over USB from an untrusted host, so
// every one of them is written to be total: any input, however malformed,
// produces an error code rather than an out-of-bounds read. The error codes
// are deliberately specific so unit tests can assert on the failure mode, not
// just that something failed.
#pragma once

#include "nexus/nx_types.h"

typedef enum {
    NexusFmt_Ok = 0,
    NexusFmt_BadMagic,        // container magic did not match
    NexusFmt_Truncated,       // buffer ended before the structure did
    NexusFmt_TooLarge,        // a declared size exceeds what we will accept
    NexusFmt_Overflow,        // arithmetic on declared sizes would overflow
    NexusFmt_BadNameTable,    // name offset outside the string table, or unterminated
    NexusFmt_OutOfRange,      // index past the end of a collection
    NexusFmt_Unsupported,     // well-formed but not something we handle
    NexusFmt_NotFound,        // lookup found no match
} NexusFmtResult;

/// Human-readable name for a result, for logs and test failure messages.
const char *nexusFmtStr(NexusFmtResult r);

// ---------------------------------------------------------------------------
// Bounds-safe little-endian readers.
//
// Every multi-byte field in these formats is little-endian, which matches both
// AArch64 and every host we test on -- but the data may be unaligned within
// the buffer, so the reads go through memcpy rather than a cast.
// ---------------------------------------------------------------------------

u8  nexusRdU8 (const void *base, size_t off);
u16 nexusRdU16(const void *base, size_t off);
u32 nexusRdU32(const void *base, size_t off);
u64 nexusRdU64(const void *base, size_t off);

/// Reads a 40-bit little-endian value. CNMT stores content sizes this way.
u64 nexusRdU40(const void *base, size_t off);

/// Checked addition. Returns false when a + b would wrap.
bool nexusAddOverflows(u64 a, u64 b, u64 *out);

/// Checked multiplication. Returns false when a * b would wrap.
bool nexusMulOverflows(u64 a, u64 b, u64 *out);

// ---------------------------------------------------------------------------
// UTF-8 safe text handling
// ---------------------------------------------------------------------------

/// Copies UTF-8 text into out, dropping characters that host filesystems and
/// MTP dislike, and stopping on a character boundary rather than mid-sequence.
///
/// The boundary part matters: MTP names are transmitted as UTF-16, and libnx's
/// utf8_to_utf16 rejects the whole string if it finds a truncated multi-byte
/// sequence. A naive byte-wise copy that clipped "Pokemon" mid-accent produced
/// an object with an empty name, which shows up in a file manager as an
/// unnamed folder.
///
/// Trailing spaces and dots are trimmed, since Windows rejects those.
void nexusSanitiseUtf8(const char *in, char *out, size_t out_size);

/// Length in bytes of the UTF-8 sequence introduced by byte c, or 0 when c is
/// a continuation byte or invalid.
size_t nexusUtf8SeqLen(unsigned char c);
