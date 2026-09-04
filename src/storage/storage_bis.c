// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- read-only raw view of the active NAND's BIS partitions.
//
// IMPORTANT SCOPE NOTE: Horizon mounts exactly one NAND. If the console booted
// from an emuMMC, then this store -- like ncm, ns and everything else -- shows
// the *emuMMC*, and the real sysMMC is not reachable from inside the running
// system. There is no IPC that exposes the inactive one; browsing it means
// reading the raw eMMC or the SD image offline. The store name says which one
// you are looking at so a backup is never ambiguous.
//
// A file-based emuMMC's raw images live at sdmc:/emuMMC/RAWn/ and are already
// visible through the SD card store, if what you want is the image itself.
//
// PRODINFO (CalibrationBinary) holds console-unique certificates and keys.
// It is exposed because dumping it is the standard way to back one up before
// a NAND repair, but treat that file as you would a private key.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/storage.h"
#include "nexus/mtp_types.h"
#include "nexus/sysinfo.h"
#include "nexus/log.h"

typedef struct {
    const char       *name;
    FsBisPartitionId  id;
} BisPartition;

// Only the partitions that are useful to back up. The boot partitions and the
// package2 slots are deliberately included: they are what a NAND restore needs.
static const BisPartition g_partitions[] = {
    { "PRODINFO.bin",     FsBisPartitionId_CalibrationBinary               },
    { "PRODINFOF.bin",    FsBisPartitionId_CalibrationFile                 },
    { "SAFE.bin",         FsBisPartitionId_SafeMode                        },
    { "SYSTEM.bin",       FsBisPartitionId_System                          },
    { "USER.bin",         FsBisPartitionId_User                            },
    { "BCPKG2-1.bin",     FsBisPartitionId_BootConfigAndPackage2Part1      },
    { "BCPKG2-2.bin",     FsBisPartitionId_BootConfigAndPackage2Part2      },
    { "BCPKG2-3.bin",     FsBisPartitionId_BootConfigAndPackage2Part3      },
    { "BCPKG2-4.bin",     FsBisPartitionId_BootConfigAndPackage2Part4      },
    { "BCPKG2-5.bin",     FsBisPartitionId_BootConfigAndPackage2Part5      },
    { "BCPKG2-6.bin",     FsBisPartitionId_BootConfigAndPackage2Part6      },
    { "SYSTEM0.bin",      FsBisPartitionId_System0                         },
};
#define BIS_COUNT (sizeof(g_partitions) / sizeof(g_partitions[0]))

typedef struct {
    char description[80];

    // One partition open at a time; a dump reads the same one end to end.
    FsStorage storage;
    bool      open;
    int       open_index;
} BisStorage;

static int bis_find(const char *name)
{
    for (size_t i = 0; i < BIS_COUNT; i++) {
        if (strcmp(g_partitions[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void bis_close(BisStorage *s)
{
    if (s->open) {
        fsStorageClose(&s->storage);
        s->open       = false;
        s->open_index = -1;
    }
}

static bool bis_open(BisStorage *s, int index)
{
    if (index < 0 || (size_t)index >= BIS_COUNT) return false;
    if (s->open && s->open_index == index) return true;

    bis_close(s);

    const Result rc = fsOpenBisStorage(&s->storage, g_partitions[index].id);
    if (R_FAILED(rc)) {
        // Expected without elevated FS permissions.
        LOG_D("bis: opening %s failed (0x%x)", g_partitions[index].name, rc);
        return false;
    }

    s->open       = true;
    s->open_index = index;
    return true;
}

static s64 bis_size(BisStorage *s, int index)
{
    if (!bis_open(s, index)) return -1;

    s64 size = 0;
    if (R_FAILED(fsStorageGetSize(&s->storage, &size))) return -1;
    return size;
}

static const char *bis_description(NexusStorage *self)
{
    return ((BisStorage *)self->impl)->description;
}

static Result bis_get_info(NexusStorage *self, NexusStorageInfo *out)
{
    BisStorage *s = (BisStorage *)self->impl;

    memset(out, 0, sizeof(*out));
    out->storage_type      = MtpStorageType_FixedROM;
    out->filesystem_type   = MtpFsType_GenericFlat;   // a flat list of images
    out->access_capability = MtpAccess_ReadOnly;
    out->free_objects      = 0;

    // Total of every partition we can actually open. Zero free is the truth
    // for a read-only raw view, and a host draws that the same way it draws
    // any read-only volume.
    for (size_t i = 0; i < BIS_COUNT; i++) {
        const s64 size = bis_size(s, (int)i);
        if (size > 0) out->capacity_bytes += (u64)size;
    }
    bis_close(s);

    out->free_bytes = 0;
    return 0;
}

static Result bis_enumerate(NexusStorage *self, const char *dir_path,
                            NexusEnumCallback cb, void *user)
{
    BisStorage *s = (BisStorage *)self->impl;

    if (strcmp(dir_path, "/") != 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    // Only list partitions that actually open, so a permissions-restricted
    // launch shows an empty store rather than a list of files that all fail.
    for (size_t i = 0; i < BIS_COUNT; i++) {
        const s64 size = bis_size(s, (int)i);
        if (size < 0) continue;
        if (!cb(user, g_partitions[i].name, false, size)) break;
    }

    return 0;
}

static Result bis_stat(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size)
{
    BisStorage *s = (BisStorage *)self->impl;

    if (strcmp(path, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }
    if (path[0] != '/') return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const int index = bis_find(path + 1);
    if (index < 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    const s64 size = bis_size(s, index);
    if (size < 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    if (out_is_dir) *out_is_dir = false;
    if (out_size)   *out_size   = size;
    return 0;
}

static Result bis_read(NexusStorage *self, const char *path, u64 offset,
                       void *buffer, size_t size, size_t *out_read)
{
    BisStorage *s = (BisStorage *)self->impl;
    if (out_read) *out_read = 0;

    if (path[0] != '/') return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const int index = bis_find(path + 1);
    if (index < 0 || !bis_open(s, index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    s64 total = 0;
    if (R_FAILED(fsStorageGetSize(&s->storage, &total))) {
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    if ((s64)offset >= total) return 0;   // clean EOF

    // fsStorageRead has no short-read concept: it either fills the request or
    // fails, so the tail of the partition must be clamped explicitly.
    size_t chunk = size;
    if ((u64)chunk > (u64)total - offset) chunk = (size_t)((u64)total - offset);

    if (R_FAILED(fsStorageRead(&s->storage, (s64)offset, buffer, chunk))) {
        LOG_W("bis: read failed at offset %llu of %s",
              (unsigned long long)offset, g_partitions[index].name);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    if (out_read) *out_read = chunk;
    return 0;
}

static const NexusStorageOps g_bis_ops = {
    .description = bis_description,
    .get_info    = bis_get_info,
    .enumerate   = bis_enumerate,
    .stat        = bis_stat,
    .read        = bis_read,
    .write_begin = NULL,   // never writable: a bad NAND write bricks the console
    .write_chunk = NULL,
    .write_end   = NULL,
    .mkdir       = NULL,
    .remove      = NULL,
    .move        = NULL,
    .copy        = NULL,
};

Result nexusStorageBisCreate(NexusStorage *out, u32 storage_id)
{
    BisStorage *s = (BisStorage *)calloc(1, sizeof(BisStorage));
    if (s == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    s->open_index = -1;

    // The name states which NAND this is, because that is the single most
    // important fact about a NAND backup.
    snprintf(s->description, sizeof(s->description), "7: NAND (%s)",
             nexusSysInfoStorageName());

    out->storage_id = storage_id;
    out->ops        = &g_bis_ops;
    out->impl       = s;

    // Probe one partition to decide whether BIS access is permitted at all.
    out->present = (bis_size(s, 0) >= 0);
    bis_close(s);

    LOG_I("storage: %s %s", s->description,
          out->present ? "accessible" : "NOT accessible (needs FS permissions)");
    return 0;
}
