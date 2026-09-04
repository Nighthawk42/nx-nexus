// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- read-only traversal over a mounted FsFileSystem.

#include <stdio.h>
#include <string.h>

#include "nexus/fs_helpers.h"
#include "nexus/log.h"

// Reading one entry at a time keeps the stack small; these directories hold a
// handful of files, so the extra IPC round trips do not matter.
#define DIR_BATCH 1

Result nexusFsEnumerate(FsFileSystem *fs, const char *path,
                        NexusEnumCallback cb, void *user)
{
    if (fs == NULL || path == NULL || cb == NULL) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // fs paths must be mutable in the IPC call, and are always absolute here.
    char fs_path[FS_MAX_PATH];
    snprintf(fs_path, sizeof(fs_path), "%s", path);

    FsDir dir;
    Result rc = fsFsOpenDirectory(fs, fs_path,
                                  FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) return rc;

    FsDirectoryEntry entry;
    s64 read_count = 0;

    while (R_SUCCEEDED(fsDirRead(&dir, &read_count, DIR_BATCH, &entry)) && read_count > 0) {
        const bool is_dir = (entry.type == FsDirEntryType_Dir);
        if (!cb(user, entry.name, is_dir, is_dir ? 0 : entry.file_size)) break;
    }

    fsDirClose(&dir);
    return 0;
}

Result nexusFsStat(FsFileSystem *fs, const char *path, bool *out_is_dir, s64 *out_size)
{
    if (fs == NULL || path == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (strcmp(path, "/") == 0) {
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    char fs_path[FS_MAX_PATH];
    snprintf(fs_path, sizeof(fs_path), "%s", path);

    // Try it as a file first, which is the common case and gives us the size
    // in the same call.
    FsFile file;
    if (R_SUCCEEDED(fsFsOpenFile(fs, fs_path, FsOpenMode_Read, &file))) {
        s64 size = 0;
        const Result rc = fsFileGetSize(&file, &size);
        fsFileClose(&file);
        if (R_FAILED(rc)) return rc;

        if (out_is_dir) *out_is_dir = false;
        if (out_size)   *out_size   = size;
        return 0;
    }

    FsDir dir;
    if (R_SUCCEEDED(fsFsOpenDirectory(fs, fs_path, FsDirOpenMode_ReadDirs, &dir))) {
        fsDirClose(&dir);
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result nexusFsRead(FsFileSystem *fs, const char *path, u64 offset,
                   void *buffer, size_t size, size_t *out_read)
{
    if (out_read) *out_read = 0;
    if (fs == NULL || path == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    char fs_path[FS_MAX_PATH];
    snprintf(fs_path, sizeof(fs_path), "%s", path);

    FsFile file;
    Result rc = fsFsOpenFile(fs, fs_path, FsOpenMode_Read, &file);
    if (R_FAILED(rc)) return rc;

    u64 got = 0;
    rc = fsFileRead(&file, (s64)offset, buffer, size, FsReadOption_None, &got);
    fsFileClose(&file);

    if (R_SUCCEEDED(rc) && out_read) *out_read = (size_t)got;
    return rc;
}

bool nexusFsSplitPath(const char *path, char *head, size_t head_size,
                      char *tail, size_t tail_size)
{
    if (path == NULL || path[0] != '/') return false;
    if (head_size == 0 || tail_size == 0) return false;

    const char *start = path + 1;
    const char *slash = strchr(start, '/');

    const size_t head_len = (slash != NULL) ? (size_t)(slash - start) : strlen(start);
    if (head_len >= head_size) return false;

    memcpy(head, start, head_len);
    head[head_len] = '\0';

    // Everything after the first component, always rooted so it can be handed
    // straight to the fs calls.
    if (slash == NULL || slash[1] == '\0') {
        snprintf(tail, tail_size, "/");
    } else {
        const int n = snprintf(tail, tail_size, "%s", slash);
        if (n < 0 || (size_t)n >= tail_size) return false;
    }

    return true;
}
