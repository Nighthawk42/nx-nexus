// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- passthrough storage backend over a libnx devoptab mount.
//
// This is the Phase 1 backend: plain POSIX file IO against sdmc:/, exposed as
// a read/write MTP store. It is deliberately the simplest possible backend so
// that Phase 1 verifies the USB and MTP layers rather than the storage layer.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>

#include "nexus/storage.h"
#include "nexus/mtp_types.h"
#include "nexus/log.h"

typedef struct {
    char  mount[16];              // e.g. "sdmc:"
    char  description[64];        // shown in the host file explorer

    // Active write transaction, set up by write_begin.
    FILE *write_file;
    char  write_path[NEXUS_PATH_MAX];
    u64   write_declared;
    u64   write_received;
} SdmcStorage;

// Joins the mount prefix and a storage-relative path into a devoptab path.
// Returns false if the result would not fit.
static bool sdmc_real_path(SdmcStorage *s, const char *path, char *out, size_t out_size)
{
    if (path == NULL || path[0] != '/') return false;

    // "/" means the mount root, which devoptab spells as "sdmc:/".
    int n = snprintf(out, out_size, "%s%s", s->mount, path);
    return n > 0 && (size_t)n < out_size;
}

static const char *sdmc_description(NexusStorage *self)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;
    return s->description;
}

static Result sdmc_get_info(NexusStorage *self, NexusStorageInfo *out)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    memset(out, 0, sizeof(*out));
    out->storage_type      = MtpStorageType_RemovableRAM;
    out->filesystem_type   = MtpFsType_GenericHierarchical;
    out->access_capability = MtpAccess_ReadWrite;
    out->free_objects      = 0xFFFFFFFFu;   // unknown

    char root[32];
    snprintf(root, sizeof(root), "%s/", s->mount);

    struct statvfs st;
    if (statvfs(root, &st) != 0) {
        // Report the store as present but of unknown size rather than failing
        // the whole operation -- hosts cope with this better than an error.
        LOG_W("sdmc: statvfs(%s) failed (errno %d)", root, errno);
        out->capacity_bytes = 0;
        out->free_bytes     = 0;
        return 0;
    }

    const u64 frag = (st.f_frsize != 0) ? (u64)st.f_frsize : (u64)st.f_bsize;
    out->capacity_bytes = (u64)st.f_blocks * frag;
    out->free_bytes     = (u64)st.f_bavail * frag;
    return 0;
}

static Result sdmc_enumerate(NexusStorage *self, const char *dir_path,
                             NexusEnumCallback cb, void *user)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, dir_path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    DIR *dir = opendir(real);
    if (dir == NULL) {
        LOG_W("sdmc: opendir(%s) failed (errno %d)", real, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        // Build the child path to stat it. d_type is available on libnx's
        // devoptab, but stat is needed for the size anyway.
        char child_real[NEXUS_PATH_MAX + 32];
        int n = snprintf(child_real, sizeof(child_real), "%s%s%s",
                         real,
                         (real[strlen(real) - 1] == '/') ? "" : "/",
                         ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child_real)) continue;  // skip over-long paths

        struct stat sb;
        if (stat(child_real, &sb) != 0) continue;

        const bool is_dir = S_ISDIR(sb.st_mode);
        if (!cb(user, ent->d_name, is_dir, is_dir ? 0 : (s64)sb.st_size)) break;
    }

    closedir(dir);
    return 0;
}

static Result sdmc_stat(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    struct stat sb;
    if (stat(real, &sb) != 0) return MAKERESULT(Module_Libnx, LibnxError_IoError);

    const bool is_dir = S_ISDIR(sb.st_mode);
    if (out_is_dir) *out_is_dir = is_dir;
    if (out_size)   *out_size   = is_dir ? 0 : (s64)sb.st_size;
    return 0;
}

static Result sdmc_read(NexusStorage *self, const char *path, u64 offset,
                        void *buffer, size_t size, size_t *out_read)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;
    if (out_read) *out_read = 0;

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    FILE *f = fopen(real, "rb");
    if (f == NULL) return MAKERESULT(Module_Libnx, LibnxError_IoError);

    Result rc = 0;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) {
        rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    } else {
        const size_t got = fread(buffer, 1, size, f);
        if (got == 0 && ferror(f)) {
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        } else if (out_read) {
            *out_read = got;
        }
    }

    fclose(f);
    return rc;
}

static Result sdmc_write_begin(NexusStorage *self, const char *path, u64 declared_size)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    if (s->write_file != NULL) {
        // A transaction is already open. MTP is strictly sequential, so this
        // means the previous one was abandoned; drop it and carry on.
        LOG_W("sdmc: replacing abandoned write transaction for %s", s->write_path);
        fclose(s->write_file);
        s->write_file = NULL;
    }

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    FILE *f = fopen(real, "wb");
    if (f == NULL) {
        LOG_E("sdmc: fopen(%s, wb) failed (errno %d)", real, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    s->write_file     = f;
    s->write_declared = declared_size;
    s->write_received = 0;
    snprintf(s->write_path, sizeof(s->write_path), "%s", path);

    LOG_D("sdmc: write_begin %s (%llu bytes declared)",
          path, (unsigned long long)declared_size);
    return 0;
}

static Result sdmc_write_chunk(NexusStorage *self, const void *buffer, size_t size)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;
    if (s->write_file == NULL) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (size == 0) return 0;

    const size_t written = fwrite(buffer, 1, size, s->write_file);
    if (written != size) {
        LOG_E("sdmc: short write on %s (%zu of %zu)", s->write_path, written, size);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    s->write_received += written;
    return 0;
}

static Result sdmc_write_end(NexusStorage *self, bool committed)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;
    if (s->write_file == NULL) return 0;

    fclose(s->write_file);
    s->write_file = NULL;

    if (!committed) {
        // Remove the partial file so the host does not see a truncated object.
        char real[NEXUS_PATH_MAX + 32];
        if (sdmc_real_path(s, s->write_path, real, sizeof(real))) {
            remove(real);
        }
        LOG_W("sdmc: write cancelled, removed %s", s->write_path);
    } else {
        LOG_I("sdmc: wrote %s (%llu bytes)", s->write_path,
              (unsigned long long)s->write_received);
    }

    s->write_path[0]  = '\0';
    s->write_declared = 0;
    s->write_received = 0;
    return 0;
}

static Result sdmc_mkdir(NexusStorage *self, const char *path)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    if (mkdir(real, 0777) != 0 && errno != EEXIST) {
        LOG_E("sdmc: mkdir(%s) failed (errno %d)", real, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    return 0;
}

static Result sdmc_remove(NexusStorage *self, const char *path, bool is_dir)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    char real[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, path, real, sizeof(real))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int r = is_dir ? rmdir(real) : remove(real);
    if (r != 0) {
        LOG_W("sdmc: remove(%s) failed (errno %d)", real, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    return 0;
}

static Result sdmc_move(NexusStorage *self, const char *from, const char *to, bool is_dir)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;
    (void)is_dir;

    char real_from[NEXUS_PATH_MAX + 32];
    char real_to[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, from, real_from, sizeof(real_from))
        || !sdmc_real_path(s, to, real_to, sizeof(real_to))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    if (rename(real_from, real_to) != 0) {
        LOG_W("sdmc: rename(%s -> %s) failed (errno %d)", real_from, real_to, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    LOG_D("sdmc: moved %s -> %s", from, to);
    return 0;
}

static Result sdmc_copy(NexusStorage *self, const char *from, const char *to)
{
    SdmcStorage *s = (SdmcStorage *)self->impl;

    char real_from[NEXUS_PATH_MAX + 32];
    char real_to[NEXUS_PATH_MAX + 32];
    if (!sdmc_real_path(s, from, real_from, sizeof(real_from))
        || !sdmc_real_path(s, to, real_to, sizeof(real_to))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    FILE *src = fopen(real_from, "rb");
    if (src == NULL) return MAKERESULT(Module_Libnx, LibnxError_IoError);

    FILE *dst = fopen(real_to, "wb");
    if (dst == NULL) {
        fclose(src);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    // 256 KiB keeps the SD card happy without a large allocation. This runs on
    // the MTP worker thread, so a multi-gigabyte copy blocks that transaction
    // for as long as it takes -- which is what the host expects of CopyObject.
    const size_t buf_size = 256 * 1024;
    u8 *buf = (u8 *)malloc(buf_size);
    if (buf == NULL) {
        fclose(src);
        fclose(dst);
        remove(real_to);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    Result rc = 0;
    for (;;) {
        const size_t got = fread(buf, 1, buf_size, src);
        if (got == 0) {
            if (ferror(src)) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
        if (fwrite(buf, 1, got, dst) != got) {
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
    }

    free(buf);
    fclose(src);
    fclose(dst);

    // Never leave a half-written copy behind.
    if (R_FAILED(rc)) {
        remove(real_to);
        LOG_W("sdmc: copy(%s -> %s) failed", from, to);
    }
    return rc;
}

static const NexusStorageOps g_sdmc_ops = {
    .description = sdmc_description,
    .get_info    = sdmc_get_info,
    .enumerate   = sdmc_enumerate,
    .stat        = sdmc_stat,
    .read        = sdmc_read,
    .write_begin = sdmc_write_begin,
    .write_chunk = sdmc_write_chunk,
    .write_end   = sdmc_write_end,
    .mkdir       = sdmc_mkdir,
    .remove      = sdmc_remove,
    .move        = sdmc_move,
    .copy        = sdmc_copy,
};

Result nexusStorageSdmcCreate(NexusStorage *out, u32 storage_id,
                              const char *mount, const char *description)
{
    SdmcStorage *s = (SdmcStorage *)calloc(1, sizeof(SdmcStorage));
    if (s == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    snprintf(s->mount,       sizeof(s->mount),       "%s", mount);
    snprintf(s->description, sizeof(s->description), "%s", description);

    out->storage_id = storage_id;
    out->ops        = &g_sdmc_ops;
    out->impl       = s;

    // Probe the mount so an absent SD card shows up as an absent store rather
    // than a store that errors on every operation.
    char root[32];
    snprintf(root, sizeof(root), "%s/", mount);
    struct stat sb;
    out->present = (stat(root, &sb) == 0);

    LOG_I("storage: %s (%s) %s", description, mount,
          out->present ? "mounted" : "NOT PRESENT");
    return 0;
}
