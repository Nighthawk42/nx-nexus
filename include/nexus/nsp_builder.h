// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- synthesises a PFS0 (NSP) container on the fly.
//
// Extracting an installed title means presenting one virtual .nsp file that
// does not exist anywhere on disk: its header is generated, and its body is the
// registered NCAs read straight out of ncm content storage.
//
// MTP can ask for any byte range of that file, so this builds the header once
// and then answers "what lives at offset X" for arbitrary reads. All of that is
// pure arithmetic with no libnx dependency, which is why it is unit-tested on
// the host -- an off-by-one in this layout would produce a corrupt NSP that
// only fails much later, when someone tries to install it.
#pragma once

#include "nexus/format.h"
#include "nexus/partition_fs.h"

#define NSP_MAX_ENTRIES   64
#define NSP_MAX_NAME_LEN  80

typedef struct {
    char name[NSP_MAX_NAME_LEN];
    u64  size;
    u64  data_offset;   // relative to the start of the data area
} NspBuilderEntry;

typedef struct {
    NspBuilderEntry entries[NSP_MAX_ENTRIES];
    u32             count;

    u8    *header;        // caller-owned buffer the header is built into
    size_t header_cap;
    size_t header_size;   // 0 until finalised

    u64  total_size;
    bool finalised;
} NspBuilder;

/// Where a given absolute offset in the virtual NSP falls.
typedef struct {
    bool   in_header;
    size_t header_off;   // valid when in_header
    u32    entry_index;  // valid when !in_header
    u64    entry_off;    // offset within that entry's data
    u64    run;          // contiguous bytes available from here without
                         // crossing into another region
} NspLocation;

/// Prepares a builder. header_buf is borrowed and must outlive the builder.
void nspBuilderInit(NspBuilder *b, void *header_buf, size_t header_cap);

/// Adds one file. Must be called before finalising.
NexusFmtResult nspBuilderAdd(NspBuilder *b, const char *name, u64 size);

/// Builds the PFS0 header and fixes every entry's offset.
NexusFmtResult nspBuilderFinalize(NspBuilder *b);

/// Total size of the virtual file: header plus all entry data.
u64 nspBuilderTotalSize(const NspBuilder *b);

const u8 *nspBuilderHeader(const NspBuilder *b, size_t *out_size);

/// Resolves an absolute offset. Returns NexusFmt_OutOfRange past the end.
NexusFmtResult nspBuilderLocate(const NspBuilder *b, u64 offset, NspLocation *out);
