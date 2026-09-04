// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- PFS0 / HFS0 parser tests.

#include <string.h>

#include "nexus_test.h"
#include "fixtures.h"

static const char *const kNames[] = {
    "0123456789abcdef0123456789abcdef.nca",
    "fedcba9876543210fedcba9876543210.cnmt.nca",
    "0123456789abcdef0123456789abcdef.tik",
};
static const u64 kSizes[] = { 0x1000, 0x800, 0x2C0 };

static void t_pfs0_roundtrip(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);
    CHECK(n > 0, "fixture build failed");

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(partitionFsGetEntryCount(&ctx), 3);
    CHECK_U64(ctx.type, PartitionFsType_Pfs0);

    // Data begins immediately after header + entries + name table.
    CHECK_U64(partitionFsGetDataOffset(&ctx), n);

    // Entries should come back in order, with offsets running end to end.
    u64 expected_offset = 0;
    for (u32 i = 0; i < 3; i++) {
        PartitionFsEntry e;
        CHECK_FMT(partitionFsGetEntry(&ctx, i, &e), NexusFmt_Ok);
        CHECK_STR(e.name, kNames[i]);
        CHECK_U64(e.size, kSizes[i]);
        CHECK_U64(e.offset, expected_offset);
        expected_offset += kSizes[i];
    }
}

static void t_pfs0_find(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);
    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_Ok);

    PartitionFsEntry e;
    CHECK_FMT(partitionFsFindEntry(&ctx, kNames[1], &e), NexusFmt_Ok);
    CHECK_U64(e.size, kSizes[1]);

    // Lookup is case-sensitive and exact.
    CHECK_FMT(partitionFsFindEntry(&ctx, "nope.nca", &e), NexusFmt_NotFound);
    CHECK_FMT(partitionFsFindEntry(&ctx, "0123456789ABCDEF0123456789abcdef.nca", &e),
              NexusFmt_NotFound);
}

static void t_hfs0_entry_stride(void)
{
    // The only structural difference from PFS0 is the 0x40 byte entry, so a
    // correct parser must still find the same names and sizes.
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Hfs0,
                                             kNames, kSizes, 3);
    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(ctx.type, PartitionFsType_Hfs0);
    CHECK_U64(ctx.entry_size, HFS0_ENTRY_SIZE);

    PartitionFsEntry e;
    CHECK_FMT(partitionFsGetEntry(&ctx, 2, &e), NexusFmt_Ok);
    CHECK_STR(e.name, kNames[2]);
    CHECK_U64(e.size, kSizes[2]);
}

static void t_peek_header_size(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);

    // This is the streaming path: only the first 16 bytes are available.
    PartitionFsType type;
    u64 header_size = 0;
    CHECK_FMT(partitionFsPeekHeaderSize(buf, PARTITION_FS_HEADER_SIZE, &type, &header_size),
              NexusFmt_Ok);
    CHECK_U64(type, PartitionFsType_Pfs0);
    CHECK_U64(header_size, n);

    // Fewer than 16 bytes cannot answer the question.
    CHECK_FMT(partitionFsPeekHeaderSize(buf, PARTITION_FS_HEADER_SIZE - 1, &type, &header_size),
              NexusFmt_Truncated);
}

static void t_bad_magic(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);
    buf[0] = 'X';

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_BadMagic);
}

static void t_truncated(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);

    PartitionFsContext ctx;
    // One byte short of the full header must be rejected, not read past.
    CHECK_FMT(partitionFsInit(&ctx, buf, n - 1), NexusFmt_Truncated);
    CHECK_FMT(partitionFsInit(&ctx, buf, 4), NexusFmt_Truncated);
    CHECK_FMT(partitionFsInit(&ctx, buf, 0), NexusFmt_Truncated);
}

static void t_absurd_entry_count(void)
{
    // A hostile header claiming a huge entry count must be rejected before any
    // arithmetic is used to size a read.
    u8 buf[FIXTURE_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "PFS0", 4);

    const u32 huge = 0xFFFFFFFFu;
    memcpy(buf + 4, &huge, 4);
    memcpy(buf + 8, &huge, 4);

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, sizeof(buf)), NexusFmt_TooLarge);

    PartitionFsType type;
    u64 header_size = 0;
    CHECK_FMT(partitionFsPeekHeaderSize(buf, PARTITION_FS_HEADER_SIZE, &type, &header_size),
              NexusFmt_TooLarge);
}

static void t_name_offset_out_of_bounds(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);

    // Point the first entry's name past the end of the name table.
    const u32 bad = 0xFFFF;
    memcpy(buf + PARTITION_FS_HEADER_SIZE + 0x10, &bad, 4);

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_BadNameTable);
}

static void t_unterminated_name(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);

    // Overwrite every NUL in the name table so no name terminates.
    PartitionFsContext probe;
    CHECK_FMT(partitionFsInit(&probe, buf, n), NexusFmt_Ok);
    for (size_t i = 0; i < probe.name_table_size; i++) {
        u8 *b = buf + probe.name_table_off + i;
        if (*b == '\0') *b = 'A';
    }

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_BadNameTable);
}

static void t_empty_partition(void)
{
    // Zero files is structurally valid: a bare header and nothing else.
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 0);
    CHECK_U64(n, PARTITION_FS_HEADER_SIZE);

    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(partitionFsGetEntryCount(&ctx), 0);

    PartitionFsEntry e;
    CHECK_FMT(partitionFsGetEntry(&ctx, 0, &e), NexusFmt_OutOfRange);
}

static void t_index_out_of_range(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildPartitionFs(buf, sizeof(buf), PartitionFsType_Pfs0,
                                             kNames, kSizes, 3);
    PartitionFsContext ctx;
    CHECK_FMT(partitionFsInit(&ctx, buf, n), NexusFmt_Ok);

    PartitionFsEntry e;
    CHECK_FMT(partitionFsGetEntry(&ctx, 3, &e), NexusFmt_OutOfRange);
    CHECK_FMT(partitionFsGetEntry(&ctx, 0xFFFFFFFFu, &e), NexusFmt_OutOfRange);
}

void test_partition_fs(void)
{
    nexusTestRun("pfs0 roundtrip",            t_pfs0_roundtrip);
    nexusTestRun("pfs0 find by name",         t_pfs0_find);
    nexusTestRun("hfs0 entry stride",         t_hfs0_entry_stride);
    nexusTestRun("peek header size",          t_peek_header_size);
    nexusTestRun("bad magic",                 t_bad_magic);
    nexusTestRun("truncated input",           t_truncated);
    nexusTestRun("absurd entry count",        t_absurd_entry_count);
    nexusTestRun("name offset out of bounds", t_name_offset_out_of_bounds);
    nexusTestRun("unterminated name",         t_unterminated_name);
    nexusTestRun("empty partition",           t_empty_partition);
    nexusTestRun("index out of range",        t_index_out_of_range);
}
