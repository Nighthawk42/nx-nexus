// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install a title straight from an XCI or an inserted gamecard.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/xci_install.h"
#include "nexus/install_horizon.h"
#include "nexus/partition_fs.h"
#include "nexus/nsp_builder.h"
#include "nexus/fs_ext.h"
#include "nexus/ncz_decode.h"
#include "nexus/log.h"

// Both plain and compressed NCAs live in the secure partition; an XCZ is an
// XCI whose contents are NCZs.
static bool is_content_name(const char *name)
{
    const size_t n = strlen(name);
    return (n > 4 && strcmp(name + (n - 4), ".nca") == 0)
        || (n > 4 && strcmp(name + (n - 4), ".ncz") == 0);
}

#define SECURE_PARTITION   "secure"
#define HEADER_CAP         (64u * 1024u)
#define STREAM_CHUNK       (1024u * 1024u)

static NexusXciState  g_state;
static volatile bool  g_cancel = false;

// ---------------------------------------------------------------------------
// Byte sources
//
// A file on the SD card and a raw gamecard both reduce to "read len bytes at
// offset", so the rest of this file does not care which it is talking to.
// ---------------------------------------------------------------------------

typedef struct {
    FILE      *file;      // set for an XCI on disk
    FsStorage  storage;   // set for a gamecard
    bool       is_card;
    bool       open;
} Source;

static bool source_open_file(Source *s, const char *path)
{
    memset(s, 0, sizeof(*s));

    s->file = fopen(path, "rb");
    if (s->file == NULL) {
        LOG_E("xci: cannot open %s", path);
        return false;
    }

    s->open = true;
    return true;
}

static bool source_open_card(Source *s)
{
    memset(s, 0, sizeof(*s));

    FsDeviceOperator op;
    if (R_FAILED(fsOpenDeviceOperator(&op))) {
        LOG_E("xci: fsOpenDeviceOperator failed");
        return false;
    }

    bool inserted = false;
    if (R_FAILED(fsDeviceOperatorIsGameCardInserted(&op, &inserted)) || !inserted) {
        fsDeviceOperatorClose(&op);
        LOG_W("xci: no gamecard inserted");
        return false;
    }

    FsGameCardHandle handle;
    const Result rc = fsDeviceOperatorGetGameCardHandle(&op, &handle);
    fsDeviceOperatorClose(&op);

    if (R_FAILED(rc)) {
        LOG_E("xci: GetGameCardHandle failed (0x%x)", rc);
        return false;
    }

    if (R_FAILED(nexusFsOpenGameCardStorage(&s->storage, &handle, NexusGcStorage_Full))) {
        LOG_E("xci: OpenGameCardStorage failed");
        return false;
    }

    s->is_card = true;
    s->open    = true;
    return true;
}

static void source_close(Source *s)
{
    if (!s->open) return;

    if (s->is_card) fsStorageClose(&s->storage);
    else if (s->file != NULL) fclose(s->file);

    s->open = false;
}

static bool source_read(Source *s, u64 offset, void *out, size_t len)
{
    if (!s->open || len == 0) return false;

    if (s->is_card) {
        return R_SUCCEEDED(fsStorageRead(&s->storage, (s64)offset, out, len));
    }

    if (fseeko(s->file, (off_t)offset, SEEK_SET) != 0) return false;
    return fread(out, 1, len, s->file) == len;
}

// ---------------------------------------------------------------------------
// Walking the container
// ---------------------------------------------------------------------------

// Everything needed to stream the secure partition, resolved once.
typedef struct {
    XciHeader hdr;

    u8 *secure_header;       // owned
    PartitionFsContext secure;
    u64 secure_offset;       // absolute, of the partition itself
    u64 data_offset;         // absolute, where its file data begins
} Layout;

static void layout_free(Layout *l)
{
    free(l->secure_header);
    l->secure_header = NULL;
}

// Reads a PFS0/HFS0 header at an absolute offset into a freshly allocated
// buffer. Two-stage, because the header length is not known until the first
// 16 bytes have been read.
static u8 *read_partition_header(Source *src, u64 offset, PartitionFsContext *ctx,
                                 const char *what)
{
    u8 prefix[PARTITION_FS_HEADER_SIZE];
    if (!source_read(src, offset, prefix, sizeof(prefix))) {
        LOG_E("xci: cannot read the %s header", what);
        return NULL;
    }

    PartitionFsType type;
    u64 header_size = 0;
    NexusFmtResult r = partitionFsPeekHeaderSize(prefix, sizeof(prefix), &type, &header_size);
    if (r != NexusFmt_Ok) {
        LOG_E("xci: %s is not a partition fs -- %s", what, nexusFmtStr(r));
        return NULL;
    }

    if (header_size > HEADER_CAP) {
        LOG_E("xci: %s header is %llu bytes, cap is %u",
              what, (unsigned long long)header_size, HEADER_CAP);
        return NULL;
    }

    u8 *buf = (u8 *)malloc((size_t)header_size);
    if (buf == NULL) return NULL;

    if (!source_read(src, offset, buf, (size_t)header_size)) {
        LOG_E("xci: cannot read the whole %s header", what);
        free(buf);
        return NULL;
    }

    r = partitionFsInit(ctx, buf, (size_t)header_size);
    if (r != NexusFmt_Ok) {
        LOG_E("xci: %s header is malformed -- %s", what, nexusFmtStr(r));
        free(buf);
        return NULL;
    }

    return buf;
}

static bool resolve_layout(Source *src, Layout *l, char *problem, size_t problem_size)
{
    memset(l, 0, sizeof(*l));

    u8 header[XCI_HEADER_SIZE];
    if (!source_read(src, 0, header, sizeof(header))) {
        snprintf(problem, problem_size, "cannot read the gamecard header");
        return false;
    }

    NexusFmtResult r = xciParseHeader(header, sizeof(header), &l->hdr);
    if (r != NexusFmt_Ok) {
        snprintf(problem, problem_size, "not an XCI -- %s", nexusFmtStr(r));
        return false;
    }

    // Root HFS0: the table of partitions.
    PartitionFsContext root;
    u8 *root_header = read_partition_header(src, l->hdr.root_offset, &root, "root");
    if (root_header == NULL) {
        snprintf(problem, problem_size, "the root partition table is unreadable");
        return false;
    }

    u64 secure_size = 0;
    r = xciFindPartition(&l->hdr, &root, SECURE_PARTITION, &l->secure_offset, &secure_size);
    free(root_header);

    if (r != NexusFmt_Ok) {
        snprintf(problem, problem_size, "no \"secure\" partition -- %s", nexusFmtStr(r));
        return false;
    }

    // The secure partition is itself an HFS0, and its files are the NCAs.
    l->secure_header = read_partition_header(src, l->secure_offset, &l->secure, "secure");
    if (l->secure_header == NULL) {
        snprintf(problem, problem_size, "the secure partition is unreadable");
        return false;
    }

    l->data_offset = l->secure_offset + partitionFsGetDataOffset(&l->secure);
    return true;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

static void inspect(Source *src, NexusXciInfo *out)
{
    memset(out, 0, sizeof(*out));

    Layout l;
    if (!resolve_layout(src, &l, out->problem, sizeof(out->problem))) return;

    out->cart_size = l.hdr.cart_size;

    const u32 count = partitionFsGetEntryCount(&l.secure);
    for (u32 i = 0; i < count; i++) {
        PartitionFsEntry e;
        if (partitionFsGetEntry(&l.secure, i, &e) != NexusFmt_Ok) continue;

        if (!is_content_name(e.name)) continue;

        const size_t n = strlen(e.name);
        if ((n > 9 && strcmp(e.name + (n - 9), ".cnmt.nca") == 0)
            || (n > 9 && strcmp(e.name + (n - 9), ".cnmt.ncz") == 0)) {
            out->has_meta = true;
        }

        out->content_count++;
        out->total_bytes += e.size;
    }

    layout_free(&l);

    if (out->content_count == 0) {
        snprintf(out->problem, sizeof(out->problem),
                 "the secure partition holds no NCAs");
        return;
    }
    if (!out->has_meta) {
        snprintf(out->problem, sizeof(out->problem),
                 "no .cnmt.nca -- this image cannot be installed");
        return;
    }

    out->valid = true;
}

Result nexusXciInspectFile(const char *path, NexusXciInfo *out)
{
    if (path == NULL || out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Source src;
    if (!source_open_file(&src, path)) {
        memset(out, 0, sizeof(*out));
        snprintf(out->problem, sizeof(out->problem), "cannot open the file");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    inspect(&src, out);
    source_close(&src);
    return 0;
}

Result nexusXciInspectGameCard(NexusXciInfo *out)
{
    if (out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Source src;
    if (!source_open_card(&src)) {
        memset(out, 0, sizeof(*out));
        snprintf(out->problem, sizeof(out->problem), "no gamecard in the slot");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    inspect(&src, out);
    source_close(&src);
    return 0;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

// Builds a PFS0 over the secure partition's NCAs and streams it through the
// ordinary installer. Entries are fed in exactly the order they were added, so
// the synthesised header's offsets match what actually arrives.
static Result run_install(Source *src, u8 target)
{
    Layout l;
    char problem[160] = {0};

    if (!resolve_layout(src, &l, problem, sizeof(problem))) {
        snprintf(g_state.status, sizeof(g_state.status), "%.90s", problem);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    u8 *nsp_header = (u8 *)malloc(HEADER_CAP);
    u8 *chunk      = (u8 *)malloc(STREAM_CHUNK);
    NexusInstaller *ins = (NexusInstaller *)calloc(1, sizeof(NexusInstaller));

    if (nsp_header == NULL || chunk == NULL || ins == NULL) {
        free(nsp_header); free(chunk); free(ins);
        layout_free(&l);
        snprintf(g_state.status, sizeof(g_state.status), "out of memory");
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    NexusNczDecoder *decoder = NULL;

    NspBuilder b;
    nspBuilderInit(&b, nsp_header, HEADER_CAP);

    // Remember where each added entry's bytes live in the source image.
    u64 src_offset[NSP_MAX_ENTRIES];
    u32 added = 0;

    const u32 count = partitionFsGetEntryCount(&l.secure);
    for (u32 i = 0; i < count && added < NSP_MAX_ENTRIES; i++) {
        PartitionFsEntry e;
        if (partitionFsGetEntry(&l.secure, i, &e) != NexusFmt_Ok) continue;

        if (!is_content_name(e.name)) continue;

        if (nspBuilderAdd(&b, e.name, e.size) != NexusFmt_Ok) {
            LOG_W("xci: skipping %s -- builder full", e.name);
            continue;
        }

        src_offset[added++] = l.data_offset + e.offset;
    }

    Result rc = 0;

    if (added == 0 || nspBuilderFinalize(&b) != NexusFmt_Ok) {
        snprintf(g_state.status, sizeof(g_state.status), "nothing installable here");
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        goto cleanup;
    }

    g_state.content_count = added;
    g_state.total         = nspBuilderTotalSize(&b);
    g_state.received      = 0;

    NexusHorizonBackend *backend = nexusHorizonBackendCreate(target);
    if (backend == NULL) {
        snprintf(g_state.status, sizeof(g_state.status), "no install target");
        rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        goto cleanup;
    }

    nexusInstallBegin(ins, nexusHorizonBackendOps(), backend, target);

    // XCZ: the secure partition holds .ncz files instead of .nca. Nothing else
    // about the container changes, so the same decoder handles it.
    decoder = nexusNczDecoderCreate();
    if (decoder != NULL) {
        nexusInstallSetDecompressor(ins, nexusNczDecoderOps(), decoder);
    }

    snprintf(g_state.status, sizeof(g_state.status), "installing");

    // 1. The synthesised header.
    size_t header_size = 0;
    const u8 *hdr = nspBuilderHeader(&b, &header_size);

    NexusInstallResult ir = nexusInstallFeed(ins, hdr, header_size);
    g_state.received += header_size;

    // 2. Each NCA's bytes, in the order the builder recorded them.
    for (u32 i = 0; i < added && (ir == NexusInstall_InProgress || ir == NexusInstall_Ok); i++) {
        u64 remaining = b.entries[i].size;
        u64 at        = src_offset[i];

        while (remaining > 0) {
            if (g_cancel) {
                snprintf(g_state.status, sizeof(g_state.status), "cancelled");
                ir = NexusInstall_Aborted;
                break;
            }

            size_t want = (remaining > STREAM_CHUNK) ? STREAM_CHUNK : (size_t)remaining;

            if (!source_read(src, at, chunk, want)) {
                LOG_E("xci: read failed at %llu", (unsigned long long)at);
                snprintf(g_state.status, sizeof(g_state.status), "read error");
                ir = NexusInstall_Aborted;
                break;
            }

            ir = nexusInstallFeed(ins, chunk, want);
            if (ir != NexusInstall_InProgress && ir != NexusInstall_Ok) break;

            at               += want;
            remaining        -= want;
            g_state.received += want;
        }
    }

    if (ir != NexusInstall_InProgress && ir != NexusInstall_Ok) {
        LOG_E("xci: %s", nexusInstallStr(ir));
        if (g_state.status[0] == '\0' || strcmp(g_state.status, "installing") == 0) {
            snprintf(g_state.status, sizeof(g_state.status), "failed: %s",
                     nexusInstallStr(ir));
        }
        nexusInstallAbort(ins);
        rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    } else {
        snprintf(g_state.status, sizeof(g_state.status), "finalising");

        const NexusInstallResult fr = nexusInstallFinish(ins);
        if (fr != NexusInstall_Ok) {
            LOG_E("xci: %s", nexusInstallStr(fr));
            snprintf(g_state.status, sizeof(g_state.status), "failed: %s",
                     nexusInstallStr(fr));
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        } else {
            const NexusInstallMeta *meta = nexusInstallGetMeta(ins);
            LOG_I("xci: installed %016llx v%u (%u contents)",
                  (unsigned long long)meta->title_id, meta->version, meta->content_count);
            snprintf(g_state.status, sizeof(g_state.status), "installed");
        }
    }

    nexusHorizonBackendDestroy(backend);

cleanup:
    free(nsp_header);
    free(chunk);
    free(ins);
    nexusNczDecoderDestroy(decoder);
    layout_free(&l);
    return rc;
}

static Result install_common(Source *src, const char *name, u8 target)
{
    memset(&g_state, 0, sizeof(g_state));
    g_cancel       = false;
    g_state.active = true;
    snprintf(g_state.name, sizeof(g_state.name), "%.120s", name);
    snprintf(g_state.status, sizeof(g_state.status), "reading the container");

    const Result rc = run_install(src, target);

    g_state.active = false;
    return rc;
}

Result nexusXciInstallFile(const char *path, u8 target)
{
    if (path == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Source src;
    if (!source_open_file(&src, path)) {
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    const char *base = strrchr(path, '/');
    const Result rc = install_common(&src, base != NULL ? base + 1 : path, target);

    source_close(&src);
    return rc;
}

Result nexusXciInstallGameCard(u8 target)
{
    Source src;
    if (!source_open_card(&src)) {
        memset(&g_state, 0, sizeof(g_state));
        snprintf(g_state.status, sizeof(g_state.status), "no gamecard in the slot");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    const Result rc = install_common(&src, "Gamecard", target);

    source_close(&src);
    return rc;
}

const NexusXciState *nexusXciGetState(void)
{
    return &g_state;
}

void nexusXciCancel(void)
{
    g_cancel = true;
}
