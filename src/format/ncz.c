// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- NCZ (compressed NCA) header parsing.

#include <string.h>

#include "nexus/ncz.h"

static const char SECTION_MAGIC[NCZ_MAGIC_SIZE] = { 'N','C','Z','S','E','C','T','N' };
static const char BLOCK_MAGIC[NCZ_MAGIC_SIZE]   = { 'N','C','Z','B','L','O','C','K' };

// Field offsets within one 0x38-byte section entry.
#define SEC_OFFSET       0x00
#define SEC_SIZE         0x08
#define SEC_CRYPTO_TYPE  0x10
// 0x18 is padding
#define SEC_KEY          0x20
#define SEC_COUNTER      0x30

size_t nczHeaderSizeFor(const void *preamble, size_t len)
{
    if (preamble == NULL || len < NCZ_PREAMBLE_SIZE) return 0;

    if (memcmp(preamble, SECTION_MAGIC, NCZ_MAGIC_SIZE) != 0) return 0;

    const u64 count = nexusRdU64(preamble, NCZ_MAGIC_SIZE);
    if (count == 0 || count > NCZ_MAX_SECTIONS) return 0;

    return (size_t)(NCZ_PREAMBLE_SIZE + count * NCZ_SECTION_SIZE);
}

NexusFmtResult nczParseHeader(NczContext *ctx, const void *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) return NexusFmt_Truncated;
    if (len < NCZ_PREAMBLE_SIZE)    return NexusFmt_Truncated;

    memset(ctx, 0, sizeof(*ctx));

    if (memcmp(buf, SECTION_MAGIC, NCZ_MAGIC_SIZE) != 0) return NexusFmt_BadMagic;

    const u64 count = nexusRdU64(buf, NCZ_MAGIC_SIZE);
    if (count == 0)                return NexusFmt_BadMagic;
    if (count > NCZ_MAX_SECTIONS)  return NexusFmt_TooLarge;

    const size_t table = (size_t)(NCZ_PREAMBLE_SIZE + count * NCZ_SECTION_SIZE);
    if (len < table) return NexusFmt_Truncated;

    ctx->section_count = (u32)count;

    // Sections tile the NCA from the end of the copied-through header, not
    // from zero: the first 0x4000 bytes are never compressed, so nothing
    // describes them. A gap or overlap would leave bytes with no defined
    // crypto and produce an NCA that is silently wrong rather than obviously
    // broken, so the tiling is checked rather than assumed.
    u64 expected_offset = NCZ_HEADER_OFFSET;

    for (u32 i = 0; i < ctx->section_count; i++) {
        const u8 *p = (const u8 *)buf + NCZ_PREAMBLE_SIZE + (size_t)i * NCZ_SECTION_SIZE;
        NczSection *s = &ctx->sections[i];

        s->offset      = nexusRdU64(p, SEC_OFFSET);
        s->size        = nexusRdU64(p, SEC_SIZE);
        s->crypto_type = nexusRdU64(p, SEC_CRYPTO_TYPE);

        memcpy(s->key,     p + SEC_KEY,     NCZ_KEY_SIZE);
        memcpy(s->counter, p + SEC_COUNTER, NCZ_KEY_SIZE);

        if (s->offset != expected_offset) return NexusFmt_Unsupported;
        if (s->size == 0)                 return NexusFmt_Unsupported;

        u64 end = 0;
        if (nexusAddOverflows(s->offset, s->size, &end)) return NexusFmt_Overflow;
        expected_offset = end;

        // XTS only ever applies to the NCA header, which an NCZ copies through
        // verbatim rather than compressing. Seeing it on a body section means
        // this is not a layout we understand.
        if (s->crypto_type == NczCrypto_Xts) return NexusFmt_Unsupported;
    }

    ctx->decompressed_size = expected_offset;
    ctx->header_bytes      = table;

    // An optional NCZBLOCK header sits between the table and the stream. The
    // block variant is not one zstd frame but a list of independently
    // compressed blocks, so it is recognised and refused rather than fed to a
    // stream decoder that would produce garbage.
    if (len >= table + NCZ_MAGIC_SIZE
        && memcmp((const u8 *)buf + table, BLOCK_MAGIC, NCZ_MAGIC_SIZE) == 0) {
        ctx->block_compressed = true;
        return NexusFmt_Unsupported;
    }

    return NexusFmt_Ok;
}

const NczSection *nczSectionAt(const NczContext *ctx, u64 offset)
{
    if (ctx == NULL) return NULL;

    for (u32 i = 0; i < ctx->section_count; i++) {
        const NczSection *s = &ctx->sections[i];
        if (offset >= s->offset && offset < s->offset + s->size) return s;
    }
    return NULL;
}

bool nczSectionIsEncrypted(const NczSection *section)
{
    if (section == NULL) return false;

    // Bktr sections are AES-CTR at this level: the extra indirection applies
    // when the RomFS is read, not when the NCA is stored. nsz treats 3 and 4
    // identically here and so does this.
    return section->crypto_type == NczCrypto_Ctr
        || section->crypto_type == NczCrypto_Bktr;
}

void nczBuildCounter(const NczSection *section, u64 offset, u8 out[NCZ_KEY_SIZE])
{
    if (section == NULL || out == NULL) return;

    // Top half is the section's own counter used as a prefix; bottom half is
    // the AES block index, big-endian. AES blocks are 16 bytes, hence the
    // shift. This mirrors Counter.new(64, prefix=nonce[0:8], initial_value=
    // offset >> 4) in the reference decompressor.
    memcpy(out, section->counter, 8);

    const u64 block = offset >> 4;
    for (int i = 0; i < 8; i++) {
        out[8 + i] = (u8)((block >> (56 - 8 * i)) & 0xFF);
    }
}
