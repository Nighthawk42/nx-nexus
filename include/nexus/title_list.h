// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- the installed-title list, for the on-console UI.
//
// The MTP titles store builds its own listing shaped for a file browser. This
// one is shaped for a menu: display name, size, and a sort order, so a console
// with a hundred and thirty titles is navigable without a host attached.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_TITLE_LIST_MAX  512
#define NEXUS_TITLE_NAME_LEN  80

typedef enum {
    NexusTitleSort_Name = 0,
    NexusTitleSort_Size,
    NexusTitleSort_Id,
} NexusTitleSort;

const char *nexusTitleSortStr(u8 sort);

typedef enum {
    NexusTitleFilter_All = 0,
    NexusTitleFilter_Applications,
    NexusTitleFilter_Updates,
    NexusTitleFilter_AddOns,
} NexusTitleFilter;

const char *nexusTitleFilterStr(u8 filter);

typedef struct {
    NcmContentMetaKey key;
    u8   storage_id;
    char name[NEXUS_TITLE_NAME_LEN];
    u64  size;
} NexusTitleInfo;

typedef struct {
    NexusTitleInfo items[NEXUS_TITLE_LIST_MAX];
    u32            count;
    u64            total_bytes;
} NexusTitleList;

/// Builds the list. Costs one ns lookup per title, so this is a deliberate
/// action rather than something done on every draw.
Result nexusTitleListBuild(NexusTitleList *out);

void nexusTitleListSort(NexusTitleList *list, u8 sort);

/// True when an entry passes the filter.
bool nexusTitleListMatches(const NexusTitleInfo *info, u8 filter);

/// Deletes one entry: the whole application, or just the update / add-on.
Result nexusTitleListDelete(const NexusTitleInfo *info);
