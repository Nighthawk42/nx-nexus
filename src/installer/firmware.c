// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- system firmware installation.
//
// A firmware set is a folder of loose NCAs: many "<id>.cnmt.nca" meta files,
// one per system title, plus the content they describe. Installing means
// registering all of it into the BuiltInSystem content storage and then writing
// a meta record for each meta NCA.
//
// The order matters. Content is registered first, then the metas, so that when
// a meta record lands every NCA it references is already present. Doing it the
// other way leaves the system briefly describing content that does not exist.
//
// The CNMT inside each meta NCA is encrypted, and this tool holds no keys -- so
// the same trick the NSP installer uses applies here: register the meta NCA
// first, then ask Horizon to mount it as ContentMeta, which hands back the
// plaintext .cnmt.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "nexus/firmware.h"
#include "nexus/install_horizon.h"
#include "nexus/installer.h"
#include "nexus/cnmt.h"
#include "nexus/sysinfo.h"
#include "nexus/log.h"

#define COPY_CHUNK (1u * 1024u * 1024u)

static NexusFwProgress g_progress;
static volatile bool   g_cancel = false;

const char *nexusFwStr(NexusFwResult r)
{
    switch (r) {
        case NexusFw_Ok:              return "ok";
        case NexusFw_NotEmummc:       return "refused: not running on an emuMMC";
        case NexusFw_NoFolder:        return "folder not found";
        case NexusFw_NoContent:       return "no NCA files in the folder";
        case NexusFw_NoSystemUpdate:  return "no SystemUpdate meta -- not a firmware set";
        case NexusFw_TooManyFiles:    return "too many files";
        case NexusFw_ReadFailed:      return "could not read a file";
        case NexusFw_InstallFailed:   return "install failed";
        case NexusFw_NoBackend:       return "could not open system content storage";
        case NexusFw_Cancelled:       return "cancelled";
        default:                      return "unknown";
    }
}

bool nexusFirmwareInstallAllowed(void)
{
    // The single gate that makes this feature reasonable to offer at all.
    return nexusSysInfoIsEmummc();
}

const NexusFwProgress *nexusFirmwareGetProgress(void) { return &g_progress; }

void nexusFirmwareCancel(void) { g_cancel = true; }

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + (n - m), suffix) == 0;
}

NexusFwResult nexusFirmwareScan(const char *dir, NexusFirmwareSet *set)
{
    if (dir == NULL || set == NULL) return NexusFw_NoFolder;

    memset(set, 0, sizeof(*set));
    snprintf(set->dir, sizeof(set->dir), "%s", dir);

    DIR *d = opendir(dir);
    if (d == NULL) {
        snprintf(set->problem, sizeof(set->problem), "%s does not exist", dir);
        return NexusFw_NoFolder;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!ends_with(ent->d_name, ".nca")) continue;

        if (set->nca_count >= NEXUS_FIRMWARE_MAX_NCA) {
            closedir(d);
            snprintf(set->problem, sizeof(set->problem),
                     "more than %d NCAs; this does not look like a firmware set",
                     NEXUS_FIRMWARE_MAX_NCA);
            return NexusFw_TooManyFiles;
        }

        char path[FS_MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) <= 0) continue;

        struct stat sb;
        if (stat(path, &sb) != 0 || sb.st_size <= 0) continue;

        set->nca_count++;
        set->total_bytes += (u64)sb.st_size;
        if (ends_with(ent->d_name, ".cnmt.nca")) set->meta_count++;
    }
    closedir(d);

    if (set->nca_count == 0) {
        snprintf(set->problem, sizeof(set->problem), "no .nca files in %s", dir);
        return NexusFw_NoContent;
    }
    if (set->meta_count == 0) {
        snprintf(set->problem, sizeof(set->problem),
                 "no .cnmt.nca files -- this is not a firmware set");
        return NexusFw_NoSystemUpdate;
    }

    // A real firmware set has on the order of a hundred metas. A handful means
    // someone pointed this at a game folder, which must not be installed as
    // system content.
    if (set->meta_count < 16) {
        snprintf(set->problem, sizeof(set->problem),
                 "only %u meta files; a firmware set has far more",
                 set->meta_count);
        return NexusFw_NoSystemUpdate;
    }

    set->has_system_update = true;   // confirmed properly during install
    set->valid = true;
    LOG_I("firmware: %s holds %u NCAs (%u meta), %llu MiB",
          dir, set->nca_count, set->meta_count,
          (unsigned long long)(set->total_bytes / (1024ull * 1024ull)));
    return NexusFw_Ok;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

// Streams one NCA from the SD card into a registered content entry.
static bool install_one_nca(const NexusInstallBackendOps *ops, void *backend,
                            const char *path, const char *filename, u64 size)
{
    u8 id[NEXUS_CONTENT_ID_SIZE];

    // The content id is the filename up to the first dot, for both plain and
    // .cnmt.nca forms.
    const char *dot = strchr(filename, '.');
    const size_t stem = (dot != NULL) ? (size_t)(dot - filename) : strlen(filename);
    if (!nexusInstallParseContentId(filename, stem, id)) {
        LOG_W("firmware: skipping %s -- name is not a content id", filename);
        return true;   // not fatal; just not ours to install
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        LOG_E("firmware: cannot open %s", filename);
        return false;
    }

    if (ops->content_begin(backend, id, size) != 0) {
        fclose(f);
        LOG_E("firmware: could not create a placeholder for %s", filename);
        return false;
    }

    u8 *buf = (u8 *)malloc(COPY_CHUNK);
    if (buf == NULL) {
        ops->content_discard(backend);
        fclose(f);
        return false;
    }

    bool ok = true;
    for (;;) {
        if (g_cancel) { ok = false; break; }

        const size_t got = fread(buf, 1, COPY_CHUNK, f);
        if (got == 0) break;

        if (ops->content_write(backend, buf, got) != 0) { ok = false; break; }

        g_progress.bytes_done += got;
    }

    free(buf);
    fclose(f);

    if (!ok) {
        ops->content_discard(backend);
        return false;
    }

    if (ops->content_commit(backend) != 0) {
        LOG_E("firmware: could not register %s", filename);
        return false;
    }

    return true;
}

// Builds and registers the meta record for one already-registered meta NCA.
static bool register_one_meta(const NexusInstallBackendOps *ops, void *backend,
                              const char *filename, u64 meta_nca_size,
                              bool *out_was_system_update)
{
    u8 meta_id[NEXUS_CONTENT_ID_SIZE];
    const char *dot = strchr(filename, '.');
    const size_t stem = (dot != NULL) ? (size_t)(dot - filename) : strlen(filename);
    if (!nexusInstallParseContentId(filename, stem, meta_id)) return true;

    // Horizon decrypts the CNMT for us now that the meta NCA is registered.
    u8 cnmt_buf[NEXUS_INSTALL_MAX_CNMT];
    size_t cnmt_len = 0;
    if (ops->read_cnmt(backend, meta_id, cnmt_buf, sizeof(cnmt_buf), &cnmt_len) != 0) {
        LOG_W("firmware: could not read the cnmt from %s", filename);
        return false;
    }

    CnmtContext cnmt;
    if (cnmtInit(&cnmt, cnmt_buf, cnmt_len) != NexusFmt_Ok) {
        LOG_W("firmware: %s holds an unreadable cnmt", filename);
        return false;
    }

    if (cnmt.meta_type == CnmtMetaType_SystemUpdate && out_was_system_update != NULL) {
        *out_was_system_update = true;
        LOG_I("firmware: SystemUpdate meta is version %u", cnmt.version);
    }

    NexusInstallMeta *meta = (NexusInstallMeta *)calloc(1, sizeof(NexusInstallMeta));
    if (meta == NULL) return false;

    meta->title_id   = cnmt.title_id;
    meta->version    = cnmt.version;
    meta->meta_type  = cnmt.meta_type;
    meta->storage_id = NcmStorageId_BuiltInSystem;

    if (cnmt.extended_header_size <= sizeof(meta->ext_header)) {
        meta->ext_header_size = cnmt.extended_header_size;
        if (cnmt.extended_header_size > 0) {
            memcpy(meta->ext_header, cnmt.data + CNMT_HEADER_SIZE,
                   cnmt.extended_header_size);
        }
    }

    // The meta NCA describes itself only by being the meta; add its own entry
    // the way the NSP installer does.
    NexusInstallContent *self = &meta->contents[meta->content_count++];
    memcpy(self->content_id, meta_id, NEXUS_CONTENT_ID_SIZE);
    self->size         = meta_nca_size;
    self->content_type = CnmtContentType_Meta;

    const u16 n = cnmtGetContentCount(&cnmt);
    for (u16 i = 0; i < n && meta->content_count < NEXUS_INSTALL_MAX_CONTENTS; i++) {
        CnmtContentInfo info;
        if (cnmtGetContentInfo(&cnmt, i, &info) != NexusFmt_Ok) continue;
        if (info.content_type == CnmtContentType_DeltaFragment) continue;

        NexusInstallContent *c = &meta->contents[meta->content_count++];
        memcpy(c->content_id, info.content_id, NEXUS_CONTENT_ID_SIZE);
        c->size         = info.size;
        c->attr         = info.content_attributes;
        c->content_type = info.content_type;
        c->id_offset    = info.id_offset;
    }

    const bool ok = (ops->register_meta(backend, meta) == 0);
    if (!ok) LOG_E("firmware: registering the meta record for %s failed", filename);

    free(meta);
    return ok;
}

NexusFwResult nexusFirmwareInstall(const NexusFirmwareSet *set)
{
    if (set == NULL || !set->valid) return NexusFw_NoContent;

    // The gate. Never install system firmware onto the NAND the console needs
    // in order to boot at all.
    if (!nexusFirmwareInstallAllowed()) {
        LOG_E("firmware: refusing to install -- this console booted from %s, "
              "not an emuMMC", nexusSysInfoStorageName());
        return NexusFw_NotEmummc;
    }

    memset(&g_progress, 0, sizeof(g_progress));
    g_cancel = false;
    g_progress.active      = true;
    g_progress.files_total = set->nca_count;
    g_progress.bytes_total = set->total_bytes;
    snprintf(g_progress.status, sizeof(g_progress.status), "opening system storage");

    NexusHorizonBackend *backend = nexusHorizonBackendCreate(NcmStorageId_BuiltInSystem);
    if (backend == NULL) {
        g_progress.active = false;
        return NexusFw_NoBackend;
    }
    const NexusInstallBackendOps *ops = nexusHorizonBackendOps();

    LOG_W("firmware: installing to the %s system partition -- do not power off",
          nexusSysInfoStorageName());

    // --- pass 1: register every NCA as content -----------------------------
    snprintf(g_progress.status, sizeof(g_progress.status), "installing content");

    DIR *d = opendir(set->dir);
    if (d == NULL) {
        nexusHorizonBackendDestroy(backend);
        g_progress.active = false;
        return NexusFw_NoFolder;
    }

    NexusFwResult result = NexusFw_Ok;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (!ends_with(ent->d_name, ".nca")) continue;
        if (g_cancel) { result = NexusFw_Cancelled; break; }

        char path[FS_MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", set->dir, ent->d_name) <= 0) continue;

        struct stat sb;
        if (stat(path, &sb) != 0 || sb.st_size <= 0) continue;

        snprintf(g_progress.current, sizeof(g_progress.current), "%s", ent->d_name);

        if (!install_one_nca(ops, backend, path, ent->d_name, (u64)sb.st_size)) {
            result = g_cancel ? NexusFw_Cancelled : NexusFw_InstallFailed;
            break;
        }

        g_progress.files_done++;
    }
    closedir(d);

    if (result != NexusFw_Ok) {
        LOG_E("firmware: %s -- rolling back", nexusFwStr(result));
        ops->rollback(backend, NULL);
        nexusHorizonBackendDestroy(backend);
        g_progress.active = false;
        snprintf(g_progress.status, sizeof(g_progress.status), "%s", nexusFwStr(result));
        return result;
    }

    // --- pass 2: write a meta record for each meta NCA ---------------------
    snprintf(g_progress.status, sizeof(g_progress.status), "registering metadata");
    g_progress.files_done = 0;
    g_progress.files_total = set->meta_count;

    bool saw_system_update = false;
    u32  metas_done = 0;

    d = opendir(set->dir);
    if (d == NULL) {
        nexusHorizonBackendDestroy(backend);
        g_progress.active = false;
        return NexusFw_NoFolder;
    }

    while ((ent = readdir(d)) != NULL) {
        if (!ends_with(ent->d_name, ".cnmt.nca")) continue;
        if (g_cancel) { result = NexusFw_Cancelled; break; }

        char path[FS_MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", set->dir, ent->d_name) <= 0) continue;

        struct stat sb;
        if (stat(path, &sb) != 0) continue;

        snprintf(g_progress.current, sizeof(g_progress.current), "%s", ent->d_name);

        if (!register_one_meta(ops, backend, ent->d_name, (u64)sb.st_size,
                               &saw_system_update)) {
            result = NexusFw_InstallFailed;
            break;
        }

        metas_done++;
        g_progress.files_done = metas_done;
    }
    closedir(d);

    if (result == NexusFw_Ok && !saw_system_update) {
        // Nothing here declared itself a system update. Better to refuse and
        // roll back than to leave a half-described system.
        LOG_E("firmware: no SystemUpdate meta was found -- rolling back");
        result = NexusFw_NoSystemUpdate;
    }

    if (result != NexusFw_Ok) {
        LOG_E("firmware: %s -- rolling back", nexusFwStr(result));
        ops->rollback(backend, NULL);
        nexusHorizonBackendDestroy(backend);
        g_progress.active = false;
        snprintf(g_progress.status, sizeof(g_progress.status), "%s", nexusFwStr(result));
        return result;
    }

    nexusHorizonBackendDestroy(backend);

    g_progress.active = false;
    snprintf(g_progress.status, sizeof(g_progress.status), "installed -- reboot to apply");
    LOG_I("firmware: installed %u content, %u meta records; reboot to apply",
          set->nca_count, metas_done);
    return NexusFw_Ok;
}
