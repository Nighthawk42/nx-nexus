// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- the real install backend, over ncm, es and ns.
//
// This is deliberately a thin shell: all the sequencing decisions live in
// installer.c, which is unit-tested on the host. Everything here is a direct
// translation of one backend hook into one or two system calls, so the parts
// that cannot be tested off-console are kept as small and boring as possible.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/install_horizon.h"
#include "nexus/es.h"
#include "nexus/ns_ext.h"
#include "nexus/ncm_ext.h"
#include "nexus/compat.h"
#include "nexus/log.h"

// Registered content is remembered so a late failure can delete it again
// rather than leaving orphaned NCAs consuming storage forever.
#define MAX_REGISTERED NEXUS_INSTALL_MAX_CONTENTS

#define MAX_APP_RECORDS NEXUS_NS_MAX_APP_RECORDS

struct NexusHorizonBackend {
    NcmContentStorage      cs;
    NcmContentMetaDatabase db;
    NcmStorageId           storage_id;
    bool                   open;

    // Placeholder currently being streamed.
    NcmPlaceHolderId placeholder;
    NcmContentId     content_id;
    u64              write_offset;
    bool             placeholder_open;

    NcmContentId registered[MAX_REGISTERED];
    u32          registered_count;

    bool meta_committed;
    bool record_pushed;

    // The application record as it was before this install touched it. Pushing
    // a record means deleting the existing one first, so a rollback has to put
    // the original list back or the install failure takes the user's other
    // titles down with it.
    NexusContentStorageRecord saved_records[MAX_APP_RECORDS];
    s32                       saved_record_count;
    bool                      saved_records_valid;
};

// Maps a title id to the application it belongs to. Patches and add-on content
// carry derived ids, and the application record must be filed under the base
// application or the title will not show up.
static u64 base_application_id(u64 id, u8 meta_type)
{
    switch (meta_type) {
        case CnmtMetaType_Patch:        return id ^ 0x800ull;
        case CnmtMetaType_AddOnContent: return (id ^ 0x1000ull) & ~0xFFFull;
        default:                        return id;
    }
}

// ---------------------------------------------------------------------------
// Backend hooks
// ---------------------------------------------------------------------------

static int hz_content_begin(void *user, const u8 id[NEXUS_CONTENT_ID_SIZE], u64 size)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;

    memcpy(b->content_id.c, id, NEXUS_CONTENT_ID_SIZE);

    Result rc = ncmContentStorageGeneratePlaceHolderId(&b->cs, &b->placeholder);
    if (R_FAILED(rc)) {
        LOG_E("install: GeneratePlaceHolderId failed (0x%x)", rc);
        return -1;
    }

    // A leftover placeholder from an interrupted install would make Create
    // fail, so clear any same-id placeholder first. Failure here is fine --
    // it usually just means there was nothing to delete.
    ncmContentStorageDeletePlaceHolder(&b->cs, &b->placeholder);

    rc = ncmContentStorageCreatePlaceHolder(&b->cs, &b->content_id, &b->placeholder, (s64)size);
    if (R_FAILED(rc)) {
        LOG_E("install: CreatePlaceHolder failed (0x%x), size %llu",
              rc, (unsigned long long)size);
        return -1;
    }

    b->write_offset     = 0;
    b->placeholder_open = true;
    return 0;
}

static int hz_content_write(void *user, const void *data, size_t len)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;
    if (!b->placeholder_open) return -1;

    Result rc = ncmContentStorageWritePlaceHolder(&b->cs, &b->placeholder,
                                                  b->write_offset, data, len);
    if (R_FAILED(rc)) {
        LOG_E("install: WritePlaceHolder failed (0x%x) at offset %llu",
              rc, (unsigned long long)b->write_offset);
        return -1;
    }

    b->write_offset += len;
    return 0;
}

static int hz_content_commit(void *user)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;
    if (!b->placeholder_open) return -1;

    // Register refuses to overwrite, so drop any existing copy of this content
    // first. Reinstalling the same title is a normal thing to do.
    bool exists = false;
    if (R_SUCCEEDED(ncmContentStorageHas(&b->cs, &exists, &b->content_id)) && exists) {
        ncmContentStorageDelete(&b->cs, &b->content_id);
    }

    Result rc = ncmContentStorageRegister(&b->cs, &b->content_id, &b->placeholder);
    if (R_FAILED(rc)) {
        LOG_E("install: Register failed (0x%x)", rc);
        return -1;
    }

    if (b->registered_count < MAX_REGISTERED) {
        b->registered[b->registered_count++] = b->content_id;
    }

    b->placeholder_open = false;
    return 0;
}

static void hz_content_discard(void *user)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;
    if (!b->placeholder_open) return;

    ncmContentStorageDeletePlaceHolder(&b->cs, &b->placeholder);
    b->placeholder_open = false;
}

static int hz_read_cnmt(void *user, const u8 meta_id[NEXUS_CONTENT_ID_SIZE],
                        void *out, size_t cap, size_t *out_len)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;

    NcmContentId cid;
    memcpy(cid.c, meta_id, NEXUS_CONTENT_ID_SIZE);

    // The Meta NCA is registered by this point, so ncm can mount it for us.
    return R_SUCCEEDED(nexusNcmReadCnmt(&b->cs, &cid, out, cap, out_len)) ? 0 : -1;
}

static int hz_import_ticket(void *user, const void *tik, size_t tik_len,
                            const void *cert, size_t cert_len)
{
    (void)user;

    Result rc = nexusEsImportTicket(tik, tik_len, cert, cert_len);
    if (R_FAILED(rc)) {
        LOG_E("install: es ImportTicket failed (0x%x)", rc);
        return -1;
    }
    return 0;
}

static int hz_register_meta(void *user, const NexusInstallMeta *meta)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;

    // The record is a header, the type-specific extended header verbatim, then
    // one NcmContentInfo per content.
    const size_t info_bytes = (size_t)meta->content_count * sizeof(NcmContentInfo);
    const size_t total = sizeof(NcmContentMetaHeader) + meta->ext_header_size + info_bytes;

    u8 *buf = (u8 *)calloc(1, total);
    if (buf == NULL) {
        LOG_E("install: out of memory building the meta record");
        return -1;
    }

    NcmContentMetaHeader header = {
        .extended_header_size = meta->ext_header_size,
        .content_count        = (u16)meta->content_count,
        .content_meta_count   = 0,
        .attributes           = 0,
        .storage_id           = 0,   // None: this field is for the record, not us
    };
    memcpy(buf, &header, sizeof(header));

    if (meta->ext_header_size > 0) {
        memcpy(buf + sizeof(header), meta->ext_header, meta->ext_header_size);
    }

    NcmContentInfo *infos = (NcmContentInfo *)(buf + sizeof(header) + meta->ext_header_size);
    for (u32 i = 0; i < meta->content_count; i++) {
        const NexusInstallContent *src = &meta->contents[i];

        memcpy(infos[i].content_id.c, src->content_id, NEXUS_CONTENT_ID_SIZE);
        ncmU64ToContentInfoSize(src->size, &infos[i]);
        infos[i].attr         = src->attr;
        infos[i].content_type = src->content_type;
        infos[i].id_offset    = src->id_offset;
    }

    const NcmContentMetaKey key = {
        .id           = meta->title_id,
        .version      = meta->version,
        .type         = meta->meta_type,
        .install_type = NcmContentInstallType_Full,
    };

    Result rc = ncmContentMetaDatabaseSet(&b->db, &key, buf, total);
    free(buf);

    if (R_FAILED(rc)) {
        LOG_E("install: ContentMetaDatabaseSet failed (0x%x)", rc);
        return -1;
    }

    rc = ncmContentMetaDatabaseCommit(&b->db);
    if (R_FAILED(rc)) {
        LOG_E("install: ContentMetaDatabaseCommit failed (0x%x)", rc);
        return -1;
    }

    b->meta_committed = true;
    return 0;
}

// Applications and patches both carry RequiredSystemVersion as a u32 at offset
// 0x8 of their extended header. Checking it turns "the game installed fine but
// crashes on launch" into something the user is told about up front.
//
// This warns rather than refuses: the install itself is perfectly valid, and
// the user may be about to update their firmware.
static void warn_if_firmware_too_old(const NexusInstallMeta *meta)
{
    const bool has_required = (meta->meta_type == CnmtMetaType_Application
                               || meta->meta_type == CnmtMetaType_Patch)
                              && meta->ext_header_size >= 0x0C;
    if (!has_required) return;

    u32 required = 0;
    memcpy(&required, meta->ext_header + 0x8, sizeof(required));

    if (!nexusCompatTitleTooNew(required)) return;

    char needed[16], have[16];
    nexusCompatFormatVersion(required, needed, sizeof(needed));
    nexusCompatFormatVersion(nexusCompatFirmwareValue(), have, sizeof(have));

    LOG_W("install: %016llx needs firmware %s but this console runs %s -- "
          "it will install but may not launch",
          (unsigned long long)meta->title_id, needed, have);
}

static int hz_push_record(void *user, const NexusInstallMeta *meta)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;

    const u64 app_id = base_application_id(meta->title_id, meta->meta_type);

    const NexusContentStorageRecord record = {
        .key = {
            .id           = meta->title_id,
            .version      = meta->version,
            .type         = meta->meta_type,
            .install_type = NcmContentInstallType_Full,
        },
        .storage_id = (u8)b->storage_id,
    };

    // Read what is already attached to this application, so installing an
    // update or a DLC adds to the title instead of replacing it. Getting this
    // wrong makes the base game disappear from HOME even though its content is
    // still registered, which looks exactly like data loss to the user.
    b->saved_record_count  = nexusNsReadApplicationRecords(app_id, b->saved_records,
                                                           MAX_APP_RECORDS);
    b->saved_records_valid = true;

    // Build the union: everything that is not the same content meta we are
    // installing now, plus the new record. Matching on id and type replaces an
    // older version of the same meta rather than accumulating both.
    //
    // Heap rather than stack: this runs on the MTP responder thread, and the
    // full record array is several kilobytes.
    NexusContentStorageRecord *merged = (NexusContentStorageRecord *)
        malloc(sizeof(NexusContentStorageRecord) * MAX_APP_RECORDS);
    if (merged == NULL) {
        LOG_E("install: out of memory building the application record");
        return -1;
    }

    s32 merged_count = 0;

    for (s32 i = 0; i < b->saved_record_count; i++) {
        const NcmContentMetaKey *k = &b->saved_records[i].key;
        if (k->id == meta->title_id && k->type == meta->meta_type) continue;

        if (merged_count >= MAX_APP_RECORDS - 1) {
            LOG_E("install: application %016llX has more than %d records",
                  (unsigned long long)app_id, MAX_APP_RECORDS);
            free(merged);
            return -1;
        }
        merged[merged_count++] = b->saved_records[i];
    }
    merged[merged_count++] = record;

    // PushApplicationRecord will not overwrite, so the existing record has to
    // be removed first. Failure is expected when there was nothing there.
    nexusNsDeleteApplicationRecord(app_id);

    const Result rc = nexusNsPushApplicationRecord(app_id, 3, merged, (size_t)merged_count);
    free(merged);

    if (R_FAILED(rc)) {
        LOG_E("install: PushApplicationRecord failed (0x%x)", rc);

        // The old record is already gone at this point. Put it back rather
        // than leaving the application with no record at all.
        if (b->saved_record_count > 0) {
            nexusNsPushApplicationRecord(app_id, 3, b->saved_records,
                                         (size_t)b->saved_record_count);
        }
        return -1;
    }

    LOG_I("install: application record %016llX now has %d entr%s",
          (unsigned long long)app_id, merged_count, merged_count == 1 ? "y" : "ies");

    warn_if_firmware_too_old(meta);

    b->record_pushed = true;
    return 0;
}

static void hz_rollback(void *user, const NexusInstallMeta *meta)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)user;

    LOG_W("install: rolling back");

    if (b->placeholder_open) {
        ncmContentStorageDeletePlaceHolder(&b->cs, &b->placeholder);
        b->placeholder_open = false;
    }

    // Undo in reverse order of creation. Restoring the application record
    // means putting back exactly the list that was there before, not merely
    // deleting ours -- the merged push replaced records belonging to titles
    // this install never touched.
    if (b->record_pushed && meta != NULL) {
        const u64 app_id = base_application_id(meta->title_id, meta->meta_type);

        nexusNsDeleteApplicationRecord(app_id);

        if (b->saved_records_valid && b->saved_record_count > 0) {
            const Result rc = nexusNsPushApplicationRecord(
                app_id, 3, b->saved_records, (size_t)b->saved_record_count);
            if (R_FAILED(rc)) {
                LOG_E("install: could not restore the previous application "
                      "record for %016llX (0x%x) -- reinstall the title to "
                      "make it visible again", (unsigned long long)app_id, rc);
            } else {
                LOG_I("install: restored %d record(s) for %016llX",
                      b->saved_record_count, (unsigned long long)app_id);
            }
        }
        b->record_pushed = false;
    }

    if (b->meta_committed && meta != NULL) {
        const NcmContentMetaKey key = {
            .id           = meta->title_id,
            .version      = meta->version,
            .type         = meta->meta_type,
            .install_type = NcmContentInstallType_Full,
        };
        if (R_SUCCEEDED(ncmContentMetaDatabaseRemove(&b->db, &key))) {
            ncmContentMetaDatabaseCommit(&b->db);
        }
        b->meta_committed = false;
    }

    for (u32 i = 0; i < b->registered_count; i++) {
        ncmContentStorageDelete(&b->cs, &b->registered[i]);
    }
    b->registered_count = 0;

    if (meta != NULL && meta->has_ticket) {
        nexusEsDeleteTicket(meta->rights_id, TICKET_RIGHTS_ID_SIZE);
    }
}

static const NexusInstallBackendOps g_horizon_ops = {
    .content_begin   = hz_content_begin,
    .content_write   = hz_content_write,
    .content_commit  = hz_content_commit,
    .content_discard = hz_content_discard,
    .read_cnmt       = hz_read_cnmt,
    .import_ticket   = hz_import_ticket,
    .register_meta   = hz_register_meta,
    .push_record     = hz_push_record,
    .rollback        = hz_rollback,
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NexusHorizonBackend *nexusHorizonBackendCreate(u8 target_storage)
{
    NexusHorizonBackend *b = (NexusHorizonBackend *)calloc(1, sizeof(*b));
    if (b == NULL) return NULL;

    b->storage_id = (NcmStorageId)target_storage;

    Result rc = ncmOpenContentStorage(&b->cs, b->storage_id);
    if (R_FAILED(rc)) {
        LOG_E("install: OpenContentStorage(%u) failed (0x%x)", target_storage, rc);
        free(b);
        return NULL;
    }

    rc = ncmOpenContentMetaDatabase(&b->db, b->storage_id);
    if (R_FAILED(rc)) {
        LOG_E("install: OpenContentMetaDatabase(%u) failed (0x%x)", target_storage, rc);
        ncmContentStorageClose(&b->cs);
        free(b);
        return NULL;
    }

    b->open = true;
    return b;
}

void nexusHorizonBackendDestroy(NexusHorizonBackend *b)
{
    if (b == NULL) return;

    if (b->placeholder_open) ncmContentStorageDeletePlaceHolder(&b->cs, &b->placeholder);

    if (b->open) {
        ncmContentMetaDatabaseClose(&b->db);
        ncmContentStorageClose(&b->cs);
    }
    free(b);
}

const NexusInstallBackendOps *nexusHorizonBackendOps(void)
{
    return &g_horizon_ops;
}

Result nexusInstallServicesInit(void)
{
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) { LOG_E("install: ncmInitialize failed (0x%x)", rc); return rc; }

    rc = nsInitialize();
    if (R_FAILED(rc)) { LOG_E("install: nsInitialize failed (0x%x)", rc); goto fail_ncm; }

    rc = nexusNsExtInitialize();
    if (R_FAILED(rc)) goto fail_ns;

    rc = nexusEsInitialize();
    if (R_FAILED(rc)) goto fail_nsext;

    LOG_I("install: ncm, ns and es ready");
    return 0;

fail_nsext:
    nexusNsExtExit();
fail_ns:
    nsExit();
fail_ncm:
    ncmExit();
    return rc;
}

void nexusInstallServicesExit(void)
{
    nexusEsExit();
    nexusNsExtExit();
    nsExit();
    ncmExit();
}
