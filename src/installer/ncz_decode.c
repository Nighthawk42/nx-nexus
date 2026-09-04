// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- NCZ decompression, on the console.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "nexus/ncz_decode.h"
#include "nexus/log.h"

// The 0x4000 NCA header is copied through verbatim, and the section table sits
// right behind it, so both are buffered before anything is emitted.
#define PRELUDE_CAP  (NCZ_HEADER_OFFSET + NCZ_PREAMBLE_SIZE \
                      + NCZ_MAX_SECTIONS * NCZ_SECTION_SIZE + NCZ_MAGIC_SIZE)

#define OUT_CHUNK    (512u * 1024u)

typedef enum {
    Phase_Prelude = 0,   // buffering the NCA header and the section table
    Phase_Stream,        // feeding zstd
    Phase_Failed,
} Phase;

struct NexusNczDecoder {
    Phase phase;

    u8    *prelude;
    size_t prelude_have;
    size_t prelude_need;   // grows once the section count is known

    NczContext ncz;

    ZSTD_DStream *zds;
    u8           *out;     // decompressed, pre-encryption

    // Where the next decompressed byte belongs in the reconstructed NCA.
    u64 out_offset;

    // The section currently being re-encrypted, and its live cipher state.
    const NczSection *section;
    Aes128CtrContext  aes;
    bool              aes_ready;

    NexusNczSink sink;
    void        *sink_user;

    char problem[96];
};

// A single decoder is enough: the installer handles one entry at a time.
static NexusNczDecoder *g_active = NULL;

static void fail(NexusNczDecoder *d, const char *why)
{
    d->phase = Phase_Failed;
    snprintf(d->problem, sizeof(d->problem), "%.90s", why);
    LOG_E("ncz: %s", why);
}

NexusNczDecoder *nexusNczDecoderCreate(void)
{
    NexusNczDecoder *d = (NexusNczDecoder *)calloc(1, sizeof(*d));
    if (d == NULL) return NULL;

    d->prelude = (u8 *)malloc(PRELUDE_CAP);
    d->out     = (u8 *)malloc(OUT_CHUNK);
    d->zds     = ZSTD_createDStream();

    if (d->prelude == NULL || d->out == NULL || d->zds == NULL) {
        nexusNczDecoderDestroy(d);
        return NULL;
    }

    return d;
}

void nexusNczDecoderDestroy(NexusNczDecoder *d)
{
    if (d == NULL) return;

    if (d->zds != NULL) ZSTD_freeDStream(d->zds);
    free(d->prelude);
    free(d->out);

    if (g_active == d) g_active = NULL;
    free(d);
}

const char *nexusNczDecoderProblem(const NexusNczDecoder *d)
{
    return (d != NULL) ? d->problem : "";
}

// ---------------------------------------------------------------------------
// Re-encryption
// ---------------------------------------------------------------------------

// Points the cipher at the section covering out_offset, rebuilding the counter
// for that exact position. Called at every section boundary; within a section
// the counter advances on its own.
static bool select_section(NexusNczDecoder *d)
{
    d->section = nczSectionAt(&d->ncz, d->out_offset);
    if (d->section == NULL) {
        fail(d, "decompressed past the end of the described sections");
        return false;
    }

    if (nczSectionIsEncrypted(d->section)) {
        u8 counter[NCZ_KEY_SIZE];
        nczBuildCounter(d->section, d->out_offset, counter);
        aes128CtrContextCreate(&d->aes, d->section->key, counter);
        d->aes_ready = true;
    } else {
        d->aes_ready = false;
    }

    return true;
}

// Emits `len` decompressed bytes, re-encrypting per section. A run may span a
// section boundary, so it is cut at each one: the key changes there.
static bool emit(NexusNczDecoder *d, u8 *data, size_t len)
{
    while (len > 0) {
        if (d->section == NULL
            || d->out_offset >= d->section->offset + d->section->size) {
            if (!select_section(d)) return false;
        }

        const u64 section_end = d->section->offset + d->section->size;
        const u64 room        = section_end - d->out_offset;
        const size_t take     = (len < room) ? len : (size_t)room;

        if (d->aes_ready) {
            // In place: the buffer is ours and the encrypted bytes are what
            // goes to the sink.
            aes128CtrCrypt(&d->aes, data, data, take);
        }

        if (d->sink(d->sink_user, data, take) != 0) {
            fail(d, "the installer rejected the rebuilt data");
            return false;
        }

        data          += take;
        len           -= take;
        d->out_offset += take;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Prelude: the NCA header and the section table
// ---------------------------------------------------------------------------

// Consumes bytes into the prelude buffer until the section table is complete,
// then emits the copied-through NCA header. Returns how many bytes were taken,
// or -1 on failure.
static ssize_t absorb_prelude(NexusNczDecoder *d, const u8 *data, size_t len)
{
    const size_t want = (d->prelude_need > 0) ? d->prelude_need : PRELUDE_CAP;
    const size_t room = want - d->prelude_have;
    const size_t take = (len < room) ? len : room;

    memcpy(d->prelude + d->prelude_have, data, take);
    d->prelude_have += take;

    // Size the table as soon as the magic and count have landed.
    if (d->prelude_need == 0
        && d->prelude_have >= NCZ_HEADER_OFFSET + NCZ_PREAMBLE_SIZE) {
        const size_t table = nczHeaderSizeFor(d->prelude + NCZ_HEADER_OFFSET,
                                              d->prelude_have - NCZ_HEADER_OFFSET);
        if (table == 0) {
            fail(d, "not an NCZ: no NCZSECTN header");
            return -1;
        }
        // One extra magic so a block-compressed file can be recognised.
        d->prelude_need = NCZ_HEADER_OFFSET + table + NCZ_MAGIC_SIZE;
    }

    if (d->prelude_need == 0 || d->prelude_have < d->prelude_need) {
        return (ssize_t)take;   // still filling
    }

    const NexusFmtResult r = nczParseHeader(&d->ncz,
                                            d->prelude + NCZ_HEADER_OFFSET,
                                            d->prelude_have - NCZ_HEADER_OFFSET);
    if (r != NexusFmt_Ok) {
        if (d->ncz.block_compressed) {
            fail(d, "block-compressed NCZ is not supported yet");
        } else {
            fail(d, nexusFmtStr(r));
        }
        return -1;
    }

    LOG_I("ncz: %u section(s), rebuilding %llu bytes",
          d->ncz.section_count, (unsigned long long)d->ncz.decompressed_size);

    // The NCA header goes out untouched -- it was never decrypted, so it is
    // not re-encrypted either. It also has no section describing it, which is
    // why it bypasses emit().
    if (d->sink(d->sink_user, d->prelude, NCZ_HEADER_OFFSET) != 0) {
        fail(d, "the installer rejected the NCA header");
        return -1;
    }
    d->out_offset = NCZ_HEADER_OFFSET;

    // Everything buffered past the table is already zstd payload, so it is
    // pushed back through the stream path below.
    const size_t table_end = NCZ_HEADER_OFFSET + d->ncz.header_bytes;
    const size_t leftover  = d->prelude_have - table_end;

    d->phase = Phase_Stream;
    ZSTD_initDStream(d->zds);

    if (leftover > 0) {
        ZSTD_inBuffer in = { d->prelude + table_end, leftover, 0 };
        while (in.pos < in.size) {
            ZSTD_outBuffer out = { d->out, OUT_CHUNK, 0 };
            const size_t rc = ZSTD_decompressStream(d->zds, &out, &in);
            if (ZSTD_isError(rc)) {
                fail(d, ZSTD_getErrorName(rc));
                return -1;
            }
            if (out.pos > 0 && !emit(d, d->out, out.pos)) return -1;
            if (rc == 0 && in.pos >= in.size) break;
        }
    }

    return (ssize_t)take;
}

// ---------------------------------------------------------------------------
// Ops
// ---------------------------------------------------------------------------

static int ncz_begin(void *user, NexusNczSink sink, void *sink_user)
{
    NexusNczDecoder *d = (NexusNczDecoder *)user;
    if (d == NULL) return -1;

    d->phase        = Phase_Prelude;
    d->prelude_have = 0;
    d->prelude_need = 0;
    d->out_offset   = 0;
    d->section      = NULL;
    d->aes_ready    = false;
    d->sink         = sink;
    d->sink_user    = sink_user;
    d->problem[0]   = '\0';
    memset(&d->ncz, 0, sizeof(d->ncz));

    g_active = d;
    return 0;
}

static int ncz_feed(void *user, const void *data, size_t len)
{
    NexusNczDecoder *d = (NexusNczDecoder *)user;
    if (d == NULL || d->phase == Phase_Failed) return -1;

    const u8 *p = (const u8 *)data;

    while (len > 0) {
        if (d->phase == Phase_Prelude) {
            const ssize_t used = absorb_prelude(d, p, len);
            if (used < 0) return -1;
            p   += (size_t)used;
            len -= (size_t)used;
            continue;
        }

        ZSTD_inBuffer in = { p, len, 0 };
        while (in.pos < in.size) {
            ZSTD_outBuffer out = { d->out, OUT_CHUNK, 0 };
            const size_t rc = ZSTD_decompressStream(d->zds, &out, &in);
            if (ZSTD_isError(rc)) {
                fail(d, ZSTD_getErrorName(rc));
                return -1;
            }
            if (out.pos > 0 && !emit(d, d->out, out.pos)) return -1;

            // A finished frame with input left means trailing bytes, which is
            // normal: the container pads entries. Stop rather than restart.
            if (rc == 0 && out.pos == 0) break;
        }

        p   += in.size;
        len -= in.size;
    }

    return 0;
}

static int ncz_end(void *user)
{
    NexusNczDecoder *d = (NexusNczDecoder *)user;
    if (d == NULL || d->phase == Phase_Failed) return -1;

    // Anything short of the full NCA means a truncated or corrupt NCZ, and
    // registering a short NCA would produce a title that fails much later with
    // no clue why.
    if (d->out_offset != d->ncz.decompressed_size) {
        char why[96];
        snprintf(why, sizeof(why), "rebuilt %llu of %llu bytes",
                 (unsigned long long)d->out_offset,
                 (unsigned long long)d->ncz.decompressed_size);
        fail(d, why);
        return -1;
    }

    LOG_I("ncz: rebuilt %llu bytes", (unsigned long long)d->out_offset);
    return 0;
}

static u64 ncz_size(void *user)
{
    NexusNczDecoder *d = (NexusNczDecoder *)user;
    return (d != NULL) ? d->ncz.decompressed_size : 0;
}

static const NexusNczOps g_ncz_ops = {
    .begin = ncz_begin,
    .feed  = ncz_feed,
    .end   = ncz_end,
    .size  = ncz_size,
};

const NexusNczOps *nexusNczDecoderOps(void)
{
    return &g_ncz_ops;
}
