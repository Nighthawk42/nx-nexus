// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- fsp-srv commands missing from libnx.
//
// nexusFsOpenGameCardStorage is ported from nxdumptool's fs_ext.c
// (Copyright DarkMatterCore, GPL-3.0-or-later). See NOTICE.

#include "nexus/fs_ext.h"

Result nexusFsOpenGameCardStorage(FsStorage *out, const FsGameCardHandle *handle,
                                  u32 partition)
{
    const struct {
        FsGameCardHandle handle;
        u32              partition;
    } in = { *handle, partition };

    return serviceDispatchIn(fsGetServiceSession(), 30, in,
        .out_num_objects = 1,
        .out_objects     = &out->s
    );
}
