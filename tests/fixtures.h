// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- builders that synthesise well-formed container blobs for tests.
//
// Building fixtures programmatically rather than checking in binary blobs keeps
// the tests readable and lets each one perturb exactly one field to exercise a
// specific failure path.
#pragma once

#include "nexus/format.h"
#include "nexus/partition_fs.h"
#include "nexus/cnmt.h"
#include "nexus/ticket.h"

#define FIXTURE_BUF_SIZE 8192

/// Builds a PFS0 (or HFS0) header into buf: header, entries and name table.
/// File data is not included -- entry offsets are relative to the data area,
/// which begins immediately after what this writes.
/// Returns the number of bytes written, or 0 if buf is too small.
size_t fixtureBuildPartitionFs(u8 *buf, size_t cap, PartitionFsType type,
                               const char *const *names, const u64 *sizes, u32 count);

/// Builds a CNMT: header, an extended header of ext_header_size zero bytes,
/// then content_count PackagedContentInfo entries.
/// content_types and content_sizes must each have content_count elements; the
/// content id of entry i is filled with the byte value (0xA0 + i).
size_t fixtureBuildCnmt(u8 *buf, size_t cap, u64 title_id, u32 version, u8 meta_type,
                        u16 ext_header_size, const u8 *content_types,
                        const u64 *content_sizes, u16 content_count);

/// One file to place inside a synthetic NSP.
typedef struct {
    const char *name;
    const u8   *data;
    size_t      size;
} FixtureNspFile;

/// Builds a complete NSP (a PFS0 with file data following the header).
/// Returns total bytes written, or 0 if buf is too small.
size_t fixtureBuildNsp(u8 *buf, size_t cap, const FixtureNspFile *files, u32 count);

/// Builds a ticket with the given signature type and key type. The rights id
/// is title_id big-endian in the first 8 bytes, zeroes, then key_generation in
/// the last byte.
size_t fixtureBuildTicket(u8 *buf, size_t cap, u32 sig_type, u8 key_type,
                          u64 title_id, u8 key_generation);
