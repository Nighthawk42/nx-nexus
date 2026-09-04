// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- XCI header tests.
//
// The XCI header is the one place where a wrong constant produces a plausible
// but useless result: a slightly wrong root offset finds no partition table
// and looks exactly like "this is not an XCI". So the field offsets are pinned
// here against a hand-built header rather than trusted.

#include <string.h>

#include "nexus_test.h"
#include "nexus/xci.h"
#include "fixtures.h"

#define MEDIA(x) ((u32)((x) / XCI_MEDIA_UNIT))

// Writes a minimal but structurally valid gamecard header.
static void build_header(u8 *buf, u64 root_offset, u64 root_header_size,
                         u8 rom_size, u32 valid_data_end_pages)
{
    memset(buf, 0, XCI_HEADER_SIZE);

    // "HEAD" at 0x100, after the signature.
    buf[0x100] = 'H'; buf[0x101] = 'E'; buf[0x102] = 'A'; buf[0x103] = 'D';

    buf[0x10D] = rom_size;
    buf[0x10E] = 0x02;   // header version
    buf[0x10F] = 0x00;   // flags

    memcpy(buf + 0x118, &valid_data_end_pages, sizeof(valid_data_end_pages));
    memcpy(buf + 0x130, &root_offset, sizeof(root_offset));
    memcpy(buf + 0x138, &root_header_size, sizeof(root_header_size));
}

static void test_parses_a_real_shaped_header(void)
{
    u8 buf[XCI_HEADER_SIZE];
    // 0xF000 is where the root partition actually sits on retail cards.
    build_header(buf, 0xF000, 0x200, 0xE2 /* 32 GB */, MEDIA(0x40000000));

    XciHeader hdr;
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_Ok);

    CHECK_U64(hdr.root_offset, 0xF000);
    CHECK_U64(hdr.root_header_size, 0x200);
    CHECK_U64(hdr.cart_size, 32ull * 1024 * 1024 * 1024);
    CHECK_U64(hdr.header_version, 2);

    // valid_data_end counts pages inclusively, so it is one page past the
    // recorded index. A trimmed dump is exactly this many bytes long.
    CHECK_U64(hdr.valid_data_end, 0x40000000 + XCI_MEDIA_UNIT);

    CHECK_STR(xciCartSizeStr(hdr.cart_size), "32 GB");
}

static void test_rejects_bad_input(void)
{
    u8 buf[XCI_HEADER_SIZE];
    XciHeader hdr;

    build_header(buf, 0xF000, 0x200, 0xE2, 0);
    CHECK_FMT(xciParseHeader(buf, XCI_HEADER_SIZE - 1, &hdr), NexusFmt_Truncated);
    CHECK_FMT(xciParseHeader(NULL, sizeof(buf), &hdr), NexusFmt_Truncated);

    // Wrong magic: an NSP handed to the XCI path, for instance.
    build_header(buf, 0xF000, 0x200, 0xE2, 0);
    buf[0x100] = 'P';
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_BadMagic);

    // A root partition inside the header itself cannot be real.
    build_header(buf, 0x10, 0x200, 0xE2, 0);
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_Truncated);

    // A header smaller than the fixed part, and one absurdly large.
    build_header(buf, 0xF000, 0x8, 0xE2, 0);
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_TooLarge);

    build_header(buf, 0xF000, 0x40000000, 0xE2, 0);
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_TooLarge);

    // A root offset near the top of the range must not wrap when the header
    // size is added to it.
    build_header(buf, 0xFFFFFFFFFFFFFF00ull, 0x200, 0xE2, 0);
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_Overflow);
}

static void test_unknown_cart_size_is_not_a_failure(void)
{
    u8 buf[XCI_HEADER_SIZE];
    build_header(buf, 0xF000, 0x200, 0x77 /* not a real capacity */, 0);

    XciHeader hdr;
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_Ok);

    // An unrecognised capacity byte says nothing about whether the image is
    // installable, so it must not reject the header.
    CHECK_U64(hdr.cart_size, 0);
    CHECK_STR(xciCartSizeStr(hdr.cart_size), "unknown");
}

// The partition lookup is where an off-by-one costs the most: the secure
// partition's absolute position is root_offset + root header size + entry
// offset, and getting it wrong yields garbage that only fails much later.
static void test_finds_the_secure_partition(void)
{
    static const char *const names[] = { "update", "normal", "secure" };
    const u64 sizes[] = { 0x1000, 0x2000, 0x300000 };

    u8 root_header[FIXTURE_BUF_SIZE];
    const size_t header_len = fixtureBuildPartitionFs(root_header, sizeof(root_header),
                                                      PartitionFsType_Hfs0,
                                                      names, sizes, 3);
    CHECK(header_len > 0, "fixture build failed");

    PartitionFsContext root;
    CHECK_FMT(partitionFsInit(&root, root_header, header_len), NexusFmt_Ok);

    u8 buf[XCI_HEADER_SIZE];
    build_header(buf, 0xF000, header_len, 0xE2, 0);

    XciHeader hdr;
    CHECK_FMT(xciParseHeader(buf, sizeof(buf), &hdr), NexusFmt_Ok);

    u64 offset = 0, size = 0;
    CHECK_FMT(xciFindPartition(&hdr, &root, "secure", &offset, &size), NexusFmt_Ok);

    // update and normal come first, so secure begins after both of them.
    const u64 expected = 0xF000 + partitionFsGetDataOffset(&root) + 0x1000 + 0x2000;
    CHECK_U64(offset, expected);
    CHECK_U64(size, 0x300000);

    // A partition that is not there must say so rather than return zero.
    CHECK_FMT(xciFindPartition(&hdr, &root, "logo", &offset, &size), NexusFmt_NotFound);
}

void test_xci(void)
{
    nexusTestRun("parses a retail-shaped header", test_parses_a_real_shaped_header);
    nexusTestRun("rejects malformed headers", test_rejects_bad_input);
    nexusTestRun("unknown capacity is tolerated", test_unknown_cart_size_is_not_a_failure);
    nexusTestRun("locates the secure partition", test_finds_the_secure_partition);
}
