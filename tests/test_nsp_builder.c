// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- virtual NSP layout tests.
//
// The strongest test here reads the synthesised container back through the
// real PFS0 parser: if the builder and the parser disagree about a single
// offset, the round trip catches it. That matters because a bad layout
// produces an NSP that looks fine until someone tries to install it.

#include <string.h>

#include "nexus_test.h"
#include "fixtures.h"
#include "nexus/nsp_builder.h"

static u8 g_header[4096];

static void t_builder_layout(void)
{
    NspBuilder b;
    nspBuilderInit(&b, g_header, sizeof(g_header));

    CHECK_FMT(nspBuilderAdd(&b, "aaaa.cnmt.nca", 0x1000), NexusFmt_Ok);
    CHECK_FMT(nspBuilderAdd(&b, "bbbb.nca",      0x20000), NexusFmt_Ok);
    CHECK_FMT(nspBuilderAdd(&b, "cccc.tik",      0x400), NexusFmt_Ok);
    CHECK_FMT(nspBuilderFinalize(&b), NexusFmt_Ok);

    size_t header_size = 0;
    CHECK(nspBuilderHeader(&b, &header_size) != NULL, "header should exist");

    // Header is 0x10 + 3 entries of 0x18 + the name table.
    const size_t names = strlen("aaaa.cnmt.nca") + 1
                       + strlen("bbbb.nca") + 1
                       + strlen("cccc.tik") + 1;
    CHECK_U64(header_size, 0x10 + (3 * 0x18) + names);
    CHECK_U64(nspBuilderTotalSize(&b), header_size + 0x1000 + 0x20000 + 0x400);
}

static void t_builder_parses_back(void)
{
    // Round trip: the synthesised header must satisfy the real parser.
    NspBuilder b;
    nspBuilderInit(&b, g_header, sizeof(g_header));

    nspBuilderAdd(&b, "aaaa.cnmt.nca", 0x1000);
    nspBuilderAdd(&b, "bbbb.nca",      0x20000);
    nspBuilderAdd(&b, "cccc.tik",      0x400);
    CHECK_FMT(nspBuilderFinalize(&b), NexusFmt_Ok);

    size_t header_size = 0;
    const u8 *header = nspBuilderHeader(&b, &header_size);

    PartitionFsContext pfs;
    CHECK_FMT(partitionFsInit(&pfs, header, header_size), NexusFmt_Ok);
    CHECK_U64(partitionFsGetEntryCount(&pfs), 3);
    CHECK_U64(partitionFsGetDataOffset(&pfs), header_size);

    const char *names[] = { "aaaa.cnmt.nca", "bbbb.nca", "cccc.tik" };
    const u64   sizes[] = { 0x1000, 0x20000, 0x400 };

    u64 expect_off = 0;
    for (u32 i = 0; i < 3; i++) {
        PartitionFsEntry e;
        CHECK_FMT(partitionFsGetEntry(&pfs, i, &e), NexusFmt_Ok);
        CHECK_STR(e.name, names[i]);
        CHECK_U64(e.size, sizes[i]);
        CHECK_U64(e.offset, expect_off);
        expect_off += sizes[i];
    }
}

static void t_locate_regions(void)
{
    NspBuilder b;
    nspBuilderInit(&b, g_header, sizeof(g_header));

    nspBuilderAdd(&b, "a.nca", 0x100);
    nspBuilderAdd(&b, "b.nca", 0x200);
    nspBuilderFinalize(&b);

    size_t hs = 0;
    nspBuilderHeader(&b, &hs);

    NspLocation loc;

    // Start of the header.
    CHECK_FMT(nspBuilderLocate(&b, 0, &loc), NexusFmt_Ok);
    CHECK(loc.in_header, "offset 0 is in the header");
    CHECK_U64(loc.header_off, 0);
    CHECK_U64(loc.run, hs);

    // Last header byte: the run must stop exactly at the boundary so a read
    // never straddles the header and the first file.
    CHECK_FMT(nspBuilderLocate(&b, hs - 1, &loc), NexusFmt_Ok);
    CHECK(loc.in_header, "last header byte is in the header");
    CHECK_U64(loc.run, 1);

    // First data byte belongs to entry 0.
    CHECK_FMT(nspBuilderLocate(&b, hs, &loc), NexusFmt_Ok);
    CHECK(!loc.in_header, "first data byte is not in the header");
    CHECK_U64(loc.entry_index, 0);
    CHECK_U64(loc.entry_off, 0);
    CHECK_U64(loc.run, 0x100);

    // Middle of entry 0.
    CHECK_FMT(nspBuilderLocate(&b, hs + 0x80, &loc), NexusFmt_Ok);
    CHECK_U64(loc.entry_index, 0);
    CHECK_U64(loc.entry_off, 0x80);
    CHECK_U64(loc.run, 0x80);

    // First byte of entry 1.
    CHECK_FMT(nspBuilderLocate(&b, hs + 0x100, &loc), NexusFmt_Ok);
    CHECK_U64(loc.entry_index, 1);
    CHECK_U64(loc.entry_off, 0);
    CHECK_U64(loc.run, 0x200);

    // Last byte of the file.
    CHECK_FMT(nspBuilderLocate(&b, hs + 0x2FF, &loc), NexusFmt_Ok);
    CHECK_U64(loc.entry_index, 1);
    CHECK_U64(loc.run, 1);

    // One past the end.
    CHECK_FMT(nspBuilderLocate(&b, hs + 0x300, &loc), NexusFmt_OutOfRange);
}

static void t_full_walk_is_contiguous(void)
{
    // Walking the whole virtual file by following `run` must cover every byte
    // exactly once and land precisely on the end. This is the property the
    // MTP read path relies on.
    NspBuilder b;
    nspBuilderInit(&b, g_header, sizeof(g_header));

    nspBuilderAdd(&b, "aaaa.cnmt.nca", 0x1234);
    nspBuilderAdd(&b, "bbbb.nca",      0x5678);
    nspBuilderAdd(&b, "cccc.tik",      0x9AB);
    nspBuilderAdd(&b, "dddd.cert",     0x700);
    nspBuilderFinalize(&b);

    const u64 total = nspBuilderTotalSize(&b);
    u64 pos = 0;
    u32 steps = 0;

    while (pos < total) {
        NspLocation loc;
        CHECK_FMT(nspBuilderLocate(&b, pos, &loc), NexusFmt_Ok);
        CHECK(loc.run > 0, "run must advance at offset %llu", (unsigned long long)pos);
        pos += loc.run;
        if (++steps > 64) break;   // guard against a non-advancing loop
    }

    CHECK_U64(pos, total);
}

static void t_builder_limits(void)
{
    NspBuilder b;
    nspBuilderInit(&b, g_header, sizeof(g_header));

    // A name that does not fit is rejected rather than truncated into a
    // colliding entry.
    char huge[NSP_MAX_NAME_LEN + 8];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK_FMT(nspBuilderAdd(&b, huge, 1), NexusFmt_TooLarge);

    CHECK_FMT(nspBuilderAdd(&b, "", 1), NexusFmt_TooLarge);

    // Finalising with nothing added is not a valid container.
    CHECK_FMT(nspBuilderFinalize(&b), NexusFmt_Truncated);

    // Adding after finalise must not silently corrupt a built layout.
    nspBuilderAdd(&b, "a.nca", 0x10);
    CHECK_FMT(nspBuilderFinalize(&b), NexusFmt_Ok);
    CHECK_FMT(nspBuilderAdd(&b, "b.nca", 0x10), NexusFmt_Unsupported);
}

static void t_header_buffer_too_small(void)
{
    // A tiny buffer must fail cleanly at finalise, not overrun.
    u8 tiny[24];
    NspBuilder b;
    nspBuilderInit(&b, tiny, sizeof(tiny));

    nspBuilderAdd(&b, "aaaa.cnmt.nca", 0x1000);
    nspBuilderAdd(&b, "bbbb.nca", 0x1000);
    CHECK_FMT(nspBuilderFinalize(&b), NexusFmt_TooLarge);
    CHECK_U64(nspBuilderTotalSize(&b), 0);
}

void test_nsp_builder(void)
{
    nexusTestRun("nsp layout",              t_builder_layout);
    nexusTestRun("nsp parses back",         t_builder_parses_back);
    nexusTestRun("locate regions",          t_locate_regions);
    nexusTestRun("full walk is contiguous", t_full_walk_is_contiguous);
    nexusTestRun("builder limits",          t_builder_limits);
    nexusTestRun("header buffer too small", t_header_buffer_too_small);
}
