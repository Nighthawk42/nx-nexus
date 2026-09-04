// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- ticket parser tests.

#include <string.h>

#include "nexus_test.h"
#include "fixtures.h"

#define TEST_TITLE_ID 0x0100000000010000ull

static void t_sig_data_sizes(void)
{
    // Sizes are 4 (type) + signature + padding, per the switchbrew table.
    // Getting any of these wrong shifts the whole ticket data section.
    const struct { u32 type; size_t expect; } cases[] = {
        { TicketSigType_Rsa4096Sha1,   4 + 0x200 + 0x3C },
        { TicketSigType_Rsa2048Sha1,   4 + 0x100 + 0x3C },
        { TicketSigType_EcdsaSha1,     4 + 0x3C  + 0x40 },
        { TicketSigType_Rsa4096Sha256, 4 + 0x200 + 0x3C },
        { TicketSigType_Rsa2048Sha256, 4 + 0x100 + 0x3C },
        { TicketSigType_EcdsaSha256,   4 + 0x3C  + 0x40 },
        { TicketSigType_HmacSha1160,   4 + 0x14  + 0x28 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t got = 0;
        CHECK_FMT(ticketGetSigDataSize(cases[i].type, &got), NexusFmt_Ok);
        CHECK_U64(got, cases[i].expect);

        // Every signature-data section must land on a 0x40 boundary.
        CHECK_U64(got % 0x40, 0);
    }

    size_t got = 0;
    CHECK_FMT(ticketGetSigDataSize(0xDEADBEEF, &got), NexusFmt_Unsupported);
}

static void t_ticket_roundtrip(void)
{
    // RSA-2048 SHA-256 is what retail tickets actually use.
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildTicket(buf, sizeof(buf), TicketSigType_Rsa2048Sha256,
                                        TicketKeyType_Common, TEST_TITLE_ID, 0x0A);
    CHECK(n > 0, "fixture build failed");

    TicketContext ctx;
    CHECK_FMT(ticketInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(ctx.sig_type, TicketSigType_Rsa2048Sha256);
    CHECK_U64(ctx.sig_data_size, 0x140);
    CHECK_U64(ctx.total_size, 0x140 + TICKET_DATA_SIZE);
    CHECK_U64(ctx.format_version, 2);
    CHECK_U64(ctx.key_type, TicketKeyType_Common);
    CHECK_U64(ctx.master_key_revision, 0x0A);
    CHECK_U64(ctx.ticket_id, 0x1234567890ABCDEFull);
    CHECK(ticketIsCommon(&ctx), "expected a common ticket");
    CHECK_STR(ticketSigTypeStr(ctx.sig_type), "RSA-2048 SHA-256");
}

static void t_rights_id_is_big_endian(void)
{
    // The rights id is the one big-endian field in these formats, so a title
    // id extracted with the little-endian readers would come out byte-reversed.
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildTicket(buf, sizeof(buf), TicketSigType_Rsa2048Sha256,
                                        TicketKeyType_Common, TEST_TITLE_ID, 0x0A);
    TicketContext ctx;
    CHECK_FMT(ticketInit(&ctx, buf, n), NexusFmt_Ok);

    CHECK_U64(ticketGetTitleId(&ctx), TEST_TITLE_ID);
    CHECK_U64(ticketGetKeyGeneration(&ctx), 0x0A);

    char hex[33];
    ticketFormatRightsId(&ctx, hex, sizeof(hex));
    CHECK_STR(hex, "010000000001000000000000000000" "0a");
}

static void t_personalised_ticket(void)
{
    // Personalised tickets are bound to the console that bought the content.
    // Detecting this matters: installing one elsewhere produces a title that
    // cannot launch, and the user should be warned before a long transfer.
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildTicket(buf, sizeof(buf), TicketSigType_Rsa2048Sha256,
                                        TicketKeyType_Personalised, TEST_TITLE_ID, 0);
    TicketContext ctx;
    CHECK_FMT(ticketInit(&ctx, buf, n), NexusFmt_Ok);
    CHECK_U64(ctx.key_type, TicketKeyType_Personalised);
    CHECK(!ticketIsCommon(&ctx), "expected a personalised ticket");
}

static void t_all_sig_types_parse(void)
{
    // Each signature type shifts the ticket data to a different offset; the
    // parser must find the same rights id regardless.
    const u32 types[] = {
        TicketSigType_Rsa4096Sha1,   TicketSigType_Rsa2048Sha1,
        TicketSigType_EcdsaSha1,     TicketSigType_Rsa4096Sha256,
        TicketSigType_Rsa2048Sha256, TicketSigType_EcdsaSha256,
        TicketSigType_HmacSha1160,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        u8 buf[FIXTURE_BUF_SIZE];
        const size_t n = fixtureBuildTicket(buf, sizeof(buf), types[i],
                                            TicketKeyType_Common, TEST_TITLE_ID, 3);
        CHECK(n > 0, "fixture build failed for sig type 0x%06x", types[i]);

        TicketContext ctx;
        CHECK_FMT(ticketInit(&ctx, buf, n), NexusFmt_Ok);
        CHECK_U64(ticketGetTitleId(&ctx), TEST_TITLE_ID);
        CHECK_U64(ticketGetKeyGeneration(&ctx), 3);
        CHECK_U64(ctx.format_version, 2);
    }
}

static void t_ticket_truncated(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    const size_t n = fixtureBuildTicket(buf, sizeof(buf), TicketSigType_Rsa2048Sha256,
                                        TicketKeyType_Common, TEST_TITLE_ID, 0);
    TicketContext ctx;
    CHECK_FMT(ticketInit(&ctx, buf, n - 1), NexusFmt_Truncated);
    CHECK_FMT(ticketInit(&ctx, buf, 0x140), NexusFmt_Truncated);
    CHECK_FMT(ticketInit(&ctx, buf, 2), NexusFmt_Truncated);
    CHECK_FMT(ticketInit(&ctx, buf, 0), NexusFmt_Truncated);
}

static void t_ticket_bad_sig_type(void)
{
    u8 buf[FIXTURE_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    // Signature type 0 is not in the table.
    TicketContext ctx;
    CHECK_FMT(ticketInit(&ctx, buf, sizeof(buf)), NexusFmt_Unsupported);
}

void test_ticket(void)
{
    nexusTestRun("signature data sizes",   t_sig_data_sizes);
    nexusTestRun("ticket roundtrip",       t_ticket_roundtrip);
    nexusTestRun("rights id is big-endian", t_rights_id_is_big_endian);
    nexusTestRun("personalised ticket",    t_personalised_ticket);
    nexusTestRun("all signature types",    t_all_sig_types_parse);
    nexusTestRun("ticket truncated",       t_ticket_truncated);
    nexusTestRun("ticket bad sig type",    t_ticket_bad_sig_type);
}
