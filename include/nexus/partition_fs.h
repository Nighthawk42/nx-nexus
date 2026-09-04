// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- PFS0 / HFS0 partition filesystem parsing.
//
// PFS0 is the container an NSP is: a flat header followed by a name table and
// then the file data. HFS0 is the same shape with larger entries and per-entry
// hashes, and is what an XCI's partitions use. Both are plaintext -- unlike the
// NCAs they contain -- so they can be parsed with no keys at all.
//
// Layout (both variants):
//
//   0x00  header      0x10 bytes
//   0x10  entries     entry_count * entry_size
//   ...   name table  name_table_size bytes, NUL-separated
//   ...   file data   entry offsets are relative to here
//
// Streaming use is two-stage, because the header size is not known until the
// first 16 bytes have been read:
//
//   partitionFsPeekHeaderSize(first_16_bytes, ...)  -> how much header to read
//   partitionFsInit(whole_header, ...)              -> parse it
//
// That is exactly what the MTP install path needs: it can size and buffer the
// header from the leading bytes of the stream without holding the whole file.
#pragma once

#include "nexus/format.h"

#define PFS0_MAGIC 0x30534650u  // "PFS0" little-endian
#define HFS0_MAGIC 0x30534648u  // "HFS0" little-endian

#define PARTITION_FS_HEADER_SIZE 0x10u
#define PFS0_ENTRY_SIZE          0x18u
#define HFS0_ENTRY_SIZE          0x40u

// Sanity caps. A real NSP has a handful of files and a name table measured in
// bytes; these bounds exist so a hostile header cannot induce a huge read.
#define PARTITION_FS_MAX_ENTRIES     0x10000u   // 65536 files
#define PARTITION_FS_MAX_NAME_TABLE  0x100000u  // 1 MiB

typedef enum {
    PartitionFsType_Pfs0 = 0,
    PartitionFsType_Hfs0 = 1,
} PartitionFsType;

typedef struct {
    const char *name;   // NUL-terminated, points into the header buffer
    u64         offset; // relative to the start of the data area
    u64         size;
} PartitionFsEntry;

typedef struct {
    PartitionFsType type;
    const u8       *header;          // caller-owned; must outlive the context
    size_t          header_len;
    u32             entry_count;
    u32             name_table_size;
    u32             entry_size;      // 0x18 for PFS0, 0x40 for HFS0
    u64             header_size;     // total header bytes; data begins at this offset
    size_t          name_table_off;
} PartitionFsContext;

/// Reads just the 16-byte header prefix to learn the variant and how many
/// header bytes must be buffered before partitionFsInit can be called.
/// buf must hold at least PARTITION_FS_HEADER_SIZE bytes.
NexusFmtResult partitionFsPeekHeaderSize(const void *buf, size_t len,
                                         PartitionFsType *out_type,
                                         u64 *out_header_size);

/// Parses a complete header blob (header + entries + name table). The blob is
/// borrowed, not copied, so it must stay valid for the life of ctx.
/// Validates every entry's name offset and NUL termination up front, so
/// partitionFsGetEntry cannot subsequently fail on a bad name.
NexusFmtResult partitionFsInit(PartitionFsContext *ctx, const void *header, size_t header_len);

u32 partitionFsGetEntryCount(const PartitionFsContext *ctx);

/// Byte offset, relative to the start of the partition, at which file data
/// begins. Add an entry's offset to this to get its absolute position.
u64 partitionFsGetDataOffset(const PartitionFsContext *ctx);

NexusFmtResult partitionFsGetEntry(const PartitionFsContext *ctx, u32 index,
                                   PartitionFsEntry *out);

/// Case-sensitive exact-name lookup. Returns NexusFmt_NotFound if absent.
NexusFmtResult partitionFsFindEntry(const PartitionFsContext *ctx, const char *name,
                                    PartitionFsEntry *out);
