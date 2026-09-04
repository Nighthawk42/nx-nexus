// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- CNMT parser tests.

#include <string.h>

#include "nexus_test.h"
#include "fixtures.h"

#define TEST_TITLE_ID 0x0100000000010000ull

// A realistic Application: one meta, one program, one control.
static const u8  kTypes[] = {
    CnmtContentType_Meta,
    CnmtContentType_Program,
    CnmtContentType_Control,
};
static const u64 kSizes[] = { 0x4000, 0x40000000, 0x20000 };

// ApplicationMetaExtendedHeader is 0x10 bytes.
#define APP_EXT_HEADER_SIZE 0x10

static void t_cnmt_header(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0x10000,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      kTypes, kSizes, 3);
    CHECK(n > 0, "fixture build failed");

    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(ctx.title_id, TEST_TITLE_ID);
    CHECK_U64(ctx.version, 0x10000);
    CHECK_U64(ctx.meta_type, CnmtMetaType_Application);
    CHECK_U64(ctx.extended_header_size, APP_EXT_HEADER_SIZE);
    CHECK_U64(cnmtGetContentCount(&ctx), 3);
    CHECK_STR(cnmtMetaTypeStr(ctx.meta_type), "Application");

    // Content info must start after the header and the extended header --
    // getting this wrong is the classic CNMT parsing bug.
    CHECK_U64(ctx.content_info_off, CNMT_HEADER_SIZE + APP_EXT_HEADER_SIZE);
}

static void t_cnmt_contents(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      kTypes, kSizes, 3);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);

    for (u16 i = 0; i < 3; i++) {
        CnmtContentInfo info;
        CHECK_FMT(cnmtGetContentInfo(&ctx, i, &info), NexusFmt_Ok);
        CHECK_U64(info.content_type, kTypes[i]);
        CHECK_U64(info.size, kSizes[i]);

        // The fixture fills each content id with a distinct repeated byte.
        for (size_t b = 0; b < CNMT_CONTENT_ID_SIZE; b++) {
            CHECK_U64(info.content_id[b], 0xA0 + i);
        }
    }
}

static void t_cnmt_40bit_size(void)
{
    // The size field is 40 bits. A value that needs all five bytes must
    // survive the round trip, and must not bleed into ContentAttributes.
    const u8  types[] = { CnmtContentType_Program };
    const u64 sizes[] = { 0xFEDCBA9876ull };   // just under 2^40

    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      types, sizes, 1);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);

    CnmtContentInfo info;
    CHECK_FMT(cnmtGetContentInfo(&ctx, 0, &info), NexusFmt_Ok);
    CHECK_U64(info.size, 0xFEDCBA9876ull);
    CHECK_U64(info.content_attributes, 0);
    CHECK_U64(info.content_type, CnmtContentType_Program);
}

static void t_cnmt_find_by_type(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      kTypes, kSizes, 3);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);

    CnmtContentInfo info;
    CHECK_FMT(cnmtFindContentByType(&ctx, CnmtContentType_Program, &info), NexusFmt_Ok);
    CHECK_U64(info.size, kSizes[1]);

    CHECK_FMT(cnmtFindContentByType(&ctx, CnmtContentType_LegalInformation, &info),
              NexusFmt_NotFound);
}

static void t_cnmt_total_size(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      kTypes, kSizes, 3);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);

    u64 total = 0;
    CHECK_FMT(cnmtGetTotalContentSize(&ctx, &total), NexusFmt_Ok);
    CHECK_U64(total, kSizes[0] + kSizes[1] + kSizes[2]);
}

static void t_cnmt_content_id_hex(void)
{
    u8 id[CNMT_CONTENT_ID_SIZE];
    memset(id, 0xAB, sizeof(id));

    char hex[33];
    cnmtFormatContentId(id, hex, sizeof(hex));
    CHECK_STR(hex, "abababababababababababababababab");

    // A buffer one byte short must produce an empty string, never a partial
    // id that could be used to build a filename.
    char small[32];
    cnmtFormatContentId(id, small, sizeof(small));
    CHECK_STR(small, "");
}

static void t_cnmt_truncated(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_Application, APP_EXT_HEADER_SIZE,
                                      kTypes, kSizes, 3);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n - 1), NexusFmt_Truncated);
    CHECK_FMT(cnmtInit(&ctx, buf, CNMT_HEADER_SIZE), NexusFmt_Truncated);
    CHECK_FMT(cnmtInit(&ctx, buf, 0), NexusFmt_Truncated);
}

static void t_cnmt_absurd_content_count(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    const u16 huge = 0xFFFF;
    memcpy(buf + 0x10, &huge, 2);   // ContentCount

    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, sizeof(buf)), NexusFmt_TooLarge);
}

static void t_cnmt_extended_header_overrun(void)
{
    // An extended header size that runs past the buffer must be caught rather
    // than used as an offset into it.
    u8 buf[FIXTURE_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    const u16 ext = 0xFFFF;
    const u16 one = 1;
    memcpy(buf + 0x0E, &ext, 2);    // ExtendedHeaderSize
    memcpy(buf + 0x10, &one, 2);    // ContentCount

    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, 0x100), NexusFmt_Truncated);
}

static void t_cnmt_no_contents(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildCnmt(buf, sizeof(buf), TEST_TITLE_ID, 0,
                                      CnmtMetaType_SystemData, 0, NULL, NULL, 0);
    CnmtContext ctx;
    CHECK_FMT(cnmtInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(cnmtGetContentCount(&ctx), 0);

    u64 total = 1;
    CHECK_FMT(cnmtGetTotalContentSize(&ctx, &total), NexusFmt_Ok);
    CHECK_U64(total, 0);
}

void test_cnmt(void)
{
    nexusTestRun("cnmt header fields",       t_cnmt_header);
    nexusTestRun("cnmt content entries",     t_cnmt_contents);
    nexusTestRun("cnmt 40-bit size field",   t_cnmt_40bit_size);
    nexusTestRun("cnmt find by type",        t_cnmt_find_by_type);
    nexusTestRun("cnmt total size",          t_cnmt_total_size);
    nexusTestRun("cnmt content id hex",      t_cnmt_content_id_hex);
    nexusTestRun("cnmt truncated",           t_cnmt_truncated);
    nexusTestRun("cnmt absurd count",        t_cnmt_absurd_content_count);
    nexusTestRun("cnmt ext header overrun",  t_cnmt_extended_header_overrun);
    nexusTestRun("cnmt with no contents",    t_cnmt_no_contents);
}
