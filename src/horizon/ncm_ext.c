// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- shared ncm helpers.

#include <stdio.h>
#include <string.h>

#include "nexus/ncm_ext.h"
#include "nexus/log.h"

Result nexusNcmReadCnmt(NcmContentStorage *cs, const NcmContentId *meta_id,
                        void *out, size_t cap, size_t *out_len)
{
    if (cs == NULL || meta_id == NULL || out == NULL || out_len == NULL) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_len = 0;

    char path[FS_MAX_PATH] = {0};
    Result rc = ncmContentStorageGetPath(cs, path, sizeof(path), meta_id);
    if (R_FAILED(rc)) {
        LOG_E("ncm: GetPath for the meta nca failed (0x%x)", rc);
        return rc;
    }

    // Mounting as ContentMeta is what makes this work without keys: Horizon
    // decrypts the NCA and exposes the plaintext .cnmt inside.
    FsFileSystem fs;
    rc = fsOpenFileSystemWithId(&fs, 0, FsFileSystemType_ContentMeta, path,
                                FsContentAttributes_All);
    if (R_FAILED(rc)) {
        LOG_E("ncm: mounting the meta nca failed (0x%x)", rc);
        return rc;
    }

    rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);

    FsDir dir;
    if (R_SUCCEEDED(fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir))) {
        FsDirectoryEntry entry;
        s64 read_count = 0;

        while (R_SUCCEEDED(fsDirRead(&dir, &read_count, 1, &entry)) && read_count > 0) {
            const size_t n = strlen(entry.name);
            if (n < 5 || strcmp(entry.name + (n - 5), ".cnmt") != 0) continue;

            char file_path[FS_MAX_PATH];
            snprintf(file_path, sizeof(file_path), "/%s", entry.name);

            FsFile file;
            if (R_FAILED(fsFsOpenFile(&fs, file_path, FsOpenMode_Read, &file))) break;

            s64 size = 0;
            if (R_SUCCEEDED(fsFileGetSize(&file, &size))
                && size > 0 && (size_t)size <= cap) {
                u64 got = 0;
                rc = fsFileRead(&file, 0, out, (u64)size, FsReadOption_None, &got);
                if (R_SUCCEEDED(rc)) *out_len = (size_t)got;
            } else {
                LOG_E("ncm: cnmt is %lld bytes, buffer is %zu", (long long)size, cap);
                rc = MAKERESULT(Module_Libnx, LibnxError_BufferProducerError);
            }

            fsFileClose(&file);
            break;
        }
        fsDirClose(&dir);
    }

    fsFsClose(&fs);

    if (R_FAILED(rc)) LOG_E("ncm: could not read the .cnmt from the meta nca");
    return rc;
}
