// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- read-only virtual store over installed game saves.
//
// The root lists one folder per save, named after the game rather than its
// title id, so a backup is recognisable without a lookup table. Each folder
// contains that save's files.
//
// Reading is done through a read-only mount, so browsing and backing up can
// never touch a save.
//
// Restoring is supported but treated carefully, because a half-written save is
// worse than no save: each incoming file is written to a ".nxtmp" scratch name
// and only renamed over the real one once the whole transfer has arrived and
// the filesystem has been committed. A transfer that dies partway leaves the
// original file untouched and a stray .nxtmp behind, which the next restore
// cleans up.
//
// Note on permissions: enumerating and mounting other titles' saves needs FS
// permissions that a plain hbmenu launch may not have. When that is the cause,
// fsOpenSaveDataInfoReader fails and the exact Result is logged -- the store
// still appears, empty, rather than silently vanishing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/storage.h"
#include "nexus/fs_helpers.h"
#include "nexus/mtp_types.h"
#include "nexus/format.h"
#include "nexus/log.h"

// Enough for a large library; saves beyond this are not listed.
#define SAVES_MAX 512

// "Some Very Long Game Name [0100000000010000] (user 00112233)" plus room for
// a disambiguating " (2)" suffix.
//
// The sizes are chosen so composition can never truncate: a title of at most
// TITLE_MAX-1 characters plus the fixed suffixes always fits in SAVE_BASE_LEN,
// and that plus " (99)" always fits in SAVE_NAME_LEN.
#define SAVE_NAME_LEN 192
#define SAVE_BASE_LEN (SAVE_NAME_LEN - 8)
#define TITLE_MAX     96

typedef struct {
    char       name[SAVE_NAME_LEN];
    u64        application_id;
    AccountUid uid;
    u8         save_data_type;
} SaveEntry;

typedef struct {
    char description[64];

    SaveEntry entries[SAVES_MAX];
    u32       entry_count;
    bool      listed;
    Result    last_error;

    // One save is kept mounted at a time.
    FsFileSystem fs;
    bool         mounted;
    bool         mounted_writable;
    u32          mounted_index;

    // In-flight restore.
    FsFile file;
    bool   file_open;
    u64    write_offset;
    char   temp_path[FS_MAX_PATH];
    char   final_path[FS_MAX_PATH];
} SavesStorage;

// Looks up a title's display name. Returns false when ns cannot answer, which
// is normal for saves whose title is no longer installed.
static bool lookup_title_name(u64 application_id, char *out, size_t out_size)
{
    // NsApplicationControlData is ~128 KiB because it carries the icon, so it
    // must not go on the stack.
    NsApplicationControlData *data =
        (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));
    if (data == NULL) return false;

    bool ok = false;
    u64 actual = 0;

    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                application_id, data,
                                                sizeof(*data), &actual))
        && actual >= sizeof(data->nacp)) {

        NacpLanguageEntry *entry = NULL;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry))
            && entry != NULL && entry->name[0] != '\0') {
            nexusSanitiseUtf8(entry->name, out, out_size);
            ok = (out[0] != '\0');
        }
    }

    free(data);
    return ok;
}

// Builds a unique, human-readable folder name for one save.
static void build_entry_name(SavesStorage *s, const FsSaveDataInfo *info,
                             char *out, size_t out_size)
{
    char title[TITLE_MAX];
    if (!lookup_title_name(info->application_id, title, sizeof(title))) {
        snprintf(title, sizeof(title), "Unknown");
    }

    // The title id stays in the name: it is what makes a backup identifiable
    // later, and two different titles can share a display name.
    char base[SAVE_BASE_LEN];
    if (info->save_data_type == FsSaveDataType_Device) {
        snprintf(base, sizeof(base), "%s [%016llx] (device)",
                 title, (unsigned long long)info->application_id);
    } else {
        snprintf(base, sizeof(base), "%s [%016llx] (user %08llx)",
                 title, (unsigned long long)info->application_id,
                 (unsigned long long)(info->uid.uid[0] >> 32));
    }

    snprintf(out, out_size, "%s", base);

    // Guarantee uniqueness: MTP object handles are interned by path, so two
    // identical folder names would collapse into one.
    for (u32 n = 2; n < 100; n++) {
        bool clash = false;
        for (u32 i = 0; i < s->entry_count; i++) {
            if (strcmp(s->entries[i].name, out) == 0) { clash = true; break; }
        }
        if (!clash) return;

        snprintf(out, out_size, "%s (%u)", base, n);
    }
}

// Walks the savedata index and caches what it finds. Re-listing on every MTP
// operation would be far too slow, and the name lookups are not cheap.
static Result saves_list(SavesStorage *s)
{
    if (s->listed) return s->last_error;

    s->entry_count = 0;
    s->last_error  = 0;

    FsSaveDataInfoReader reader;
    Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(rc)) {
        // Almost always a permissions problem: a plain hbmenu launch may not
        // be allowed to enumerate other titles' savedata.
        LOG_E("saves: OpenSaveDataInfoReader failed (0x%x) -- this usually means "
              "the app lacks FS permissions for savedata", rc);
        s->listed     = true;   // do not retry on every single MTP call
        s->last_error = rc;
        return rc;
    }

    FsSaveDataInfo info;
    s64 read_count = 0;
    u32 skipped = 0;

    while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &read_count))
           && read_count > 0) {

        // Account saves are the per-user game saves people want to back up;
        // device saves belong to the console rather than a user but are still
        // real game data. Everything else (system, bcat, cache, temporary) is
        // noise for this purpose.
        if (info.save_data_type != FsSaveDataType_Account
            && info.save_data_type != FsSaveDataType_Device) {
            skipped++;
            continue;
        }
        if (info.application_id == 0) { skipped++; continue; }

        if (s->entry_count >= SAVES_MAX) {
            LOG_W("saves: listing truncated at %d entries", SAVES_MAX);
            break;
        }

        SaveEntry *e = &s->entries[s->entry_count];
        e->application_id = info.application_id;
        e->uid            = info.uid;
        e->save_data_type = info.save_data_type;
        build_entry_name(s, &info, e->name, sizeof(e->name));

        s->entry_count++;
    }

    fsSaveDataInfoReaderClose(&reader);

    s->listed = true;
    LOG_I("saves: %u save(s) listed (%u other entries skipped)", s->entry_count, skipped);
    return 0;
}

static int saves_find(SavesStorage *s, const char *name)
{
    for (u32 i = 0; i < s->entry_count; i++) {
        if (strcmp(s->entries[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void saves_unmount(SavesStorage *s)
{
    if (s->file_open) {
        fsFileClose(&s->file);
        s->file_open = false;
    }
    if (s->mounted) {
        fsFsClose(&s->fs);
        s->mounted          = false;
        s->mounted_writable = false;
    }
}

// Mounts a save. writable selects a read/write mount, which is only used by
// the restore path -- browsing and backup always get the read-only mount.
static bool saves_mount_mode(SavesStorage *s, u32 index, bool writable)
{
    if (index >= s->entry_count) return false;
    if (s->mounted && s->mounted_index == index && s->mounted_writable == writable) {
        return true;
    }

    saves_unmount(s);

    const SaveEntry *e = &s->entries[index];

    Result rc;
    if (e->save_data_type == FsSaveDataType_Device) {
        // Device saves have no read-only variant.
        rc = fsOpen_DeviceSaveData(&s->fs, e->application_id);
    } else if (writable) {
        rc = fsOpen_SaveData(&s->fs, e->application_id, e->uid);
    } else {
        rc = fsOpen_SaveDataReadOnly(&s->fs, e->application_id, e->uid);
    }

    if (R_FAILED(rc)) {
        LOG_W("saves: opening \"%s\" %s failed (0x%x)", e->name,
              writable ? "for writing" : "read-only", rc);
        return false;
    }

    s->mounted          = true;
    s->mounted_index    = index;
    s->mounted_writable = writable;
    return true;
}

static bool saves_mount(SavesStorage *s, u32 index)
{
    return saves_mount_mode(s, index, false);
}

static const char *saves_description(NexusStorage *self)
{
    return ((SavesStorage *)self->impl)->description;
}

static Result saves_get_info(NexusStorage *self, NexusStorageInfo *out)
{
    (void)self;

    memset(out, 0, sizeof(*out));
    out->storage_type      = MtpStorageType_FixedRAM;
    out->filesystem_type   = MtpFsType_GenericHierarchical;
    out->access_capability = MtpAccess_ReadWrite;
    out->free_objects      = 0xFFFFFFFFu;

    // Reporting 0 of 0 makes a host draw the store as a full, red volume.
    // Saves live on the internal user partition, so report that instead --
    // which is both accurate and reassuring to look at.
    NcmContentStorage cs;
    if (R_SUCCEEDED(ncmOpenContentStorage(&cs, NcmStorageId_BuiltInUser))) {
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

static Result saves_enumerate(NexusStorage *self, const char *dir_path,
                              NexusEnumCallback cb, void *user)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    // A listing failure must not fail enumeration outright -- an empty folder
    // is far easier to diagnose than a store that errors on open.
    saves_list(s);

    if (strcmp(dir_path, "/") == 0) {
        for (u32 i = 0; i < s->entry_count; i++) {
            if (!cb(user, s->entries[i].name, true, 0)) break;
        }
        return 0;
    }

    char head[SAVE_NAME_LEN], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(dir_path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = saves_find(s, head);
    if (index < 0 || !saves_mount(s, (u32)index)) {
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    return nexusFsEnumerate(&s->fs, tail, cb, user);
}

static Result saves_stat(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    if (strcmp(path, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    saves_list(s);

    char head[SAVE_NAME_LEN], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = saves_find(s, head);
    if (index < 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    // The save's own folder.
    if (strcmp(tail, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    if (!saves_mount(s, (u32)index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return nexusFsStat(&s->fs, tail, out_is_dir, out_size);
}

static Result saves_read(NexusStorage *self, const char *path, u64 offset,
                         void *buffer, size_t size, size_t *out_read)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    saves_list(s);

    char head[SAVE_NAME_LEN], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = saves_find(s, head);
    if (index < 0 || !saves_mount(s, (u32)index)) {
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    return nexusFsRead(&s->fs, tail, offset, buffer, size, out_read);
}


// ---------------------------------------------------------------------------
// Restore
//
// Writes land on a ".nxtmp" scratch file and are only renamed over the real
// target once the whole transfer has arrived. That keeps a failed restore from
// destroying an existing save.
// ---------------------------------------------------------------------------

static Result saves_write_begin(NexusStorage *self, const char *path, u64 declared_size)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    saves_list(s);

    char head[SAVE_NAME_LEN], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = saves_find(s, head);
    if (index < 0) {
        // Creating a whole new save would mean choosing a size and owner, which
        // this store has no way to know. Restores go into an existing save.
        LOG_W("saves: cannot restore into \"%s\" -- no such save on this console", head);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    if (strcmp(tail, "/") == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (!saves_mount_mode(s, (u32)index, true)) {
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    snprintf(s->final_path, sizeof(s->final_path), "%s", tail);
    const int n = snprintf(s->temp_path, sizeof(s->temp_path), "%s.nxtmp", tail);
    if (n <= 0 || (size_t)n >= sizeof(s->temp_path)) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // A leftover scratch file from an interrupted restore must not block this
    // one; deleting it is safe because it was never a real save file.
    fsFsDeleteFile(&s->fs, s->temp_path);

    // Savedata filesystems are fixed-size, so the file is created at its final
    // size up front: if the save cannot fit it, this fails now rather than
    // halfway through.
    const s64 size = (declared_size == 0xFFFFFFFFull) ? 0 : (s64)declared_size;
    Result rc = fsFsCreateFile(&s->fs, s->temp_path, size, 0);
    if (R_FAILED(rc)) {
        LOG_E("saves: creating %s failed (0x%x)", s->temp_path, rc);
        return rc;
    }

    rc = fsFsOpenFile(&s->fs, s->temp_path, FsOpenMode_Write | FsOpenMode_Append, &s->file);
    if (R_FAILED(rc)) {
        LOG_E("saves: opening %s for writing failed (0x%x)", s->temp_path, rc);
        fsFsDeleteFile(&s->fs, s->temp_path);
        return rc;
    }

    s->file_open    = true;
    s->write_offset = 0;

    LOG_I("saves: restoring %s into \"%s\"", tail, s->entries[index].name);
    return 0;
}

static Result saves_write_chunk(NexusStorage *self, const void *buffer, size_t size)
{
    SavesStorage *s = (SavesStorage *)self->impl;
    if (!s->file_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (size == 0) return 0;

    const Result rc = fsFileWrite(&s->file, (s64)s->write_offset, buffer, size,
                                  FsWriteOption_None);
    if (R_FAILED(rc)) {
        LOG_E("saves: write failed at offset %llu (0x%x)",
              (unsigned long long)s->write_offset, rc);
        return rc;
    }

    s->write_offset += size;
    return 0;
}

static Result saves_write_end(NexusStorage *self, bool committed)
{
    SavesStorage *s = (SavesStorage *)self->impl;
    if (!s->file_open) return 0;

    fsFileClose(&s->file);
    s->file_open = false;

    if (!committed) {
        LOG_W("saves: restore cancelled, leaving the original file untouched");
        fsFsDeleteFile(&s->fs, s->temp_path);
        return 0;
    }

    // Replace the target only now that every byte has arrived.
    fsFsDeleteFile(&s->fs, s->final_path);

    Result rc = fsFsRenameFile(&s->fs, s->temp_path, s->final_path);
    if (R_FAILED(rc)) {
        LOG_E("saves: renaming %s -> %s failed (0x%x)",
              s->temp_path, s->final_path, rc);
        fsFsDeleteFile(&s->fs, s->temp_path);
        return rc;
    }

    // Savedata is journalled: without a commit the write is rolled back when
    // the filesystem is closed.
    rc = fsFsCommit(&s->fs);
    if (R_FAILED(rc)) {
        LOG_E("saves: commit failed (0x%x) -- the restore did not take", rc);
        return rc;
    }

    LOG_I("saves: restored %s (%llu bytes)", s->final_path,
          (unsigned long long)s->write_offset);
    return 0;
}

static Result saves_mkdir(NexusStorage *self, const char *path)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    saves_list(s);

    char head[SAVE_NAME_LEN], tail[FS_MAX_PATH];
    if (!nexusFsSplitPath(path, head, sizeof(head), tail, sizeof(tail))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const int index = saves_find(s, head);
    if (index < 0 || strcmp(tail, "/") == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (!saves_mount_mode(s, (u32)index, true)) {
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    Result rc = fsFsCreateDirectory(&s->fs, tail);
    if (R_SUCCEEDED(rc)) fsFsCommit(&s->fs);
    return rc;
}

static const NexusStorageOps g_saves_ops = {
    .description = saves_description,
    .get_info    = saves_get_info,
    .enumerate   = saves_enumerate,
    .stat        = saves_stat,
    .read        = saves_read,
    // Restore, guarded by the scratch-file dance described above.
    .write_begin = saves_write_begin,
    .write_chunk = saves_write_chunk,
    .write_end   = saves_write_end,
    .mkdir       = saves_mkdir,
    // Deleting or renaming inside a live save is not offered: there is no use
    // case that outweighs the risk of losing progress.
    .remove      = NULL,
    .move        = NULL,
    .copy        = NULL,
};

Result nexusStorageSavesCreate(NexusStorage *out, u32 storage_id, const char *description)
{
    SavesStorage *s = (SavesStorage *)calloc(1, sizeof(SavesStorage));
    if (s == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    snprintf(s->description, sizeof(s->description), "%s", description);

    out->storage_id = storage_id;
    out->ops        = &g_saves_ops;
    out->impl       = s;

    // The store is always advertised. Hiding it when the listing comes back
    // empty makes a permissions failure look identical to "you have no saves",
    // which is exactly the confusion this is meant to avoid.
    out->present = true;

    LOG_I("storage: %s registered (listing deferred to first use)", description);
    return 0;
}

void nexusStorageSavesInvalidate(NexusStorage *self)
{
    SavesStorage *s = (SavesStorage *)self->impl;

    saves_unmount(s);
    s->listed      = false;
    s->entry_count = 0;
    s->last_error  = 0;
}
