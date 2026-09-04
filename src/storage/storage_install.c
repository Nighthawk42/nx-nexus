// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- virtual MTP store that installs what you drop into it.
//
// This is the store that makes the whole design work without a companion app:
// it appears in the host file manager as an ordinary (empty, write-only) drive,
// and copying an NSP into it streams the bytes straight through the installer
// into ncm placeholders. Nothing is staged on the SD card.
//
// The store deliberately lists no contents. Enumerating installed titles here
// would invite hosts to try to read or delete them, and MTP has no way to say
// "write-only" beyond simply having nothing to show.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/storage.h"
#include "nexus/mtp_types.h"
#include "nexus/installer.h"
#include "nexus/install_horizon.h"
#include "nexus/log.h"

typedef struct {
    char description[64];
    u8   target;              // NexusInstallTarget

    // Live only for the duration of one transfer.
    NexusHorizonBackend *backend;
    NexusInstaller      *installer;
    bool                 active;
    char                 filename[NEXUS_PATH_MAX];
    u64                  declared_size;
} InstallStorage;

// True for names the installer can actually consume.
static bool is_installable(const char *path)
{
    const size_t n = strlen(path);
    if (n < 5) return false;

    // .nsp is a PFS0; .nsz is a compressed NSP that we cannot handle yet.
    return strcmp(path + (n - 4), ".nsp") == 0
        || strcmp(path + (n - 4), ".NSP") == 0;
}

static const char *inst_description(NexusStorage *self)
{
    return ((InstallStorage *)self->impl)->description;
}

static Result inst_get_info(NexusStorage *self, NexusStorageInfo *out)
{
    InstallStorage *s = (InstallStorage *)self->impl;

    memset(out, 0, sizeof(*out));
    out->storage_type      = MtpStorageType_FixedRAM;
    out->filesystem_type   = MtpFsType_GenericHierarchical;
    out->access_capability = MtpAccess_ReadWrite;
    out->free_objects      = 0xFFFFFFFFu;

    // Report the real free space of the install target so the host refuses an
    // NSP that cannot fit, rather than failing partway through the copy.
    NcmContentStorage cs;
    if (R_SUCCEEDED(ncmOpenContentStorage(&cs, (NcmStorageId)s->target))) {
        s64 total = 0, freesp = 0;
        if (R_SUCCEEDED(ncmContentStorageGetTotalSpaceSize(&cs, &total))) {
            out->capacity_bytes = (u64)total;
        }
        if (R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(&cs, &freesp))) {
            out->free_bytes = (u64)freesp;
        }
        ncmContentStorageClose(&cs);
    }

    return 0;
}

static Result inst_enumerate(NexusStorage *self, const char *dir_path,
                             NexusEnumCallback cb, void *user)
{
    // Always empty: this is a drop target, not a browsable filesystem.
    (void)self; (void)dir_path; (void)cb; (void)user;
    return 0;
}

static Result inst_stat(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size)
{
    // Only the root exists.
    (void)self;
    if (strcmp(path, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

static Result inst_read(NexusStorage *self, const char *path, u64 offset,
                        void *buffer, size_t size, size_t *out_read)
{
    (void)self; (void)path; (void)offset; (void)buffer; (void)size;
    if (out_read) *out_read = 0;
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

// Releases everything held by an in-flight transfer.
static void inst_teardown(InstallStorage *s)
{
    if (s->installer != NULL) {
        free(s->installer);
        s->installer = NULL;
    }
    if (s->backend != NULL) {
        nexusHorizonBackendDestroy(s->backend);
        s->backend = NULL;
    }
    s->active        = false;
    s->filename[0]   = '\0';
    s->declared_size = 0;
}

static Result inst_write_begin(NexusStorage *self, const char *path, u64 declared_size)
{
    InstallStorage *s = (InstallStorage *)self->impl;

    if (s->active) {
        LOG_W("install: replacing an abandoned transfer of %s", s->filename);
        nexusInstallAbort(s->installer);
        inst_teardown(s);
    }

    if (!is_installable(path)) {
        LOG_W("install: refusing %s -- only .nsp is supported", path);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    s->backend = nexusHorizonBackendCreate(s->target);
    if (s->backend == NULL) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    // NexusInstaller embeds its staging buffers, so it is far too large to sit
    // on a thread stack.
    s->installer = (NexusInstaller *)calloc(1, sizeof(NexusInstaller));
    if (s->installer == NULL) {
        nexusHorizonBackendDestroy(s->backend);
        s->backend = NULL;
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    nexusInstallBegin(s->installer, nexusHorizonBackendOps(), s->backend, s->target);

    s->active        = true;
    s->declared_size = declared_size;
    snprintf(s->filename, sizeof(s->filename), "%s", path);

    LOG_I("install: starting %s (%llu MiB) -> %s", path,
          (unsigned long long)(declared_size / (1024ull * 1024ull)), s->description);
    return 0;
}

static Result inst_write_chunk(NexusStorage *self, const void *buffer, size_t size)
{
    InstallStorage *s = (InstallStorage *)self->impl;
    if (!s->active) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    const NexusInstallResult r = nexusInstallFeed(s->installer, buffer, size);
    if (r != NexusInstall_InProgress && r != NexusInstall_Ok) {
        LOG_E("install: %s failed -- %s", s->filename, nexusInstallStr(r));
        inst_teardown(s);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    return 0;
}

static Result inst_write_end(NexusStorage *self, bool committed)
{
    InstallStorage *s = (InstallStorage *)self->impl;
    if (!s->active) return 0;

    if (!committed) {
        LOG_W("install: %s cancelled by the host", s->filename);
        nexusInstallAbort(s->installer);
        inst_teardown(s);
        return 0;
    }

    const NexusInstallResult r = nexusInstallFinish(s->installer);
    if (r != NexusInstall_Ok) {
        LOG_E("install: %s failed to finalise -- %s", s->filename, nexusInstallStr(r));
        inst_teardown(s);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    const NexusInstallMeta *meta = nexusInstallGetMeta(s->installer);
    LOG_I("install: %s done -- title %016llx v%u, %u contents",
          s->filename, (unsigned long long)meta->title_id,
          meta->version, meta->content_count);

    inst_teardown(s);
    return 0;
}

static const NexusStorageOps g_install_ops = {
    .description = inst_description,
    .get_info    = inst_get_info,
    .enumerate   = inst_enumerate,
    .stat        = inst_stat,
    .read        = inst_read,
    .write_begin = inst_write_begin,
    .write_chunk = inst_write_chunk,
    .write_end   = inst_write_end,
    .mkdir       = NULL,   // a drop target has no directories
    .remove      = NULL,
};

Result nexusStorageInstallCreate(NexusStorage *out, u32 storage_id, u8 target,
                                 const char *description)
{
    InstallStorage *s = (InstallStorage *)calloc(1, sizeof(InstallStorage));
    if (s == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    s->target = target;
    snprintf(s->description, sizeof(s->description), "%s", description);

    out->storage_id = storage_id;
    out->ops        = &g_install_ops;
    out->impl       = s;

    // Present only if the target storage can actually be opened -- an unmounted
    // SD card or a disabled NAND should not show up as a drop target.
    NcmContentStorage cs;
    if (R_SUCCEEDED(ncmOpenContentStorage(&cs, (NcmStorageId)target))) {
        out->present = true;
        ncmContentStorageClose(&cs);
    } else {
        out->present = false;
    }

    LOG_I("storage: %s %s", description, out->present ? "ready" : "NOT AVAILABLE");
    return 0;
}
