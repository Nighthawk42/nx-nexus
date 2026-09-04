// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- PFS0 / HFS0 parsing.

#include <string.h>

#include "nexus/partition_fs.h"

// Field offsets within the 0x10 byte header, identical for both variants.
#define OFF_MAGIC           0x0
#define OFF_ENTRY_COUNT     0x4
#define OFF_NAME_TABLE_SIZE 0x8

// Field offsets within an entry. The first three fields are common to both
// variants; HFS0 adds hash fields after them that this parser ignores.
#define ENT_OFF_OFFSET      0x0
#define ENT_OFF_SIZE        0x8
#define ENT_OFF_NAME_OFFSET 0x10

// Computes the total header size for a given variant and counts, rejecting any
// combination whose arithmetic would overflow or exceed the sanity caps.
static NexusFmtResult compute_header_size(u32 entry_count, u32 name_table_size,
                                          u32 entry_size, u64 *out)
{
    if (entry_count > PARTITION_FS_MAX_ENTRIES)       return NexusFmt_TooLarge;
    if (name_table_size > PARTITION_FS_MAX_NAME_TABLE) return NexusFmt_TooLarge;

    u64 entries_bytes = 0;
    if (nexusMulOverflows(entry_count, entry_size, &entries_bytes)) return NexusFmt_Overflow;

    u64 total = 0;
    if (nexusAddOverflows(PARTITION_FS_HEADER_SIZE, entries_bytes, &total)) return NexusFmt_Overflow;
    if (nexusAddOverflows(total, name_table_size, &total))                  return NexusFmt_Overflow;

    *out = total;
    return NexusFmt_Ok;
}

// Identifies the variant from the magic and reports its entry size.
static NexusFmtResult classify(u32 magic, PartitionFsType *out_type, u32 *out_entry_size)
{
    if (magic == PFS0_MAGIC) {
        *out_type       = PartitionFsType_Pfs0;
        *out_entry_size = PFS0_ENTRY_SIZE;
        return NexusFmt_Ok;
    }
    if (magic == HFS0_MAGIC) {
        *out_type       = PartitionFsType_Hfs0;
        *out_entry_size = HFS0_ENTRY_SIZE;
        return NexusFmt_Ok;
    }
    return NexusFmt_BadMagic;
}

NexusFmtResult partitionFsPeekHeaderSize(const void *buf, size_t len,
                                         PartitionFsType *out_type,
                                         u64 *out_header_size)
{
    if (buf == NULL || out_type == NULL || out_header_size == NULL) return NexusFmt_Truncated;
    if (len < PARTITION_FS_HEADER_SIZE) return NexusFmt_Truncated;

    PartitionFsType type;
    u32 entry_size;
    NexusFmtResult r = classify(nexusRdU32(buf, OFF_MAGIC), &type, &entry_size);
    if (r != NexusFmt_Ok) return r;

    const u32 entry_count     = nexusRdU32(buf, OFF_ENTRY_COUNT);
    const u32 name_table_size = nexusRdU32(buf, OFF_NAME_TABLE_SIZE);

    u64 header_size = 0;
    r = compute_header_size(entry_count, name_table_size, entry_size, &header_size);
    if (r != NexusFmt_Ok) return r;

    *out_type        = type;
    *out_header_size = header_size;
    return NexusFmt_Ok;
}

NexusFmtResult partitionFsInit(PartitionFsContext *ctx, const void *header, size_t header_len)
{
    if (ctx == NULL || header == NULL) return NexusFmt_Truncated;
    if (header_len < PARTITION_FS_HEADER_SIZE) return NexusFmt_Truncated;

    memset(ctx, 0, sizeof(*ctx));

    PartitionFsType type;
    u32 entry_size;
    NexusFmtResult r = classify(nexusRdU32(header, OFF_MAGIC), &type, &entry_size);
    if (r != NexusFmt_Ok) return r;

    const u32 entry_count     = nexusRdU32(header, OFF_ENTRY_COUNT);
    const u32 name_table_size = nexusRdU32(header, OFF_NAME_TABLE_SIZE);

    u64 header_size = 0;
    r = compute_header_size(entry_count, name_table_size, entry_size, &header_size);
    if (r != NexusFmt_Ok) return r;

    // The caller must have buffered the whole header, not just its prefix.
    if (header_len < header_size) return NexusFmt_Truncated;

    ctx->type            = type;
    ctx->header          = (const u8 *)header;
    ctx->header_len      = header_len;
    ctx->entry_count     = entry_count;
    ctx->name_table_size = name_table_size;
    ctx->entry_size      = entry_size;
    ctx->header_size     = header_size;
    ctx->name_table_off  = (size_t)(PARTITION_FS_HEADER_SIZE + ((u64)entry_count * entry_size));

    // Validate every name up front. Doing it here means partitionFsGetEntry
    // can hand back a plain const char* that is guaranteed NUL-terminated
    // inside the buffer, with no per-call checking.
    const char *table = (const char *)ctx->header + ctx->name_table_off;
    for (u32 i = 0; i < entry_count; i++) {
        const size_t ent = (size_t)(PARTITION_FS_HEADER_SIZE + ((u64)i * entry_size));
        const u32 name_off = nexusRdU32(ctx->header, ent + ENT_OFF_NAME_OFFSET);

        if (name_off >= name_table_size) return NexusFmt_BadNameTable;

        // The name must terminate before the table does.
        if (memchr(table + name_off, '\0', name_table_size - name_off) == NULL) {
            return NexusFmt_BadNameTable;
        }
    }

    return NexusFmt_Ok;
}

u32 partitionFsGetEntryCount(const PartitionFsContext *ctx)
{
    return (ctx != NULL) ? ctx->entry_count : 0;
}

u64 partitionFsGetDataOffset(const PartitionFsContext *ctx)
{
    return (ctx != NULL) ? ctx->header_size : 0;
}

NexusFmtResult partitionFsGetEntry(const PartitionFsContext *ctx, u32 index,
                                   PartitionFsEntry *out)
{
    if (ctx == NULL || out == NULL)  return NexusFmt_Truncated;
    if (index >= ctx->entry_count)   return NexusFmt_OutOfRange;

    const size_t ent = (size_t)(PARTITION_FS_HEADER_SIZE + ((u64)index * ctx->entry_size));

    out->offset = nexusRdU64(ctx->header, ent + ENT_OFF_OFFSET);
    out->size   = nexusRdU64(ctx->header, ent + ENT_OFF_SIZE);

    const u32 name_off = nexusRdU32(ctx->header, ent + ENT_OFF_NAME_OFFSET);
    out->name = (const char *)ctx->header + ctx->name_table_off + name_off;

    return NexusFmt_Ok;
}

NexusFmtResult partitionFsFindEntry(const PartitionFsContext *ctx, const char *name,
                                    PartitionFsEntry *out)
{
    if (ctx == NULL || name == NULL || out == NULL) return NexusFmt_Truncated;

    for (u32 i = 0; i < ctx->entry_count; i++) {
        PartitionFsEntry e;
        if (partitionFsGetEntry(ctx, i, &e) != NexusFmt_Ok) continue;
        if (strcmp(e.name, name) == 0) {
            *out = e;
            return NexusFmt_Ok;
        }
    }
    return NexusFmt_NotFound;
}
