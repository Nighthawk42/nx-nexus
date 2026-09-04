// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- fixture builders.

#include <string.h>

#include "fixtures.h"

static void wr_u16(u8 *p, u16 v) { memcpy(p, &v, sizeof(v)); }
static void wr_u32(u8 *p, u32 v) { memcpy(p, &v, sizeof(v)); }
static void wr_u64(u8 *p, u64 v) { memcpy(p, &v, sizeof(v)); }

size_t fixtureBuildPartitionFs(u8 *buf, size_t cap, PartitionFsType type,
                               const char *const *names, const u64 *sizes, u32 count)
{
    const u32 entry_size = (type == PartitionFsType_Hfs0) ? HFS0_ENTRY_SIZE : PFS0_ENTRY_SIZE;
    const u32 magic      = (type == PartitionFsType_Hfs0) ? HFS0_MAGIC : PFS0_MAGIC;

    // Name table is the concatenation of the NUL-terminated names.
    size_t name_table_size = 0;
    for (u32 i = 0; i < count; i++) name_table_size += strlen(names[i]) + 1;

    const size_t total = PARTITION_FS_HEADER_SIZE + ((size_t)count * entry_size) + name_table_size;
    if (cap < total) return 0;

    memset(buf, 0, total);

    wr_u32(buf + 0x0, magic);
    wr_u32(buf + 0x4, count);
    wr_u32(buf + 0x8, (u32)name_table_size);

    u8 *const table = buf + PARTITION_FS_HEADER_SIZE + ((size_t)count * entry_size);

    // Entry offsets are laid out end to end, as a real packer would.
    u64    data_offset = 0;
    size_t name_offset = 0;

    for (u32 i = 0; i < count; i++) {
        u8 *e = buf + PARTITION_FS_HEADER_SIZE + ((size_t)i * entry_size);

        wr_u64(e + 0x00, data_offset);
        wr_u64(e + 0x08, sizes[i]);
        wr_u32(e + 0x10, (u32)name_offset);

        const size_t n = strlen(names[i]) + 1;
        memcpy(table + name_offset, names[i], n);

        data_offset += sizes[i];
        name_offset += n;
    }

    return total;
}

size_t fixtureBuildCnmt(u8 *buf, size_t cap, u64 title_id, u32 version, u8 meta_type,
                        u16 ext_header_size, const u8 *content_types,
                        const u64 *content_sizes, u16 content_count)
{
    const size_t total = CNMT_HEADER_SIZE + ext_header_size
                       + ((size_t)content_count * CNMT_CONTENT_INFO_SIZE);
    if (cap < total) return 0;

    memset(buf, 0, total);

    wr_u64(buf + 0x00, title_id);
    wr_u32(buf + 0x08, version);
    buf[0x0C] = meta_type;
    buf[0x0D] = 0;                       // ContentMetaPlatform
    wr_u16(buf + 0x0E, ext_header_size);
    wr_u16(buf + 0x10, content_count);
    wr_u16(buf + 0x12, 0);               // ContentMetaCount
    buf[0x14] = 0;                       // ContentMetaAttributes
    wr_u32(buf + 0x18, 0);               // RequiredDownloadSystemVersion

    u8 *ci = buf + CNMT_HEADER_SIZE + ext_header_size;

    for (u16 i = 0; i < content_count; i++) {
        u8 *e = ci + ((size_t)i * CNMT_CONTENT_INFO_SIZE);

        memset(e + 0x00, 0x11 + i, CNMT_HASH_SIZE);          // Hash
        memset(e + 0x20, 0xA0 + i, CNMT_CONTENT_ID_SIZE);    // ContentId

        // Size is a 40-bit little-endian field.
        const u64 sz = content_sizes[i];
        for (size_t b = 0; b < 5; b++) e[0x30 + b] = (u8)((sz >> (8 * b)) & 0xFF);

        e[0x35] = 0;                    // ContentAttributes
        e[0x36] = content_types[i];     // ContentType
        e[0x37] = 0;                    // IdOffset
    }

    return total;
}

size_t fixtureBuildTicket(u8 *buf, size_t cap, u32 sig_type, u8 key_type,
                          u64 title_id, u8 key_generation)
{
    size_t sig_data_size = 0;
    if (ticketGetSigDataSize(sig_type, &sig_data_size) != NexusFmt_Ok) return 0;

    const size_t total = sig_data_size + TICKET_DATA_SIZE;
    if (cap < total) return 0;

    memset(buf, 0, total);
    wr_u32(buf + 0, sig_type);

    u8 *td = buf + sig_data_size;

    td[0x140] = 2;              // format version, always 2 on Switch
    td[0x141] = key_type;
    wr_u16(td + 0x142, 1);      // ticket version
    td[0x144] = 0;              // license type
    td[0x145] = key_generation; // master key revision
    wr_u16(td + 0x146, 0);      // properties
    wr_u64(td + 0x150, 0x1234567890ABCDEFull);  // ticket id
    wr_u64(td + 0x158, 0);      // device id (0 == not console-bound)

    // Rights id: title id big-endian, then zeroes, then key generation.
    for (size_t i = 0; i < 8; i++) {
        td[0x160 + i] = (u8)((title_id >> (8 * (7 - i))) & 0xFF);
    }
    td[0x160 + 0x0F] = key_generation;

    wr_u32(td + 0x170, 0);      // account id

    return total;
}

size_t fixtureBuildNsp(u8 *buf, size_t cap, const FixtureNspFile *files, u32 count)
{
    // Header first, so the entry offsets can be computed against the data area.
    size_t name_table_size = 0;
    for (u32 i = 0; i < count; i++) name_table_size += strlen(files[i].name) + 1;

    const size_t header_size = PARTITION_FS_HEADER_SIZE
                             + ((size_t)count * PFS0_ENTRY_SIZE)
                             + name_table_size;

    size_t data_size = 0;
    for (u32 i = 0; i < count; i++) data_size += files[i].size;

    if (cap < header_size + data_size) return 0;

    memset(buf, 0, header_size + data_size);

    wr_u32(buf + 0x0, PFS0_MAGIC);
    wr_u32(buf + 0x4, count);
    wr_u32(buf + 0x8, (u32)name_table_size);

    u8 *const table = buf + PARTITION_FS_HEADER_SIZE + ((size_t)count * PFS0_ENTRY_SIZE);
    u8 *const data  = buf + header_size;

    u64    data_offset = 0;
    size_t name_offset = 0;

    for (u32 i = 0; i < count; i++) {
        u8 *e = buf + PARTITION_FS_HEADER_SIZE + ((size_t)i * PFS0_ENTRY_SIZE);

        wr_u64(e + 0x00, data_offset);
        wr_u64(e + 0x08, files[i].size);
        wr_u32(e + 0x10, (u32)name_offset);

        const size_t n = strlen(files[i].name) + 1;
        memcpy(table + name_offset, files[i].name, n);

        if (files[i].data != NULL && files[i].size > 0) {
            memcpy(data + data_offset, files[i].data, files[i].size);
        }

        data_offset += files[i].size;
        name_offset += n;
    }

    return header_size + data_size;
}
