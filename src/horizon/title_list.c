// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- the installed-title list, for the on-console UI.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/title_list.h"
#include "nexus/ns_ext.h"
#include "nexus/format.h"
#include "nexus/log.h"

const char *nexusTitleSortStr(u8 sort)
{
    switch (sort) {
        case NexusTitleSort_Name: return "name";
        case NexusTitleSort_Size: return "size";
        case NexusTitleSort_Id:   return "title id";
        default:                  return "?";
    }
}

const char *nexusTitleFilterStr(u8 filter)
{
    switch (filter) {
        case NexusTitleFilter_All:          return "all";
        case NexusTitleFilter_Applications: return "games";
        case NexusTitleFilter_Updates:      return "updates";
        case NexusTitleFilter_AddOns:       return "DLC";
        default:                            return "?";
    }
}

static u64 base_application_id(u64 id, u8 meta_type)
{
    switch (meta_type) {
        case NcmContentMetaType_Patch:        return id ^ 0x800ull;
        case NcmContentMetaType_AddOnContent: return (id ^ 0x1000ull) & ~0xFFFull;
        default:                              return id;
    }
}

static const char *type_suffix(u8 t)
{
    switch (t) {
        case NcmContentMetaType_Application:  return "";
        case NcmContentMetaType_Patch:        return " [update]";
        case NcmContentMetaType_AddOnContent: return " [DLC]";
        default:                              return " [system]";
    }
}

static void resolve_name(u64 application_id, char *out, size_t out_size)
{
    NsApplicationControlData *data =
        (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));

    if (data != NULL) {
        u64 actual = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                    application_id, data,
                                                    sizeof(*data), &actual))
            && actual >= sizeof(data->nacp)) {
            NacpLanguageEntry *entry = NULL;
            if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry))
                && entry != NULL && entry->name[0] != '\0') {
                nexusSanitiseUtf8(entry->name, out, out_size);
                free(data);
                if (out[0] != '\0') return;
                out[0] = '\0';
            }
        }
        free(data);
    }

    snprintf(out, out_size, "%016llx", (unsigned long long)application_id);
}

// As in storage_titles.c: ncmContentMetaDatabaseList is not paginated, so the
// buffer has to be large enough to take the whole storage in one call.
static void collect(NexusTitleList *out, NcmStorageId storage_id)
{
    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id))) return;

    const s32 room = (s32)(NEXUS_TITLE_LIST_MAX - out->count);
    if (room <= 0) { ncmContentMetaDatabaseClose(&db); return; }

    NcmContentMetaKey *keys = (NcmContentMetaKey *)calloc((size_t)room, sizeof(*keys));
    if (keys == NULL) { ncmContentMetaDatabaseClose(&db); return; }

    s32 total = 0, written = 0;
    const Result rc = ncmContentMetaDatabaseList(&db, &total, &written, keys, room,
                                                 NcmContentMetaType_Unknown,
                                                 0, 0, UINT64_MAX,
                                                 NcmContentInstallType_Full);

    if (R_SUCCEEDED(rc)) {
        for (s32 i = 0; i < written && out->count < NEXUS_TITLE_LIST_MAX; i++) {
            NexusTitleInfo *info = &out->items[out->count];

            info->key        = keys[i];
            info->storage_id = (u8)storage_id;

            u64 size = 0;
            ncmContentMetaDatabaseGetSize(&db, &size, &keys[i]);
            info->size         = size;
            out->total_bytes  += size;

            char display[NEXUS_TITLE_NAME_LEN];
            resolve_name(base_application_id(keys[i].id, keys[i].type),
                         display, sizeof(display));

            snprintf(info->name, sizeof(info->name), "%.56s%s",
                     display, type_suffix(keys[i].type));

            out->count++;
        }
    }

    free(keys);
    ncmContentMetaDatabaseClose(&db);
}

Result nexusTitleListBuild(NexusTitleList *out)
{
    if (out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(out, 0, sizeof(*out));

    collect(out, NcmStorageId_SdCard);
    collect(out, NcmStorageId_BuiltInUser);

    nexusTitleListSort(out, NexusTitleSort_Name);

    LOG_I("titles: %u installed content meta(s)", out->count);
    return 0;
}

// Case-insensitive, so "zelda" and "Zelda" sort together rather than the
// uppercase names forming their own block ahead of everything else.
static int name_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return (*a == '\0') ? -1 : 1;
}

static u8 g_sort_mode = NexusTitleSort_Name;

static int compare(const void *lhs, const void *rhs)
{
    const NexusTitleInfo *a = (const NexusTitleInfo *)lhs;
    const NexusTitleInfo *b = (const NexusTitleInfo *)rhs;

    switch (g_sort_mode) {
        case NexusTitleSort_Size:
            // Largest first: the reason to sort by size is to find what to
            // delete.
            if (a->size != b->size) return (a->size > b->size) ? -1 : 1;
            break;

        case NexusTitleSort_Id:
            if (a->key.id != b->key.id) return (a->key.id < b->key.id) ? -1 : 1;
            break;

        default:
            break;
    }

    // Name is both the default and the tie-break, so equal sizes still come
    // out in a stable, readable order.
    const int by_name = name_cmp(a->name, b->name);
    if (by_name != 0) return by_name;

    if (a->key.id != b->key.id) return (a->key.id < b->key.id) ? -1 : 1;
    return (a->key.version < b->key.version) ? -1 : 1;
}

void nexusTitleListSort(NexusTitleList *list, u8 sort)
{
    if (list == NULL || list->count == 0) return;

    g_sort_mode = sort;
    qsort(list->items, list->count, sizeof(list->items[0]), compare);
}

bool nexusTitleListMatches(const NexusTitleInfo *info, u8 filter)
{
    switch (filter) {
        case NexusTitleFilter_Applications:
            return info->key.type == NcmContentMetaType_Application;
        case NexusTitleFilter_Updates:
            return info->key.type == NcmContentMetaType_Patch;
        case NexusTitleFilter_AddOns:
            return info->key.type == NcmContentMetaType_AddOnContent;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// Deleting
// ---------------------------------------------------------------------------

// Uninstalls one content meta -- an update or a single add-on -- leaving the
// rest of the application alone. ns has no command for this: DeleteApplication*
// always takes the whole title, so the work is done by hand.
//
// Order matters. The application record is trimmed first, so that if anything
// later fails Horizon is never left with a record pointing at content that has
// already been deleted; an unreferenced NCA merely wastes space, whereas a
// dangling record makes the title unlaunchable.
static Result delete_content_meta(const NexusTitleInfo *info)
{
    const u64 app_id = base_application_id(info->key.id, info->key.type);

    s32 remaining = 0;
    Result rc = nexusNsRemoveContentMetaFromRecord(app_id, info->key.id,
                                                   info->key.type, &remaining);
    if (R_FAILED(rc)) {
        LOG_E("titles: could not trim the application record (0x%x)", rc);
        return rc;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, (NcmStorageId)info->storage_id);
    if (R_FAILED(rc)) {
        LOG_E("titles: OpenContentMetaDatabase failed (0x%x)", rc);
        return rc;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, (NcmStorageId)info->storage_id);
    if (R_FAILED(rc)) {
        LOG_E("titles: OpenContentStorage failed (0x%x)", rc);
        ncmContentMetaDatabaseClose(&db);
        return rc;
    }

    // Delete the NCAs this meta owns. ListContentInfo is genuinely paginated,
    // so it is walked with a real start index.
    NcmContentInfo content[24];
    s32 start = 0, deleted = 0;

    for (;;) {
        s32 got = 0;
        if (R_FAILED(ncmContentMetaDatabaseListContentInfo(
                &db, &got, content, (s32)(sizeof(content) / sizeof(content[0])),
                &info->key, start))
            || got <= 0) {
            break;
        }

        for (s32 i = 0; i < got; i++) {
            if (R_SUCCEEDED(ncmContentStorageDelete(&cs, &content[i].content_id))) deleted++;
        }
        start += got;
    }

    // The meta NCA describes the meta but is not listed inside it, so it has to
    // be removed separately or it is left behind forever.
    NcmContentId meta_nca;
    if (R_SUCCEEDED(ncmContentMetaDatabaseGetContentIdByType(&db, &meta_nca, &info->key,
                                                             NcmContentType_Meta))) {
        if (R_SUCCEEDED(ncmContentStorageDelete(&cs, &meta_nca))) deleted++;
    }

    rc = ncmContentMetaDatabaseRemove(&db, &info->key);
    if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
    if (R_FAILED(rc)) LOG_E("titles: removing the meta entry failed (0x%x)", rc);

    ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);

    LOG_I("titles: removed %s -- %d nca(s) deleted, %d record(s) left on %016llX",
          info->name, deleted, remaining, (unsigned long long)app_id);
    return rc;
}

Result nexusTitleListDelete(const NexusTitleInfo *info)
{
    if (info == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    LOG_W("titles: deleting %s (%016llx v%u)", info->name,
          (unsigned long long)info->key.id, info->key.version);

    // Deleting an Application removes its updates and DLC too, which is what ns
    // does and what a user expects. An update or a single add-on is uninstalled
    // on its own instead, leaving the base game installed and launchable.
    return (info->key.type == NcmContentMetaType_Application)
        ? nsDeleteApplicationCompletely(info->key.id)
        : delete_content_meta(info);
}
