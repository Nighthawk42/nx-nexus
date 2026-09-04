// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- application record commands missing from libnx.

#include <stdlib.h>
#include <string.h>

#include "nexus/ns_ext.h"
#include "nexus/log.h"

static Service g_ns_am;
static u64     g_ns_refcnt = 0;
static Mutex   g_ns_lock;   // zero-initialised, which is an unlocked Mutex

Result nexusNsExtInitialize(void)
{
    mutexLock(&g_ns_lock);

    Result rc = 0;
    if (g_ns_refcnt == 0) {
        // On [3.0.0+] the application manager is reached through the getter
        // interface. This build already requires 5.0.0+, so the pre-3.0.0 path
        // (nsGetServiceSession_ApplicationManagerInterface) is not needed.
        rc = nsGetApplicationManagerInterface(&g_ns_am);
        if (R_FAILED(rc)) LOG_E("ns: GetApplicationManagerInterface failed (0x%x)", rc);
    }
    if (R_SUCCEEDED(rc)) g_ns_refcnt++;

    mutexUnlock(&g_ns_lock);
    return rc;
}

void nexusNsExtExit(void)
{
    mutexLock(&g_ns_lock);

    if (g_ns_refcnt > 0 && --g_ns_refcnt == 0) {
        serviceClose(&g_ns_am);
    }

    mutexUnlock(&g_ns_lock);
}

Result nexusNsPushApplicationRecord(u64 application_id, u8 last_modified_event,
                                    const NexusContentStorageRecord *records,
                                    size_t record_count)
{
    if (records == NULL || record_count == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // The u8 comes first in the request struct, followed by padding and then
    // the application id.
    const struct {
        u8  last_modified_event;
        u8  padding[7];
        u64 application_id;
    } in = {
        .last_modified_event = last_modified_event,
        .padding             = { 0 },
        .application_id      = application_id,
    };

    return serviceDispatchIn(&g_ns_am, 16, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers      = { { records, record_count * sizeof(NexusContentStorageRecord) } },
    );
}

Result nexusNsListApplicationRecordContentMeta(u64 offset, u64 application_id,
                                               NexusContentStorageRecord *out_records,
                                               size_t max_records, s32 *out_count)
{
    if (out_records == NULL || max_records == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const struct {
        u64 offset;
        u64 application_id;
    } in = { .offset = offset, .application_id = application_id };

    s32 count = 0;
    Result rc = serviceDispatchInOut(&g_ns_am, 17, in, count,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers      = { { out_records, max_records * sizeof(NexusContentStorageRecord) } },
    );

    if (R_SUCCEEDED(rc) && out_count != NULL) *out_count = count;
    return rc;
}

Result nexusNsDeleteApplicationRecord(u64 application_id)
{
    return serviceDispatchIn(&g_ns_am, 27, application_id);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

s32 nexusNsReadApplicationRecords(u64 application_id,
                                  NexusContentStorageRecord *out, s32 max)
{
    if (out == NULL || max <= 0) return 0;

    s32 total = 0;

    while (total < max) {
        s32 got = 0;
        const Result rc = nexusNsListApplicationRecordContentMeta(
            (u64)total, application_id, out + total, (size_t)(max - total), &got);

        // A title with no record at all fails here. That is the normal case
        // for a fresh install, not something worth logging loudly.
        if (R_FAILED(rc)) {
            if (total == 0) {
                LOG_D("ns: no application record for %016llX",
                      (unsigned long long)application_id);
            }
            break;
        }
        if (got <= 0) break;

        total += got;
    }

    return total;
}

Result nexusNsRemoveContentMetaFromRecord(u64 application_id, u64 meta_id,
                                          u8 meta_type, s32 *out_remaining)
{
    NexusContentStorageRecord *records = (NexusContentStorageRecord *)
        malloc(sizeof(NexusContentStorageRecord) * NEXUS_NS_MAX_APP_RECORDS);
    if (records == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    const s32 count = nexusNsReadApplicationRecords(application_id, records,
                                                    NEXUS_NS_MAX_APP_RECORDS);

    // Compact in place, dropping the meta being removed.
    s32 kept = 0;
    for (s32 i = 0; i < count; i++) {
        const NcmContentMetaKey *k = &records[i].key;
        if (k->id == meta_id && k->type == meta_type) continue;
        records[kept++] = records[i];
    }

    if (kept == count) {
        // Nothing matched. The record is already in the desired state, so this
        // is a success rather than an error -- the caller may be cleaning up
        // after a partially completed uninstall.
        LOG_D("ns: %016llX has no record for meta %016llX type %u",
              (unsigned long long)application_id, (unsigned long long)meta_id,
              (unsigned)meta_type);
        free(records);
        if (out_remaining != NULL) *out_remaining = count;
        return 0;
    }

    Result rc = nexusNsDeleteApplicationRecord(application_id);
    if (R_FAILED(rc)) {
        LOG_E("ns: DeleteApplicationRecord(%016llX) failed (0x%x)",
              (unsigned long long)application_id, rc);
        free(records);
        return rc;
    }

    // Zero records left means the application is gone entirely; pushing an
    // empty list is rejected, and leaving the record deleted is correct.
    if (kept > 0) {
        rc = nexusNsPushApplicationRecord(application_id, 3, records, (size_t)kept);
        if (R_FAILED(rc)) {
            LOG_E("ns: could not push the trimmed record for %016llX (0x%x)",
                  (unsigned long long)application_id, rc);
        }
    }

    free(records);
    if (out_remaining != NULL) *out_remaining = kept;
    return rc;
}
