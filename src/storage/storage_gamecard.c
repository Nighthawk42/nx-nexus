// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- read-only virtual store over an inserted game card.
//
// The card's partitions appear as top-level folders, so dragging the "secure"
// folder to a PC dumps the NCAs that make up the cartridge.
//
// The root also carries a synthetic "card.xci" -- a byte-exact image of the
// whole cartridge, served straight from raw sector reads. libnx has no raw
// gamecard API, so the fsp-srv command behind it is hand-rolled in
// src/horizon/fs_ext.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/storage.h"
#include "nexus/fs_helpers.h"
#include "nexus/fs_ext.h"
#include "nexus/mtp_types.h"
#include "nexus/log.h"

// Name of the synthetic whole-card image at the store root.
#define GC_XCI_NAME "card.xci"

typedef struct {
    const char         *name;
    FsGameCardPartition partition;
} GcPartition;

// Update and Logo are frequently absent; an empty or unmountable partition is
// simply skipped during enumeration rather than treated as an error.
static const GcPartition g_partitions[] = {
    { "secure", FsGameCardPartition_Secure },
    { "normal", FsGameCardPartition_Normal },
    { "update", FsGameCardPartition_Update },
    { "logo",   FsGameCardPartition_Logo   },
};
#define GC_PARTITION_COUNT (sizeof(g_partitions) / sizeof(g_partitions[0]))

typedef struct {
    char description[64];

    // One partition is kept mounted at a time; streaming a dump touches the
    // same partition repeatedly, so remounting per read would be painful.
    FsFileSystem fs;
    bool         mounted;
    int          mounted_index;

    // Raw whole-card storage, opened lazily for the .xci image.
    FsStorage raw;
    bool      raw_open;
    s64       raw_size;
} GamecardStorage;

static void gc_raw_close(GamecardStorage *s)
{
    if (s->raw_open) {
        fsStorageClose(&s->raw);
        s->raw_open = false;
        s->raw_size = 0;
    }
}

// Opens the whole-card raw view. Returns the image size, or -1 when raw access
// is unavailable (no card, or insufficient FS permissions).
static s64 gc_raw_open(GamecardStorage *s)
{
    if (s->raw_open) return s->raw_size;

    FsDeviceOperator op;
    if (R_FAILED(fsOpenDeviceOperator(&op))) return -1;

    FsGameCardHandle handle;
    const Result hrc = fsDeviceOperatorGetGameCardHandle(&op, &handle);
    fsDeviceOperatorClose(&op);
    if (R_FAILED(hrc)) return -1;

    if (R_FAILED(nexusFsOpenGameCardStorage(&s->raw, &handle, NexusGcStorage_Full))) {
        LOG_D("gamecard: raw storage unavailable");
        return -1;
    }

    s64 size = 0;
    if (R_FAILED(fsStorageGetSize(&s->raw, &size)) || size <= 0) {
        fsStorageClose(&s->raw);
        return -1;
    }

    s->raw_open = true;
    s->raw_size = size;
    return size;
}

static bool gamecard_inserted(void)
{
    FsDeviceOperator op;
    if (R_FAILED(fsOpenDeviceOperator(&op))) return false;

    bool inserted = false;
    const Result rc = fsDeviceOperatorIsGameCardInserted(&op, &inserted);
    fsDeviceOperatorClose(&op);

    return R_SUCCEEDED(rc) && inserted;
}

static int find_partition(const char *name)
{
    for (size_t i = 0; i < GC_PARTITION_COUNT; i++) {
        if (strcmp(g_partitions[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void gc_unmount(GamecardStorage *s)
{
    if (s->mounted) {
        fsFsClose(&s->fs);
        s->mounted       = false;
        s->mounted_index = -1;
    }
}

// Drops both the mounted partition and the raw view; used when the card is
// swapped, since every handle into the old card is now meaningless.
static void gc_release_all(GamecardStorage *s)
{
    gc_unmount(s);
    gc_raw_close(s);
}

// Mounts a partition, reusing the existing mount when it is already the one
// wanted. Returns false when the card is gone or the partition is absent.
static bool gc_mount(GamecardStorage *s, int index)
{
    if (index < 0 || (size_t)index >= GC_PARTITION_COUNT) return false;
    if (s->mounted && s->mounted_index == index) return true;

    gc_unmount(s);

    FsDeviceOperator op;
    if (R_FAILED(fsOpenDeviceOperator(&op))) return false;

    FsGameCardHandle handle;
    const Result rc = fsDeviceOperatorGetGameCardHandle(&op, &handle);
    fsDeviceOperatorClose(&op);
    if (R_FAILED(rc)) return false;

    if (R_FAILED(fsOpenGameCardFileSystem(&s->fs, &handle, g_partitions[index].partition))) {
        return false;
    }

    s->mounted       = true;
    s->mounted_index = index;
    return true;
}

static const char *gc_description(NexusStorage *self)
{
    return ((GamecardStorage *)self->impl)->description;
}

static Result gc_get_info(NexusStorage *self, NexusStorageInfo *out)
{
    (void)self;

    memset(out, 0, sizeof(*out));
    out->storage_type      = MtpStorageType_RemovableROM;
    out->filesystem_type   = MtpFsType_GenericHierarchical;
    out->access_capability = MtpAccess_ReadOnly;
    out->free_objects      = 0;
    out->free_bytes        = 0;

    // The card's total size is not available without raw storage access, so
    // report it as unknown rather than inventing a number.
    out->capacity_bytes = 0;
    return 0;
}

static Result gc_enumerate(NexusStorage *self, const char *dir_path,
                           NexusEnumCallback cb, void *user)
{
    GamecardStorage *s = (GamecardStorage *)self->impl;

    // The root lists the partitions that actually mount, plus the whole-card
    // image when raw access is available.
    if (strcmp(dir_path, "/") == 0) {
        const s64 raw_size = gc_raw_open(s);
        if (raw_size > 0 && !cb(user, GC_XCI_NAME, false, raw_size)) return 0;

        for (size_t i = 0; i < GC_PARTITION_COUNT; i++) {
            if (!gc_mount(s, (int)i)) continue;
            if (!cb(user, g_partitions[i].name, true, 0)) break;
        }
        return 0;
    }

    char head[64], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(dir_path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = find_partition(head);
    if (index < 0 || !gc_mount(s, index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    return nexusFsEnumerate(&s->fs, tail, cb, user);
}

static Result gc_stat(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size)
{
    GamecardStorage *s = (GamecardStorage *)self->impl;

    if (strcmp(path, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    if (strcmp(path, "/" GC_XCI_NAME) == 0) {
        const s64 raw_size = gc_raw_open(s);
        if (raw_size <= 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        if (out_is_dir) *out_is_dir = false;
        if (out_size)   *out_size   = raw_size;
        return 0;
    }

    char head[64], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = find_partition(head);
    if (index < 0 || !gc_mount(s, index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    // The partition folder itself.
    if (strcmp(tail, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    return nexusFsStat(&s->fs, tail, out_is_dir, out_size);
}

static Result gc_read(NexusStorage *self, const char *path, u64 offset,
                      void *buffer, size_t size, size_t *out_read)
{
    GamecardStorage *s = (GamecardStorage *)self->impl;

    // The whole-card image is served straight from raw sector reads.
    if (strcmp(path, "/" GC_XCI_NAME) == 0) {
        const s64 raw_size = gc_raw_open(s);
        if (raw_size <= 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        if ((s64)offset >= raw_size) return 0;   // clean EOF

        // fsStorageRead fills the request or fails, so clamp at the end.
        size_t chunk = size;
        if ((u64)chunk > (u64)raw_size - offset) chunk = (size_t)((u64)raw_size - offset);

        if (R_FAILED(fsStorageRead(&s->raw, (s64)offset, buffer, chunk))) {
            LOG_W("gamecard: raw read failed at offset %llu",
                  (unsigned long long)offset);
            return MAKERESULT(Module_Libnx, LibnxError_IoError);
        }

        if (out_read) *out_read = chunk;
        return 0;
    }

    char head[64], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = find_partition(head);
    if (index < 0 || !gc_mount(s, index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    return nexusFsRead(&s->fs, tail, offset, buffer, size, out_read);
}

static const NexusStorageOps g_gamecard_ops = {
    .description = gc_description,
    .get_info    = gc_get_info,
    .enumerate   = gc_enumerate,
    .stat        = gc_stat,
    .read        = gc_read,
    // Read-only: every write hook stays NULL, which the MTP layer reports as
    // StoreReadOnly.
    .write_begin = NULL,
    .write_chunk = NULL,
    .write_end   = NULL,
    .mkdir       = NULL,
    .remove      = NULL,
};

Result nexusStorageGamecardCreate(NexusStorage *out, u32 storage_id, const char *description)
{
    GamecardStorage *s = (GamecardStorage *)calloc(1, sizeof(GamecardStorage));
    if (s == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    s->mounted_index = -1;
    snprintf(s->description, sizeof(s->description), "%s", description);

    out->storage_id = storage_id;
    out->ops        = &g_gamecard_ops;
    out->impl       = s;
    out->present    = gamecard_inserted();

    LOG_I("storage: %s %s", description, out->present ? "card inserted" : "no card");
    return 0;
}

void nexusStorageGamecardRefresh(NexusStorage *self)
{
    GamecardStorage *s = (GamecardStorage *)self->impl;

    const bool inserted = gamecard_inserted();
    if (inserted == self->present) return;

    // A swapped card invalidates every handle we are holding.
    gc_release_all(s);
    self->present = inserted;
    LOG_I("storage: game card %s", inserted ? "inserted" : "removed");
}
