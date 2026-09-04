// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install from files already on the SD card.

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nexus/local_install.h"
#include "nexus/install_horizon.h"
#include "nexus/xci_install.h"
#include "nexus/ncz_decode.h"
#include "nexus/log.h"

#define STREAM_CHUNK (1024u * 1024u)

static NexusLocalState g_state;
static volatile bool   g_cancel = false;

const char *nexusLocalKindStr(u8 kind)
{
    switch (kind) {
        case NexusLocalKind_Nsp:        return "NSP";
        case NexusLocalKind_SplitNsp:   return "split NSP";
        case NexusLocalKind_Xci:        return "XCI";
        default:                        return "unknown";
    }
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return false;

    for (size_t i = 0; i < m; i++) {
        if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

// A split NSP's parts are numbered files: 00, 01, 02 ... Anything else in the
// directory means it is not a split container.
static bool part_name_index(const char *name, u32 *out_index)
{
    if (name[0] == '\0') return false;

    u32 value = 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        value = value * 10 + (u32)(*p - '0');
        if (value > NEXUS_LOCAL_MAX_PARTS) return false;
    }

    *out_index = value;
    return true;
}

// Counts and sizes the parts of a candidate split NSP. Returns 0 when the
// directory is not one.
static u32 measure_split(const char *path, u64 *out_size)
{
    DIR *d = opendir(path);
    if (d == NULL) return 0;

    u32 count = 0;
    u64 total = 0;
    bool ok   = true;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        u32 index = 0;
        if (!part_name_index(ent->d_name, &index)) { ok = false; break; }

        char part[FS_MAX_PATH];
        snprintf(part, sizeof(part), "%.*s/%.32s",
                 (int)(sizeof(part) - 40), path, ent->d_name);

        struct stat st;
        if (stat(part, &st) != 0) { ok = false; break; }

        total += (u64)st.st_size;
        count++;
    }

    closedir(d);

    if (!ok || count == 0 || count > NEXUS_LOCAL_MAX_PARTS) return 0;

    *out_size = total;
    return count;
}

Result nexusLocalScan(const char *dir, NexusLocalList *out)
{
    if (dir == NULL || out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(out, 0, sizeof(*out));

    DIR *d = opendir(dir);
    if (d == NULL) {
        // Creating it is friendlier than telling the user to: the folder is
        // where the feature expects its input, so it should just exist.
        mkdir(dir, 0777);
        LOG_W("local: %s does not exist -- created it", dir);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        if (out->count >= NEXUS_LOCAL_MAX) { out->truncated = true; break; }

        char path[FS_MAX_PATH];
        snprintf(path, sizeof(path), "%.*s/%.180s",
                 (int)(sizeof(path) - 200), dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        NexusLocalItem *item = &out->items[out->count];
        memset(item, 0, sizeof(*item));
        snprintf(item->name, sizeof(item->name), "%.180s", ent->d_name);
        snprintf(item->path, sizeof(item->path), "%s", path);

        if (S_ISDIR(st.st_mode)) {
            u64 size = 0;
            const u32 parts = measure_split(path, &size);
            if (parts == 0) continue;   // just a folder, not a container

            item->kind  = NexusLocalKind_SplitNsp;
            item->parts = parts;
            item->size  = size;
        } else if (ends_with(ent->d_name, ".nsp") || ends_with(ent->d_name, ".nsz")) {
            // An NSZ is an ordinary PFS0 whose NCAs have been replaced by
            // NCZs, so it takes exactly the same path with a decoder attached.
            item->kind = NexusLocalKind_Nsp;
            item->size = (u64)st.st_size;
        } else if (ends_with(ent->d_name, ".xci") || ends_with(ent->d_name, ".xcz")) {
            item->kind = NexusLocalKind_Xci;
            item->size = (u64)st.st_size;
        } else {
            continue;
        }

        out->count++;
    }

    closedir(d);

    LOG_I("local: %u installable item(s) in %s", out->count, dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Streaming an NSP, whole or split
// ---------------------------------------------------------------------------

// Feeds one file's bytes into the installer. Returns false on any failure,
// with the reason already in g_state.status.
static bool feed_file(NexusInstaller *ins, const char *path, u8 *chunk)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        LOG_E("local: cannot open %s", path);
        snprintf(g_state.status, sizeof(g_state.status), "cannot open a part");
        return false;
    }

    bool ok = true;

    for (;;) {
        if (g_cancel) {
            snprintf(g_state.status, sizeof(g_state.status), "cancelled");
            ok = false;
            break;
        }

        const size_t got = fread(chunk, 1, STREAM_CHUNK, f);
        if (got == 0) break;

        const NexusInstallResult r = nexusInstallFeed(ins, chunk, got);
        if (r != NexusInstall_InProgress && r != NexusInstall_Ok) {
            LOG_E("local: %s", nexusInstallStr(r));
            snprintf(g_state.status, sizeof(g_state.status), "failed: %s",
                     nexusInstallStr(r));
            ok = false;
            break;
        }

        g_state.received += got;
    }

    fclose(f);
    return ok;
}

// Split parts must be fed strictly in numeric order: together they are one
// PFS0, so a part out of sequence corrupts the stream silently.
static bool feed_split(NexusInstaller *ins, const char *dir, u32 parts, u8 *chunk)
{
    for (u32 i = 0; i < parts; i++) {
        // The index is masked as well as bounded so the compiler can see the
        // formatted width, not just the loop bound.
        char part[FS_MAX_PATH];
        snprintf(part, sizeof(part), "%.*s/%02u", (int)(sizeof(part) - 16), dir,
                 (unsigned)(i & 0xFFu));

        struct stat st;
        if (stat(part, &st) != 0) {
            LOG_E("local: split part %02u is missing from %s", i, dir);
            snprintf(g_state.status, sizeof(g_state.status), "part %02u is missing", i);
            return false;
        }

        if (!feed_file(ins, part, chunk)) return false;
    }

    return true;
}

Result nexusLocalInstall(const NexusLocalItem *item, u8 target)
{
    if (item == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // XCIs have their own path: they are not PFS0 containers and need a
    // synthesised header built over the secure partition.
    if (item->kind == NexusLocalKind_Xci) {
        return nexusXciInstallFile(item->path, target);
    }

    memset(&g_state, 0, sizeof(g_state));
    g_cancel       = false;
    g_state.active = true;
    g_state.total  = item->size;
    snprintf(g_state.name, sizeof(g_state.name), "%.180s", item->name);
    snprintf(g_state.status, sizeof(g_state.status), "installing");

    u8 *chunk = (u8 *)malloc(STREAM_CHUNK);
    NexusInstaller *ins = (NexusInstaller *)calloc(1, sizeof(NexusInstaller));
    NexusHorizonBackend *backend = nexusHorizonBackendCreate(target);

    if (chunk == NULL || ins == NULL || backend == NULL) {
        free(chunk);
        free(ins);
        if (backend != NULL) nexusHorizonBackendDestroy(backend);
        snprintf(g_state.status, sizeof(g_state.status), "out of memory");
        g_state.active = false;
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    nexusInstallBegin(ins, nexusHorizonBackendOps(), backend, target);

    NexusNczDecoder *decoder = nexusNczDecoderCreate();
    if (decoder != NULL) {
        nexusInstallSetDecompressor(ins, nexusNczDecoderOps(), decoder);
    }

    const bool fed = (item->kind == NexusLocalKind_SplitNsp)
        ? feed_split(ins, item->path, item->parts, chunk)
        : feed_file(ins, item->path, chunk);

    Result rc = 0;

    if (!fed) {
        nexusInstallAbort(ins);
        rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    } else {
        snprintf(g_state.status, sizeof(g_state.status), "finalising");

        const NexusInstallResult r = nexusInstallFinish(ins);
        if (r != NexusInstall_Ok) {
            LOG_E("local: %s", nexusInstallStr(r));
            snprintf(g_state.status, sizeof(g_state.status), "failed: %s",
                     nexusInstallStr(r));
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        } else {
            const NexusInstallMeta *meta = nexusInstallGetMeta(ins);
            LOG_I("local: installed %016llx v%u (%u contents)",
                  (unsigned long long)meta->title_id, meta->version, meta->content_count);
            snprintf(g_state.status, sizeof(g_state.status), "installed");
        }
    }

    free(chunk);
    free(ins);
    nexusNczDecoderDestroy(decoder);
    nexusHorizonBackendDestroy(backend);

    g_state.active = false;
    return rc;
}

const NexusLocalState *nexusLocalGetState(void)
{
    return &g_state;
}

void nexusLocalCancel(void)
{
    g_cancel = true;
}
