// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- storage maintenance.
//
// Interrupted installs leave debris: placeholder files that were never
// registered, and content that no meta record points at any more. Both consume
// space invisibly -- the system reports them as used, but nothing in the HOME
// menu accounts for them.
//
// Every operation here is scoped to that debris. Nothing touches content a
// title still references.

#include <stdio.h>
#include <string.h>

#include "nexus/maintenance.h"
#include "nexus/log.h"

static const NcmStorageId k_storages[] = {
    NcmStorageId_SdCard,
    NcmStorageId_BuiltInUser,
};
#define STORAGE_COUNT (sizeof(k_storages) / sizeof(k_storages[0]))

static const char *storage_name(NcmStorageId id)
{
    return (id == NcmStorageId_SdCard) ? "SD card" : "internal";
}

Result nexusMaintenanceScan(NexusMaintenanceReport *out)
{
    if (out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i < STORAGE_COUNT; i++) {
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, k_storages[i]))) continue;

        // Placeholders are always debris by the time we look: a live install
        // holds one only for the moments between create and register.
        NcmPlaceHolderId ids[64];
        s32 written = 0;
        if (R_SUCCEEDED(ncmContentStorageListPlaceHolder(&cs, ids,
                                                         (s32)(sizeof(ids) / sizeof(ids[0])),
                                                         &written))
            && written > 0) {
            out->placeholders += (u32)written;

            for (s32 k = 0; k < written; k++) {
                s64 size = 0;
                if (R_SUCCEEDED(ncmContentStorageGetSizeFromPlaceHolderId(&cs, &size, &ids[k]))
                    && size > 0) {
                    out->placeholder_bytes += (u64)size;
                }
            }
        }

        s64 freesp = 0;
        if (R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(&cs, &freesp))) {
            out->free_bytes += (u64)freesp;
        }

        ncmContentStorageClose(&cs);
    }

    LOG_I("maintenance: %u stray placeholder(s), %llu MiB",
          out->placeholders,
          (unsigned long long)(out->placeholder_bytes / (1024ull * 1024ull)));
    return 0;
}

Result nexusMaintenanceCleanPlaceholders(u32 *out_removed)
{
    u32 removed = 0;

    for (size_t i = 0; i < STORAGE_COUNT; i++) {
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, k_storages[i]))) continue;

        s32 before = 0;
        NcmPlaceHolderId ids[64];
        ncmContentStorageListPlaceHolder(&cs, ids, (s32)(sizeof(ids) / sizeof(ids[0])), &before);

        // ncm has a dedicated sweep for exactly this.
        const Result rc = ncmContentStorageCleanupAllPlaceHolder(&cs);
        if (R_SUCCEEDED(rc)) {
            removed += (before > 0) ? (u32)before : 0;
            LOG_I("maintenance: cleared placeholders on %s", storage_name(k_storages[i]));
        } else {
            LOG_W("maintenance: placeholder cleanup failed on %s (0x%x)",
                  storage_name(k_storages[i]), rc);
        }

        ncmContentStorageClose(&cs);
    }

    if (out_removed) *out_removed = removed;
    return 0;
}

Result nexusMaintenanceCleanOrphans(u32 *out_removed)
{
    // ns knows which application entities no longer have a record behind them;
    // letting it do the reasoning is far safer than deciding ourselves which
    // content is unreferenced.
    const Result rc = nsDeleteRedundantApplicationEntity();
    if (R_FAILED(rc)) {
        LOG_W("maintenance: DeleteRedundantApplicationEntity failed (0x%x)", rc);
        if (out_removed) *out_removed = 0;
        return rc;
    }

    LOG_I("maintenance: removed redundant application entities");
    if (out_removed) *out_removed = 1;
    return 0;
}
