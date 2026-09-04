// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- NCZ header tests.
//
// These pin the layout against nsz's own reference decompressor, because every
// mistake here is silent: a section entry read at the wrong stride still parses
// into plausible-looking numbers, and the NCA it rebuilds is corrupt in a way
// that only shows up when the game refuses to boot.

#include <string.h>

#include "nexus_test.h"
#include "nexus/ncz.h"

#define SEC(n) (NCZ_PREAMBLE_SIZE + (n) * NCZ_SECTION_SIZE)

static void put64(u8 *p, size_t off, u64 v)
{
    for (int i = 0; i < 8; i++) p[off + i] = (u8)((v >> (8 * i)) & 0xFF);
}

// Builds a section table as it appears from NCZ_HEADER_OFFSET onwards.
static size_t build(u8 *buf, size_t cap, const u64 *sizes, const u64 *types, u32 count)
{
    const size_t total = SEC(count);
    if (cap < total) return 0;

    memset(buf, 0, total);
    memcpy(buf, "NCZSECTN", 8);
    put64(buf, 8, count);

    u64 offset = NCZ_HEADER_OFFSET;
    for (u32 i = 0; i < count; i++) {
        u8 *s = buf + SEC(i);

        put64(s, 0x00, offset);
        put64(s, 0x08, sizes[i]);
        put64(s, 0x10, types[i]);

        // Key and counter are distinguishable per section so a stride error
        // shows up as the wrong bytes rather than as zeroes.
        memset(s + 0x20, (int)(0xA0 + i), NCZ_KEY_SIZE);
        memset(s + 0x30, (int)(0xC0 + i), NCZ_KEY_SIZE);

        offset += sizes[i];
    }

    return total;
}

static void test_parses_a_two_section_table(void)
{
    const u64 sizes[] = { 0x20000, 0x100000 };
    const u64 types[] = { NczCrypto_None, NczCrypto_Ctr };

    u8 buf[512];
    const size_t len = build(buf, sizeof(buf), sizes, types, 2);
    CHECK(len > 0, "fixture too small");

    NczContext ctx;
    CHECK_FMT(nczParseHeader(&ctx, buf, len), NexusFmt_Ok);

    CHECK_U64(ctx.section_count, 2);
    CHECK_U64(ctx.header_bytes, SEC(2));

    // Sections start after the copied-through NCA header, not at zero.
    CHECK_U64(ctx.sections[0].offset, NCZ_HEADER_OFFSET);
    CHECK_U64(ctx.sections[1].offset, NCZ_HEADER_OFFSET + 0x20000);

    // The rebuilt NCA is the header plus everything the sections describe.
    CHECK_U64(ctx.decompressed_size, NCZ_HEADER_OFFSET + 0x20000 + 0x100000);

    CHECK_U64(ctx.sections[0].key[0], 0xA0);
    CHECK_U64(ctx.sections[1].key[0], 0xA1);
    CHECK_U64(ctx.sections[1].counter[0], 0xC1);

    CHECK(!nczSectionIsEncrypted(&ctx.sections[0]), "type 1 must be copied through");
    CHECK(nczSectionIsEncrypted(&ctx.sections[1]), "type 3 must be re-encrypted");
}

static void test_header_sizing(void)
{
    const u64 sizes[] = { 0x1000, 0x1000, 0x1000 };
    const u64 types[] = { NczCrypto_Ctr, NczCrypto_Ctr, NczCrypto_Bktr };

    u8 buf[512];
    build(buf, sizeof(buf), sizes, types, 3);

    // The caller streams, so it must be able to size the table from the first
    // sixteen bytes alone.
    CHECK_U64(nczHeaderSizeFor(buf, NCZ_PREAMBLE_SIZE), SEC(3));
    CHECK_U64(nczHeaderSizeFor(buf, NCZ_PREAMBLE_SIZE - 1), 0);

    u8 wrong[NCZ_PREAMBLE_SIZE];
    memcpy(wrong, buf, sizeof(wrong));
    wrong[0] = 'X';
    CHECK_U64(nczHeaderSizeFor(wrong, sizeof(wrong)), 0);

    // Bktr is AES-CTR at this level, exactly as nsz treats it.
    NczContext ctx;
    CHECK_FMT(nczParseHeader(&ctx, buf, SEC(3)), NexusFmt_Ok);
    CHECK(nczSectionIsEncrypted(&ctx.sections[2]), "type 4 must be re-encrypted");
}

static void test_rejects_bad_tables(void)
{
    const u64 sizes[] = { 0x1000, 0x1000 };
    const u64 types[] = { NczCrypto_Ctr, NczCrypto_Ctr };

    u8 buf[512];
    const size_t len = build(buf, sizeof(buf), sizes, types, 2);

    NczContext ctx;

    // A plain NCA fed to the NCZ path.
    u8 plain[NCZ_PREAMBLE_SIZE];
    memset(plain, 0, sizeof(plain));
    CHECK_FMT(nczParseHeader(&ctx, plain, sizeof(plain)), NexusFmt_BadMagic);

    // Still streaming.
    CHECK_FMT(nczParseHeader(&ctx, buf, SEC(1)), NexusFmt_Truncated);
    CHECK_FMT(nczParseHeader(&ctx, buf, 4), NexusFmt_Truncated);

    // Absurd section count.
    u8 many[512];
    memcpy(many, buf, sizeof(many));
    put64(many, 8, NCZ_MAX_SECTIONS + 1);
    CHECK_FMT(nczParseHeader(&ctx, many, len), NexusFmt_TooLarge);

    // A gap between sections would leave bytes with no defined crypto.
    u8 gap[512];
    memcpy(gap, buf, len);
    put64(gap, SEC(1) + 0x00, NCZ_HEADER_OFFSET + 0x1000 + 0x10);
    CHECK_FMT(nczParseHeader(&ctx, gap, len), NexusFmt_Unsupported);

    // The first section must begin exactly where the copied header ends.
    u8 shifted[512];
    memcpy(shifted, buf, len);
    put64(shifted, SEC(0) + 0x00, 0);
    CHECK_FMT(nczParseHeader(&ctx, shifted, len), NexusFmt_Unsupported);

    // XTS never applies to a body section.
    u8 xts[512];
    memcpy(xts, buf, len);
    put64(xts, SEC(0) + 0x10, NczCrypto_Xts);
    CHECK_FMT(nczParseHeader(&ctx, xts, len), NexusFmt_Unsupported);

    // A size that would wrap the address space.
    u8 huge[512];
    memcpy(huge, buf, len);
    put64(huge, SEC(0) + 0x08, 0xFFFFFFFFFFFFFFFFull);
    CHECK_FMT(nczParseHeader(&ctx, huge, len), NexusFmt_Overflow);
}

// The block-compressed variant is a list of independently compressed blocks,
// not one zstd frame. Feeding it to a stream decoder yields garbage, so it has
// to be recognised and refused rather than half-decoded.
static void test_block_compressed_is_refused(void)
{
    const u64 sizes[] = { 0x1000 };
    const u64 types[] = { NczCrypto_Ctr };

    u8 buf[512];
    const size_t len = build(buf, sizeof(buf), sizes, types, 1);
    memcpy(buf + len, "NCZBLOCK", 8);

    NczContext ctx;
    CHECK_FMT(nczParseHeader(&ctx, buf, len + 8), NexusFmt_Unsupported);
    CHECK(ctx.block_compressed, "block compression must be reported, not just refused");
}

// The counter is the one piece of arithmetic that cannot be checked against
// anything else at runtime: get the endianness or the shift wrong and the NCA
// decrypts to noise with no error anywhere.
static void test_counter_construction(void)
{
    NczSection s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 8; i++) s.counter[i] = (u8)(0x10 + i);

    u8 ctr[NCZ_KEY_SIZE];

    nczBuildCounter(&s, 0, ctr);
    CHECK(memcmp(ctr, s.counter, 8) == 0, "nonce prefix must be copied verbatim");
    for (int i = 8; i < 16; i++) CHECK_U64(ctr[i], 0);

    // Offset 0x4000 is AES block 0x400, big-endian in the low eight bytes.
    nczBuildCounter(&s, 0x4000, ctr);
    CHECK_U64(ctr[8],  0);
    CHECK_U64(ctr[13], 0x00);
    CHECK_U64(ctr[14], 0x04);
    CHECK_U64(ctr[15], 0x00);

    // One AES block in.
    nczBuildCounter(&s, 0x10, ctr);
    CHECK_U64(ctr[15], 1);

    // Sub-block offsets share a counter block, which is what makes chunked
    // encryption at 16-byte-aligned boundaries equivalent to one pass.
    nczBuildCounter(&s, 0x1F, ctr);
    CHECK_U64(ctr[15], 1);

    // A 4 GiB offset is AES block 0x1000_0000, which big-endian across the low
    // eight bytes puts 0x10 at index 12.
    nczBuildCounter(&s, 0x100000000ull, ctr);
    CHECK_U64(ctr[12], 0x10);
    CHECK_U64(ctr[11], 0x00);
    CHECK_U64(ctr[15], 0x00);
}

static void test_section_lookup(void)
{
    const u64 sizes[] = { 0x1000, 0x2000 };
    const u64 types[] = { NczCrypto_None, NczCrypto_Ctr };

    u8 buf[512];
    const size_t len = build(buf, sizeof(buf), sizes, types, 2);

    NczContext ctx;
    CHECK_FMT(nczParseHeader(&ctx, buf, len), NexusFmt_Ok);

    CHECK(nczSectionAt(&ctx, NCZ_HEADER_OFFSET) == &ctx.sections[0], "first section");
    CHECK(nczSectionAt(&ctx, NCZ_HEADER_OFFSET + 0xFFF) == &ctx.sections[0], "last byte of it");
    CHECK(nczSectionAt(&ctx, NCZ_HEADER_OFFSET + 0x1000) == &ctx.sections[1], "boundary");
    CHECK(nczSectionAt(&ctx, ctx.decompressed_size) == NULL, "past the end");

    // Nothing describes the copied-through header, so a lookup inside it must
    // not silently return the first section.
    CHECK(nczSectionAt(&ctx, 0) == NULL, "the NCA header has no section");
}

void test_ncz(void)
{
    nexusTestRun("parses a two-section table", test_parses_a_two_section_table);
    nexusTestRun("sizes the header while streaming", test_header_sizing);
    nexusTestRun("rejects malformed tables", test_rejects_bad_tables);
    nexusTestRun("refuses block compression", test_block_compressed_is_refused);
    nexusTestRun("builds the CTR counter", test_counter_construction);
    nexusTestRun("maps offsets to sections", test_section_lookup);
}
