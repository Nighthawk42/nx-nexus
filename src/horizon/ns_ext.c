// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- application record commands missing from libnx.

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
