// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- fsp-srv commands missing from libnx.
//
// libnx wraps fsOpenGameCardFileSystem (the mounted partitions) but not
// fsOpenGameCardStorage (raw sector access), which is what a byte-exact XCI
// dump needs. The IPC below is ported from nxdumptool (GPL-3.0) -- see NOTICE.
#pragma once

#include <switch.h>

/// fsp-srv command 30: OpenGameCardStorage.
/// partition selects the raw view: 0 = normal area, 1 = secure area,
/// 2 = the whole card image.
Result nexusFsOpenGameCardStorage(FsStorage *out, const FsGameCardHandle *handle,
                                  u32 partition);

// Raw gamecard storage areas, as accepted by the command above.
typedef enum {
    NexusGcStorage_Normal = 0,
    NexusGcStorage_Secure = 1,
    NexusGcStorage_Full   = 2,
} NexusGcStorageArea;
