// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- integrity checking for installed content.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/verify.h"
#include "nexus/ncm_ext.h"
#include "nexus/cnmt.h"
#include "nexus/format.h"
#include "nexus/log.h"

// A CNMT for a title with a large add-on catalogue runs to tens of kilobytes.
#define CNMT_CAP     (256u * 1024u)
// Big enough that the per-read IPC overhead disappears, small enough that the
// allocation always succeeds alongside a running MTP transfer.
#define READ_CHUNK   (1024u * 1024u)

const char *nexusVerifyIssueStr(u8 kind)
{
    switch (kind) {
        case NexusVerifyIssue_Corrupt:    return "corrupt";
        case NexusVerifyIssue_Missing:    return "missing";
        case NexusVerifyIssue_Unreadable: return "unreadable";
        case NexusVerifyIssue_NoManifest: return "no manifest";
        default:                          return "unknown";
    }
}

static void add_issue(NexusVerifyReport *r, const char *title,
                      const char *content, u8 kind)
{
    r->contents_bad++;

    if (r->issue_count >= NEXUS_VERIFY_MAX_ISSUES) {
        r->issues_truncated = true;
        return;
    }

    NexusVerifyIssue *e = &r->issues[r->issue_count++];
    snprintf(e->title, sizeof(e->title), "%.120s", title);
    snprintf(e->content, sizeof(e->content), "%.32s", content);
    e->kind = kind;
}

static void format_id(const NcmContentId *id, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    char buf[33];

    for (int i = 0; i < 16; i++) {
        buf[i * 2]     = hex[(id->c[i] >> 4) & 0xF];
        buf[i * 2 + 1] = hex[id->c[i] & 0xF];
    }
    buf[32] = '\0';

    snprintf(out, out_size, "%s", buf);
}

// Hashes one registered NCA and compares it with the SHA-256 the CNMT recorded
// for it. The hash covers the NCA as stored -- encrypted -- so no key material
// is involved anywhere in this.
static u8 hash_one_content(NcmContentStorage *cs, const NcmContentId *id,
                           const u8 expected[CNMT_HASH_SIZE], u64 expected_size,
                           u8 *buffer, NexusVerifyReport *r,
                           NexusVerifyTick tick, void *tick_user, bool *cancelled)
{
    bool present = false;
    if (R_FAILED(ncmContentStorageHas(cs, &present, id)) || !present) {
        return NexusVerifyIssue_Missing;
    }

    s64 actual_size = 0;
    if (R_FAILED(ncmContentStorageGetSizeFromContentId(cs, &actual_size, id))
        || actual_size <= 0) {
        return NexusVerifyIssue_Unreadable;
    }

    // A size that disagrees with the manifest is already a failure; hashing it
    // would only confirm the same thing more slowly.
    if (expected_size != 0 && (u64)actual_size != expected_size) {
        LOG_W("verify: size mismatch -- manifest %llu, on disk %lld",
              (unsigned long long)expected_size, (long long)actual_size);
        return NexusVerifyIssue_Corrupt;
    }

    Sha256Context sha;
    sha256ContextCreate(&sha);

    s64 offset = 0;
    while (offset < actual_size) {
        s64 chunk = actual_size - offset;
        if (chunk > (s64)READ_CHUNK) chunk = (s64)READ_CHUNK;

        if (R_FAILED(ncmContentStorageReadContentIdFile(cs, buffer, (size_t)chunk,
                                                        id, offset))) {
            LOG_E("verify: read failed at offset %lld", (long long)offset);
            return NexusVerifyIssue_Unreadable;
        }

        sha256ContextUpdate(&sha, buffer, (size_t)chunk);

        offset        += chunk;
        r->bytes_done += (u64)chunk;

        if (tick != NULL && !tick(tick_user, r)) {
            *cancelled = true;
            return NexusVerifyIssue_Unreadable;   // unused; the caller bails out
        }
    }

    u8 got[CNMT_HASH_SIZE];
    sha256ContextGetHash(&sha, got);

    return (memcmp(got, expected, CNMT_HASH_SIZE) == 0)
        ? 0xFF                       // sentinel: no issue
        : NexusVerifyIssue_Corrupt;
}

Result nexusVerifyContentMeta(const NcmContentMetaKey *key, u8 storage_id,
                              const char *title_name, NexusVerifyReport *report,
                              NexusVerifyTick tick, void *tick_user)
{
    if (key == NULL || report == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    snprintf(report->current, sizeof(report->current), "%.120s", title_name);

    NcmContentMetaDatabase db;
    Result rc = ncmOpenContentMetaDatabase(&db, (NcmStorageId)storage_id);
    if (R_FAILED(rc)) return rc;

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, (NcmStorageId)storage_id);
    if (R_FAILED(rc)) {
        ncmContentMetaDatabaseClose(&db);
        return rc;
    }

    u8 *cnmt_buf = (u8 *)malloc(CNMT_CAP);
    u8 *chunk    = (u8 *)malloc(READ_CHUNK);

    if (cnmt_buf == NULL || chunk == NULL) {
        free(cnmt_buf);
        free(chunk);
        ncmContentStorageClose(&cs);
        ncmContentMetaDatabaseClose(&db);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    // The manifest lives inside the Meta NCA. Without it there is nothing to
    // compare against, so this is a verification failure in its own right.
    NcmContentId meta_id;
    size_t cnmt_len = 0;

    if (R_FAILED(ncmContentMetaDatabaseGetContentIdByType(&db, &meta_id, key,
                                                          NcmContentType_Meta))
        || R_FAILED(nexusNcmReadCnmt(&cs, &meta_id, cnmt_buf, CNMT_CAP, &cnmt_len))) {
        add_issue(report, title_name, "(meta)", NexusVerifyIssue_NoManifest);
        goto done;
    }

    CnmtContext cnmt;
    if (cnmtInit(&cnmt, cnmt_buf, cnmt_len) != NexusFmt_Ok) {
        add_issue(report, title_name, "(meta)", NexusVerifyIssue_NoManifest);
        goto done;
    }

    for (u16 i = 0; i < cnmtGetContentCount(&cnmt); i++) {
        CnmtContentInfo info;
        if (cnmtGetContentInfo(&cnmt, i, &info) != NexusFmt_Ok) continue;

        // Delta fragments describe an update path between two versions and are
        // not installed, so the CNMT listing one that is absent is normal.
        if (info.content_type == CnmtContentType_DeltaFragment) continue;

        NcmContentId id;
        memcpy(id.c, info.content_id, CNMT_CONTENT_ID_SIZE);

        char hex[36];
        format_id(&id, hex, sizeof(hex));

        report->bytes_total += info.size;

        bool cancelled = false;
        const u8 issue = hash_one_content(&cs, &id, info.hash, info.size, chunk,
                                          report, tick, tick_user, &cancelled);

        if (cancelled) {
            report->cancelled = true;
            goto done;
        }

        if (issue == 0xFF) {
            report->contents_ok++;
        } else {
            LOG_W("verify: %s -- %s (%s)", title_name, hex, nexusVerifyIssueStr(issue));
            add_issue(report, title_name, hex, issue);
        }
    }

done:
    free(cnmt_buf);
    free(chunk);
    ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);
    return 0;
}

// ---------------------------------------------------------------------------
// Whole-console pass
// ---------------------------------------------------------------------------

typedef struct {
    NcmContentMetaKey key;
    u8                storage_id;
} MetaRef;

#define MAX_METAS 512

static u32 collect(NcmStorageId storage_id, MetaRef *out, u32 have, u32 max)
{
    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id))) return have;

    const s32 room = (s32)(max - have);
    if (room <= 0) { ncmContentMetaDatabaseClose(&db); return have; }

    NcmContentMetaKey *keys = (NcmContentMetaKey *)calloc((size_t)room, sizeof(*keys));
    if (keys == NULL) { ncmContentMetaDatabaseClose(&db); return have; }

    // As in storage_titles.c: this call is not paginated despite appearances,
    // so it has to be made once with a buffer big enough for everything.
    s32 total = 0, written = 0;
    const Result rc = ncmContentMetaDatabaseList(&db, &total, &written, keys, room,
                                                 NcmContentMetaType_Unknown,
                                                 0, 0, UINT64_MAX,
                                                 NcmContentInstallType_Full);
    ncmContentMetaDatabaseClose(&db);

    if (R_SUCCEEDED(rc)) {
        for (s32 i = 0; i < written && have < max; i++) {
            out[have].key        = keys[i];
            out[have].storage_id = (u8)storage_id;
            have++;
        }
    }

    free(keys);
    return have;
}

static void name_for(const NcmContentMetaKey *key, char *out, size_t out_size)
{
    // Deliberately not the pretty NACP name: this runs over every installed
    // title and nsGetApplicationControlData is expensive. The id is what a user
    // needs to find the title again anyway.
    static const char *type_str[] = { "Application", "Update", "DLC" };
    const char *t = "Content";

    if (key->type == NcmContentMetaType_Application)       t = type_str[0];
    else if (key->type == NcmContentMetaType_Patch)        t = type_str[1];
    else if (key->type == NcmContentMetaType_AddOnContent) t = type_str[2];

    snprintf(out, out_size, "%016llx v%u %s",
             (unsigned long long)key->id, key->version, t);
}

Result nexusVerifyEverything(NexusVerifyReport *report,
                             NexusVerifyTick tick, void *tick_user)
{
    if (report == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(report, 0, sizeof(*report));

    MetaRef *metas = (MetaRef *)calloc(MAX_METAS, sizeof(MetaRef));
    if (metas == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    u32 count = collect(NcmStorageId_SdCard, metas, 0, MAX_METAS);
    count     = collect(NcmStorageId_BuiltInUser, metas, count, MAX_METAS);

    report->titles_total = count;
    LOG_I("verify: checking %u installed content meta(s)", count);

    for (u32 i = 0; i < count; i++) {
        char name[128];
        name_for(&metas[i].key, name, sizeof(name));

        nexusVerifyContentMeta(&metas[i].key, metas[i].storage_id, name,
                               report, tick, tick_user);

        report->titles_done++;

        if (report->cancelled) break;
    }

    free(metas);

    LOG_I("verify: %u ok, %u bad, %u title(s) checked",
          report->contents_ok, report->contents_bad, report->titles_done);
    return 0;
}
