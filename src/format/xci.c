// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- XCI (gamecard image) header parsing.

#include <stdio.h>
#include <string.h>

#include "nexus/xci.h"

// Field offsets within the 0x200-byte gamecard header.
#define OFF_MAGIC             0x100u
#define OFF_ROM_SIZE          0x10Du
#define OFF_HEADER_VERSION    0x10Eu
#define OFF_FLAGS             0x10Fu
#define OFF_VALID_DATA_END    0x118u   // u32, media units
#define OFF_ROOT_OFFSET       0x130u   // u64, bytes
#define OFF_ROOT_HEADER_SIZE  0x138u   // u64, bytes

// nn::gc::MemoryCapacity. The values are not a sequence, so they are looked up.
static u64 rom_size_bytes(u8 rom_size)
{
    switch (rom_size) {
        case 0xFA: return  1ull * 1024 * 1024 * 1024;   // 1 GB
        case 0xF8: return  2ull * 1024 * 1024 * 1024;   // 2 GB
        case 0xF0: return  4ull * 1024 * 1024 * 1024;   // 4 GB
        case 0xE0: return  8ull * 1024 * 1024 * 1024;   // 8 GB
        case 0xE1: return 16ull * 1024 * 1024 * 1024;   // 16 GB
        case 0xE2: return 32ull * 1024 * 1024 * 1024;   // 32 GB
        default:   return 0;
    }
}

const char *xciCartSizeStr(u64 cart_size)
{
    switch (cart_size) {
        case  1ull * 1024 * 1024 * 1024: return "1 GB";
        case  2ull * 1024 * 1024 * 1024: return "2 GB";
        case  4ull * 1024 * 1024 * 1024: return "4 GB";
        case  8ull * 1024 * 1024 * 1024: return "8 GB";
        case 16ull * 1024 * 1024 * 1024: return "16 GB";
        case 32ull * 1024 * 1024 * 1024: return "32 GB";
        default:                         return "unknown";
    }
}

NexusFmtResult xciParseHeader(const void *buf, size_t len, XciHeader *out)
{
    if (buf == NULL || out == NULL) return NexusFmt_Truncated;
    if (len < XCI_HEADER_SIZE)      return NexusFmt_Truncated;

    if (nexusRdU32(buf, OFF_MAGIC) != XCI_MAGIC) return NexusFmt_BadMagic;

    memset(out, 0, sizeof(*out));

    out->root_offset      = nexusRdU64(buf, OFF_ROOT_OFFSET);
    out->root_header_size = nexusRdU64(buf, OFF_ROOT_HEADER_SIZE);
    out->header_version   = nexusRdU8(buf, OFF_HEADER_VERSION);
    out->flags            = nexusRdU8(buf, OFF_FLAGS);
    out->cart_size        = rom_size_bytes(nexusRdU8(buf, OFF_ROM_SIZE));

    // Media units, so the multiply is checked before it is trusted.
    const u64 end_pages = (u64)nexusRdU32(buf, OFF_VALID_DATA_END);
    if (nexusMulOverflows(end_pages + 1, XCI_MEDIA_UNIT, &out->valid_data_end)) {
        return NexusFmt_Overflow;
    }

    // The root HFS0 must sit after the header and declare a plausible size.
    if (out->root_offset < XCI_HEADER_SIZE) return NexusFmt_Truncated;

    if (out->root_header_size < PARTITION_FS_HEADER_SIZE
        || out->root_header_size > PARTITION_FS_MAX_NAME_TABLE) {
        return NexusFmt_TooLarge;
    }

    u64 root_end = 0;
    if (nexusAddOverflows(out->root_offset, out->root_header_size, &root_end)) {
        return NexusFmt_Overflow;
    }

    return NexusFmt_Ok;
}

NexusFmtResult xciFindPartition(const XciHeader *hdr, const PartitionFsContext *root,
                                const char *name, u64 *out_offset, u64 *out_size)
{
    if (hdr == NULL || root == NULL || name == NULL) return NexusFmt_Truncated;

    PartitionFsEntry entry;
    const NexusFmtResult r = partitionFsFindEntry(root, name, &entry);
    if (r != NexusFmt_Ok) return r;

    // Partition offsets are relative to the end of the root header, which
    // itself sits at root_offset within the image.
    u64 base = 0;
    if (nexusAddOverflows(hdr->root_offset, partitionFsGetDataOffset(root), &base)) {
        return NexusFmt_Overflow;
    }

    u64 absolute = 0;
    if (nexusAddOverflows(base, entry.offset, &absolute)) return NexusFmt_Overflow;

    u64 end = 0;
    if (nexusAddOverflows(absolute, entry.size, &end)) return NexusFmt_Overflow;

    if (out_offset != NULL) *out_offset = absolute;
    if (out_size   != NULL) *out_size   = entry.size;
    return NexusFmt_Ok;
}
