// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- application record commands missing from libnx.
//
// libnx exposes IApplicationManagerInterface but not PushApplicationRecord and
// friends, so those three commands are hand-rolled here. Command numbers come
// from https://switchbrew.org/wiki/NS_services.
//
// Registering content in ncm is not enough to make a title appear: without an
// application record the HOME menu never shows it. That missing step is the
// single most common reason a hand-written installer "works" but produces an
// invisible title.
#pragma once

#include <switch.h>

/// One entry in an application record: which content meta lives on which
/// storage. Layout is nn::ns::ContentStorageRecord (0x18 bytes).
typedef struct {
    NcmContentMetaKey key;
    u8                storage_id;
    u8                padding[7];
} NexusContentStorageRecord;

/// Opens the IApplicationManagerInterface session. Requires nsInitialize()
/// to have succeeded first. Reference counted.
Result nexusNsExtInitialize(void);

void nexusNsExtExit(void);

/// ns:am command 16: PushApplicationRecord.
/// last_modified_event is 3 for a normal install.
Result nexusNsPushApplicationRecord(u64 application_id, u8 last_modified_event,
                                    const NexusContentStorageRecord *records,
                                    size_t record_count);

/// ns:am command 17: ListApplicationRecordContentMeta.
/// Reads the records already attached to a title, so an update can be merged
/// with what is installed instead of replacing it.
Result nexusNsListApplicationRecordContentMeta(u64 offset, u64 application_id,
                                               NexusContentStorageRecord *out_records,
                                               size_t max_records, s32 *out_count);

/// ns:am command 27: DeleteApplicationRecord.
/// PushApplicationRecord refuses to overwrite an existing record, so updating
/// one means deleting it and pushing the merged list back.
Result nexusNsDeleteApplicationRecord(u64 application_id);
