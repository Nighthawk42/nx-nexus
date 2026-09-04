// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- the Album: screenshots and capture videos.
//
// Horizon keeps captures in an "image directory" filesystem rather than a
// plain folder on the SD card, so reaching them means asking fsp-srv for it
// and mounting it under a devoptab name of our own.
//
// Once mounted it is an ordinary read/write directory tree, so this delegates
// entirely to the sdmc passthrough backend. That is the whole reason the
// backend takes a mount prefix: the Album needed nothing but a different one.

#include <stdio.h>
#include <string.h>

#include "nexus/storage.h"
#include "nexus/log.h"

#define ALBUM_MOUNT "nxalbum"

static bool g_mounted = false;

Result nexusStorageAlbumCreate(NexusStorage *out, u32 storage_id,
                               const char *description)
{
    if (!g_mounted) {
        FsFileSystem fs;

        // The SD album is the one users care about. NAND captures exist but are
        // only written when no card is present, and mounting both would show
        // two nearly identical stores.
        Result rc = fsOpenImageDirectoryFileSystem(&fs, FsImageDirectoryId_Sd);
        if (R_FAILED(rc)) {
            LOG_W("album: OpenImageDirectoryFileSystem failed (0x%x)", rc);
            return rc;
        }

        if (fsdevMountDevice(ALBUM_MOUNT, fs) < 0) {
            LOG_E("album: could not mount the image directory");
            fsFsClose(&fs);
            return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        }

        g_mounted = true;
        LOG_I("album: mounted as %s:", ALBUM_MOUNT);
    }

    return nexusStorageSdmcCreate(out, storage_id, ALBUM_MOUNT ":", description);
}

void nexusStorageAlbumDestroy(void)
{
    if (!g_mounted) return;

    fsdevUnmountDevice(ALBUM_MOUNT);
    g_mounted = false;
}
