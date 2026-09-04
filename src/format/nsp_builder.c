// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- synthesises a PFS0 (NSP) container on the fly.

#include <stdio.h>
#include <string.h>

#include "nexus/nsp_builder.h"

static void wr_u32(u8 *p, u32 v) { memcpy(p, &v, sizeof(v)); }
static void wr_u64(u8 *p, u64 v) { memcpy(p, &v, sizeof(v)); }

void nspBuilderInit(NspBuilder *b, void *header_buf, size_t header_cap)
{
    memset(b, 0, sizeof(*b));
    b->header     = (u8 *)header_buf;
    b->header_cap = header_cap;
}

NexusFmtResult nspBuilderAdd(NspBuilder *b, const char *name, u64 size)
{
    if (b == NULL || name == NULL)   return NexusFmt_Truncated;
    if (b->finalised)                return NexusFmt_Unsupported;
    if (b->count >= NSP_MAX_ENTRIES) return NexusFmt_TooLarge;

    const size_t n = strlen(name);
    if (n == 0 || n >= NSP_MAX_NAME_LEN) return NexusFmt_TooLarge;

    NspBuilderEntry *e = &b->entries[b->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size        = size;
    e->data_offset = 0;   // assigned during finalise

    return NexusFmt_Ok;
}

NexusFmtResult nspBuilderFinalize(NspBuilder *b)
{
    if (b == NULL)    return NexusFmt_Truncated;
    if (b->finalised) return NexusFmt_Ok;
    if (b->count == 0) return NexusFmt_Truncated;

    // Name table is the concatenation of the NUL-terminated names.
    size_t name_table_size = 0;
    for (u32 i = 0; i < b->count; i++) name_table_size += strlen(b->entries[i].name) + 1;

    const size_t header_size = PARTITION_FS_HEADER_SIZE
                             + ((size_t)b->count * PFS0_ENTRY_SIZE)
                             + name_table_size;

    if (header_size > b->header_cap) return NexusFmt_TooLarge;

    memset(b->header, 0, header_size);

    wr_u32(b->header + 0x0, PFS0_MAGIC);
    wr_u32(b->header + 0x4, b->count);
    wr_u32(b->header + 0x8, (u32)name_table_size);

    u8 *const table = b->header + PARTITION_FS_HEADER_SIZE
                    + ((size_t)b->count * PFS0_ENTRY_SIZE);

    u64    data_offset = 0;
    size_t name_offset = 0;

    for (u32 i = 0; i < b->count; i++) {
        NspBuilderEntry *e = &b->entries[i];
        u8 *slot = b->header + PARTITION_FS_HEADER_SIZE + ((size_t)i * PFS0_ENTRY_SIZE);

        // Entries are laid end to end with no alignment padding. A PFS0 is
        // valid either way, and skipping padding keeps the offset arithmetic
        // -- and therefore this file -- simple enough to verify by eye.
        wr_u64(slot + 0x00, data_offset);
        wr_u64(slot + 0x08, e->size);
        wr_u32(slot + 0x10, (u32)name_offset);

        const size_t n = strlen(e->name) + 1;
        memcpy(table + name_offset, e->name, n);

        e->data_offset = data_offset;

        u64 next = 0;
        if (nexusAddOverflows(data_offset, e->size, &next)) return NexusFmt_Overflow;
        data_offset  = next;
        name_offset += n;
    }

    u64 total = 0;
    if (nexusAddOverflows(header_size, data_offset, &total)) return NexusFmt_Overflow;

    b->header_size = header_size;
    b->total_size  = total;
    b->finalised   = true;
    return NexusFmt_Ok;
}

u64 nspBuilderTotalSize(const NspBuilder *b)
{
    return (b != NULL && b->finalised) ? b->total_size : 0;
}

const u8 *nspBuilderHeader(const NspBuilder *b, size_t *out_size)
{
    if (b == NULL || !b->finalised) return NULL;
    if (out_size) *out_size = b->header_size;
    return b->header;
}

NexusFmtResult nspBuilderLocate(const NspBuilder *b, u64 offset, NspLocation *out)
{
    if (b == NULL || out == NULL) return NexusFmt_Truncated;
    if (!b->finalised)            return NexusFmt_Unsupported;
    if (offset >= b->total_size)  return NexusFmt_OutOfRange;

    memset(out, 0, sizeof(*out));

    if (offset < b->header_size) {
        out->in_header  = true;
        out->header_off = (size_t)offset;
        out->run        = b->header_size - offset;
        return NexusFmt_Ok;
    }

    const u64 data_pos = offset - b->header_size;

    // Linear scan: NSPs have a handful of entries, so an index would cost more
    // than it saves.
    for (u32 i = 0; i < b->count; i++) {
        const NspBuilderEntry *e = &b->entries[i];
        if (data_pos < e->data_offset || data_pos >= e->data_offset + e->size) continue;

        out->in_header   = false;
        out->entry_index = i;
        out->entry_off   = data_pos - e->data_offset;
        out->run         = e->size - out->entry_off;
        return NexusFmt_Ok;
    }

    // Only reachable if an entry has zero size and lands exactly on a
    // boundary; treat it as the end of the file.
    return NexusFmt_OutOfRange;
}
