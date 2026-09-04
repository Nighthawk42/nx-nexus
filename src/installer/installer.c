// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- streaming NSP install orchestration.
//
// No libnx here on purpose: every decision this file makes is exercised by the
// host tests in tests/test_installer.c against a mock backend, including the
// failure paths that are impractical to reproduce on hardware.

#include <string.h>

#include "nexus/installer.h"

const char *nexusInstallStr(NexusInstallResult r)
{
    switch (r) {
        case NexusInstall_Ok:              return "ok";
        case NexusInstall_InProgress:      return "in progress";
        case NexusInstall_BadContainer:    return "bad container";
        case NexusInstall_HeaderTooLarge:  return "header too large";
        case NexusInstall_NoMetaNca:       return "no meta nca";
        case NexusInstall_TooManyContents: return "too many contents";
        case NexusInstall_BadContentId:    return "bad content id";
        case NexusInstall_BadCnmt:         return "bad cnmt";
        case NexusInstall_MissingContent:  return "missing content";
        case NexusInstall_BufferTooSmall:  return "buffer too small";
        case NexusInstall_BackendError:    return "backend error";
        case NexusInstall_NoDecompressor:  return "compressed, and no decoder attached";
        case NexusInstall_BadNcz:          return "bad or unsupported ncz";
        case NexusInstall_Aborted:         return "aborted";
        case NexusInstall_NotFinished:     return "stream not finished";
        default:                           return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Filename handling
// ---------------------------------------------------------------------------

static bool hex_nibble(char c, u8 *out)
{
    if (c >= '0' && c <= '9') { *out = (u8)(c - '0');      return true; }
    if (c >= 'a' && c <= 'f') { *out = (u8)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (u8)(c - 'A' + 10); return true; }
    return false;
}

bool nexusInstallParseContentId(const char *hex, size_t hex_len,
                                u8 out[NEXUS_CONTENT_ID_SIZE])
{
    if (hex == NULL || out == NULL) return false;
    if (hex_len != NEXUS_CONTENT_ID_SIZE * 2) return false;

    for (size_t i = 0; i < NEXUS_CONTENT_ID_SIZE; i++) {
        u8 hi, lo;
        if (!hex_nibble(hex[i * 2], &hi))       return false;
        if (!hex_nibble(hex[(i * 2) + 1], &lo)) return false;
        out[i] = (u8)((hi << 4) | lo);
    }
    return true;
}

// True when name ends with suffix (case-sensitive; NSP packers emit lowercase).
static bool ends_with(const char *name, const char *suffix)
{
    const size_t n = strlen(name);
    const size_t s = strlen(suffix);
    return n >= s && strcmp(name + (n - s), suffix) == 0;
}

NexusEntryKind nexusInstallClassify(const char *name, u8 out_id[NEXUS_CONTENT_ID_SIZE])
{
    if (name == NULL) return NexusEntryKind_Ignore;

    // The content id is the filename up to the first dot, for every NCA form:
    // "<id>.nca" and "<id>.cnmt.nca" alike.
    const char *dot = strchr(name, '.');
    const size_t stem_len = (dot != NULL) ? (size_t)(dot - name) : strlen(name);

    if (ends_with(name, ".cnmt.nca")) {
        if (out_id != NULL && !nexusInstallParseContentId(name, stem_len, out_id)) {
            return NexusEntryKind_Ignore;
        }
        return NexusEntryKind_MetaNca;
    }

    if (ends_with(name, ".nca")) {
        if (out_id != NULL && !nexusInstallParseContentId(name, stem_len, out_id)) {
            return NexusEntryKind_Ignore;
        }
        return NexusEntryKind_Nca;
    }

    // NSZ: the same content, zstd-compressed. The content id still comes from
    // the filename stem, so an .ncz installs as the .nca it rebuilds to.
    if (ends_with(name, ".cnmt.ncz")) {
        if (out_id != NULL && !nexusInstallParseContentId(name, stem_len, out_id)) {
            return NexusEntryKind_Ignore;
        }
        return NexusEntryKind_MetaNcz;
    }

    if (ends_with(name, ".ncz")) {
        if (out_id != NULL && !nexusInstallParseContentId(name, stem_len, out_id)) {
            return NexusEntryKind_Ignore;
        }
        return NexusEntryKind_Ncz;
    }

    if (ends_with(name, ".tik"))  return NexusEntryKind_Ticket;
    if (ends_with(name, ".cert")) return NexusEntryKind_Cert;

    // .cnmt.xml, .nacp.xml, .jpg and similar packaging cruft.
    return NexusEntryKind_Ignore;
}

// ---------------------------------------------------------------------------
// Header handling
// ---------------------------------------------------------------------------

// Builds the entry table from the parsed PFS0, ordered by offset so that
// streaming can walk it with a single cursor.
static NexusInstallResult build_entries(NexusInstaller *ins)
{
    const u32 count = partitionFsGetEntryCount(&ins->pfs);
    const u64 data_off = partitionFsGetDataOffset(&ins->pfs);

    ins->entry_count = 0;

    for (u32 i = 0; i < count; i++) {
        PartitionFsEntry e;
        if (partitionFsGetEntry(&ins->pfs, i, &e) != NexusFmt_Ok) return NexusInstall_BadContainer;

        u8 id[NEXUS_CONTENT_ID_SIZE];
        memset(id, 0, sizeof(id));
        const NexusEntryKind kind = nexusInstallClassify(e.name, id);
        if (kind == NexusEntryKind_Ignore) continue;

        if (ins->entry_count >= NEXUS_INSTALL_MAX_CONTENTS) return NexusInstall_TooManyContents;

        NexusInstallEntry *slot = &ins->entries[ins->entry_count++];
        slot->pfs_index  = i;
        slot->kind       = (u8)kind;
        slot->abs_offset = data_off + e.offset;
        slot->size       = e.size;
        memcpy(slot->content_id, id, sizeof(id));

        if (NEXUS_KIND_IS_META(kind)) {
            memcpy(ins->meta_content_id, id, sizeof(id));
            // For a compressed meta this is the compressed length; it is
            // replaced with the real one once the decoder reports it, because
            // the meta record must carry the size of the NCA on disk.
            ins->meta_nca_size = e.size;
            ins->has_meta_nca  = true;
        }
    }

    if (!ins->has_meta_nca) return NexusInstall_NoMetaNca;

    // Insertion sort by absolute offset. The list is tiny and usually already
    // ordered, but a packer is not obliged to emit entries in offset order and
    // the streaming cursor depends on it.
    for (u32 i = 1; i < ins->entry_count; i++) {
        NexusInstallEntry key = ins->entries[i];
        u32 j = i;
        while (j > 0 && ins->entries[j - 1].abs_offset > key.abs_offset) {
            ins->entries[j] = ins->entries[j - 1];
            j--;
        }
        ins->entries[j] = key;
    }

    return NexusInstall_Ok;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NexusInstallResult nexusInstallBegin(NexusInstaller *ins,
                                     const NexusInstallBackendOps *ops, void *user,
                                     u8 target_storage)
{
    if (ins == NULL || ops == NULL) return NexusInstall_BackendError;

    memset(ins, 0, sizeof(*ins));
    ins->ops            = ops;
    ins->user           = user;
    ins->target_storage = target_storage;
    ins->stage          = NexusInstallStage_Magic;
    ins->meta.storage_id = target_storage;

    return NexusInstall_InProgress;
}

static NexusInstallResult fail(NexusInstaller *ins, NexusInstallResult r)
{
    // Drop any placeholder still open, then let the backend clean up whatever
    // it already committed.
    if (ins->content_open && ins->ops->content_discard != NULL) {
        ins->ops->content_discard(ins->user);
        ins->content_open = false;
    }
    if (ins->ops->rollback != NULL) ins->ops->rollback(ins->user, &ins->meta);

    ins->stage = NexusInstallStage_Failed;
    return r;
}

// Consumes header bytes. Returns how many of `len` were used.
static size_t absorb_header(NexusInstaller *ins, const u8 *data, size_t len, size_t want)
{
    const size_t need = want - ins->header_have;
    const size_t take = (len < need) ? len : need;
    memcpy(ins->header + ins->header_have, data, take);
    ins->header_have += take;
    return take;
}

// Receives reconstructed NCA bytes from the decompressor.
//
// The placeholder is opened here rather than at the start of the entry: an
// NCZ's compressed length says nothing about the size of the NCA it rebuilds
// to, and ncm needs the real length up front. The decoder does not emit
// anything until it has read the section table, so by the first call the size
// is known.
static int ncz_sink(void *user, const void *data, size_t len)
{
    NexusInstaller *ins = (NexusInstaller *)user;

    if (!ins->content_open) {
        const u64 size = ins->ncz->size(ins->ncz_user);
        if (size == 0) return -1;

        const NexusInstallEntry *e = &ins->entries[ins->entry_cursor];
        if (ins->ops->content_begin(ins->user, e->content_id, size) != 0) return -1;
        ins->content_open = true;
    }

    if (ins->ops->content_write(ins->user, data, len) != 0) return -1;

    ins->bytes_written += len;
    return 0;
}

void nexusInstallSetDecompressor(NexusInstaller *ins, const NexusNczOps *ops, void *user)
{
    if (ins == NULL) return;

    ins->ncz      = ops;
    ins->ncz_user = user;
}

// Routes `len` bytes that fall inside the current entry.
static NexusInstallResult route_bytes(NexusInstaller *ins, const u8 *data, size_t len)
{
    NexusInstallEntry *e = &ins->entries[ins->entry_cursor];

    switch (e->kind) {
        case NexusEntryKind_Nca:
        case NexusEntryKind_MetaNca:
            if (ins->ops->content_write(ins->user, data, len) != 0) {
                return fail(ins, NexusInstall_BackendError);
            }
            ins->bytes_written += len;
            break;

        case NexusEntryKind_Ncz:
        case NexusEntryKind_MetaNcz:
            // Straight into the decoder, which calls back into ncz_sink with
            // reconstructed bytes once it knows how big the NCA will be.
            if (ins->ncz->feed(ins->ncz_user, data, len) != 0) {
                return fail(ins, NexusInstall_BadNcz);
            }
            break;

        case NexusEntryKind_Ticket:
            if (ins->tik_len + len > sizeof(ins->tik)) return fail(ins, NexusInstall_BufferTooSmall);
            memcpy(ins->tik + ins->tik_len, data, len);
            ins->tik_len += len;
            break;

        case NexusEntryKind_Cert:
            if (ins->cert_len + len > sizeof(ins->cert)) return fail(ins, NexusInstall_BufferTooSmall);
            memcpy(ins->cert + ins->cert_len, data, len);
            ins->cert_len += len;
            break;

        default:
            break;  // ignored entry: discard the bytes
    }

    return NexusInstall_InProgress;
}

NexusInstallResult nexusInstallFeed(NexusInstaller *ins, const void *data, size_t len)
{
    if (ins == NULL || data == NULL) return NexusInstall_BackendError;
    if (ins->stage == NexusInstallStage_Failed) return NexusInstall_Aborted;
    if (len == 0) return NexusInstall_InProgress;

    const u8 *p = (const u8 *)data;
    size_t remaining = len;

    while (remaining > 0) {
        switch (ins->stage) {

        case NexusInstallStage_Magic: {
            const size_t used = absorb_header(ins, p, remaining, PARTITION_FS_HEADER_SIZE);
            p += used; remaining -= used; ins->stream_pos += used;

            if (ins->header_have < PARTITION_FS_HEADER_SIZE) return NexusInstall_InProgress;

            PartitionFsType type;
            if (partitionFsPeekHeaderSize(ins->header, ins->header_have,
                                          &type, &ins->header_size) != NexusFmt_Ok) {
                return fail(ins, NexusInstall_BadContainer);
            }
            if (ins->header_size > sizeof(ins->header)) {
                return fail(ins, NexusInstall_HeaderTooLarge);
            }
            ins->stage = NexusInstallStage_Header;
            break;
        }

        case NexusInstallStage_Header: {
            const size_t used = absorb_header(ins, p, remaining, (size_t)ins->header_size);
            p += used; remaining -= used; ins->stream_pos += used;

            if (ins->header_have < ins->header_size) return NexusInstall_InProgress;

            if (partitionFsInit(&ins->pfs, ins->header, ins->header_have) != NexusFmt_Ok) {
                return fail(ins, NexusInstall_BadContainer);
            }

            const NexusInstallResult r = build_entries(ins);
            if (r != NexusInstall_Ok) return fail(ins, r);

            ins->stage        = NexusInstallStage_Streaming;
            ins->entry_cursor = 0;
            break;
        }

        case NexusInstallStage_Streaming: {
            // Past the last entry: anything left is trailing padding.
            if (ins->entry_cursor >= ins->entry_count) {
                ins->stream_pos += remaining;
                remaining = 0;
                ins->stage = NexusInstallStage_Complete;
                break;
            }

            NexusInstallEntry *e = &ins->entries[ins->entry_cursor];

            // Inter-entry alignment padding.
            if (ins->stream_pos < e->abs_offset) {
                const u64 gap = e->abs_offset - ins->stream_pos;
                const size_t skip = (remaining < gap) ? remaining : (size_t)gap;
                p += skip; remaining -= skip; ins->stream_pos += skip;
                break;
            }

            // Opening a new content placeholder. A compressed entry defers
            // this: its final size is not known until the decoder has read the
            // NCZ header, so ncz_sink opens the placeholder instead.
            if (NEXUS_KIND_IS_COMPRESSED(e->kind)) {
                if (!ins->ncz_active) {
                    if (ins->ncz == NULL) return fail(ins, NexusInstall_NoDecompressor);
                    if (ins->ncz->begin(ins->ncz_user, ncz_sink, ins) != 0) {
                        return fail(ins, NexusInstall_BadNcz);
                    }
                    ins->ncz_active = true;
                }
            } else if (!ins->content_open && NEXUS_KIND_IS_CONTENT(e->kind)) {
                if (ins->ops->content_begin(ins->user, e->content_id, e->size) != 0) {
                    return fail(ins, NexusInstall_BackendError);
                }
                ins->content_open = true;
            }

            const u64 entry_end = e->abs_offset + e->size;
            const u64 left      = entry_end - ins->stream_pos;
            const size_t take   = (remaining < left) ? remaining : (size_t)left;

            if (take > 0) {
                const NexusInstallResult r = route_bytes(ins, p, take);
                if (r != NexusInstall_InProgress) return r;
                p += take; remaining -= take; ins->stream_pos += take;
            }

            // Entry finished.
            if (ins->stream_pos >= entry_end) {
                if (ins->ncz_active) {
                    if (ins->ncz->end(ins->ncz_user) != 0) {
                        return fail(ins, NexusInstall_BadNcz);
                    }
                    // The meta record wants the reconstructed size, not the
                    // compressed one it was provisionally given.
                    if (NEXUS_KIND_IS_META(e->kind)) {
                        const u64 real = ins->ncz->size(ins->ncz_user);
                        if (real > 0) ins->meta_nca_size = real;
                    }
                    ins->ncz_active = false;
                }

                if (ins->content_open) {
                    if (ins->ops->content_commit(ins->user) != 0) {
                        return fail(ins, NexusInstall_BackendError);
                    }
                    ins->content_open = false;
                }
                ins->entry_cursor++;
                if (ins->entry_cursor >= ins->entry_count) {
                    ins->stage = NexusInstallStage_Complete;
                }
            }
            break;
        }

        case NexusInstallStage_Complete:
            // Trailing bytes after the last entry are padding.
            ins->stream_pos += remaining;
            remaining = 0;
            break;

        default:
            return NexusInstall_Aborted;
        }
    }

    return (ins->stage == NexusInstallStage_Complete) ? NexusInstall_Ok
                                                      : NexusInstall_InProgress;
}

// ---------------------------------------------------------------------------
// Finalisation
// ---------------------------------------------------------------------------

// Turns the CNMT into the meta record, adding the Meta NCA's own entry, which
// the CNMT does not list for itself.
static NexusInstallResult build_meta(NexusInstaller *ins, const CnmtContext *cnmt)
{
    NexusInstallMeta *m = &ins->meta;

    m->title_id   = cnmt->title_id;
    m->version    = cnmt->version;
    m->meta_type  = cnmt->meta_type;
    m->storage_id = ins->target_storage;

    // Carry the type-specific extended header through verbatim; ncm wants it
    // back exactly as it appeared.
    if (cnmt->extended_header_size > sizeof(m->ext_header)) return NexusInstall_BadCnmt;
    m->ext_header_size = cnmt->extended_header_size;
    if (cnmt->extended_header_size > 0) {
        memcpy(m->ext_header, cnmt->data + CNMT_HEADER_SIZE, cnmt->extended_header_size);
    }

    const u16 n = cnmtGetContentCount(cnmt);
    if ((u32)n + 1 > NEXUS_INSTALL_MAX_CONTENTS) return NexusInstall_TooManyContents;

    m->content_count = 0;

    // The Meta NCA first: its id comes from the filename and its size from the
    // PFS0 entry, because the CNMT never describes itself.
    NexusInstallContent *meta_entry = &m->contents[m->content_count++];
    memcpy(meta_entry->content_id, ins->meta_content_id, NEXUS_CONTENT_ID_SIZE);
    meta_entry->size         = ins->meta_nca_size;
    meta_entry->attr         = 0;
    meta_entry->content_type = CnmtContentType_Meta;
    meta_entry->id_offset    = 0;

    for (u16 i = 0; i < n; i++) {
        CnmtContentInfo info;
        if (cnmtGetContentInfo(cnmt, i, &info) != NexusFmt_Ok) return NexusInstall_BadCnmt;

        // Delta fragments are only meaningful for a delta update and are not
        // installable content; skipping them matches what the system does.
        if (info.content_type == CnmtContentType_DeltaFragment) continue;

        // Every content the CNMT lists must have actually arrived, or the
        // title would register with content that is not on the console.
        bool present = false;
        for (u32 j = 0; j < ins->entry_count; j++) {
            const NexusInstallEntry *e = &ins->entries[j];
            if (NEXUS_KIND_IS_CONTENT(e->kind)
                && memcmp(e->content_id, info.content_id, NEXUS_CONTENT_ID_SIZE) == 0) {
                present = true;
                break;
            }
        }
        if (!present) return NexusInstall_MissingContent;

        NexusInstallContent *c = &m->contents[m->content_count++];
        memcpy(c->content_id, info.content_id, NEXUS_CONTENT_ID_SIZE);
        c->size         = info.size;
        c->attr         = info.content_attributes;
        c->content_type = info.content_type;
        c->id_offset    = info.id_offset;
    }

    return NexusInstall_Ok;
}

NexusInstallResult nexusInstallFinish(NexusInstaller *ins)
{
    if (ins == NULL) return NexusInstall_BackendError;
    if (ins->stage == NexusInstallStage_Failed)   return NexusInstall_Aborted;
    if (ins->stage != NexusInstallStage_Complete) return fail(ins, NexusInstall_NotFinished);

    // A ticket without its certificate chain cannot be imported, and a title
    // that needs one will not launch, so treat a lone ticket as fatal rather
    // than silently installing something broken.
    if (ins->tik_len > 0) {
        if (ins->cert_len == 0) return fail(ins, NexusInstall_BufferTooSmall);

        if (ins->ops->import_ticket(ins->user, ins->tik, ins->tik_len,
                                    ins->cert, ins->cert_len) != 0) {
            return fail(ins, NexusInstall_BackendError);
        }

        TicketContext tik;
        if (ticketInit(&tik, ins->tik, ins->tik_len) == NexusFmt_Ok) {
            ins->meta.has_ticket = true;
            memcpy(ins->meta.rights_id, tik.rights_id, TICKET_RIGHTS_ID_SIZE);
        }
    }

    // The Meta NCA is registered by now, so Horizon will decrypt it for us.
    u8 cnmt_buf[NEXUS_INSTALL_MAX_CNMT];
    size_t cnmt_len = 0;
    if (ins->ops->read_cnmt(ins->user, ins->meta_content_id,
                            cnmt_buf, sizeof(cnmt_buf), &cnmt_len) != 0) {
        return fail(ins, NexusInstall_BackendError);
    }

    CnmtContext cnmt;
    if (cnmtInit(&cnmt, cnmt_buf, cnmt_len) != NexusFmt_Ok) {
        return fail(ins, NexusInstall_BadCnmt);
    }

    const NexusInstallResult r = build_meta(ins, &cnmt);
    if (r != NexusInstall_Ok) return fail(ins, r);

    if (ins->ops->register_meta(ins->user, &ins->meta) != 0) {
        return fail(ins, NexusInstall_BackendError);
    }

    // Without this the content is installed but invisible on the HOME menu.
    if (ins->ops->push_record(ins->user, &ins->meta) != 0) {
        return fail(ins, NexusInstall_BackendError);
    }

    return NexusInstall_Ok;
}

void nexusInstallAbort(NexusInstaller *ins)
{
    if (ins == NULL || ins->stage == NexusInstallStage_Failed) return;
    fail(ins, NexusInstall_Aborted);
}

const NexusInstallMeta *nexusInstallGetMeta(const NexusInstaller *ins)
{
    return (ins != NULL) ? &ins->meta : NULL;
}

u64 nexusInstallGetBytesWritten(const NexusInstaller *ins)
{
    return (ins != NULL) ? ins->bytes_written : 0;
}
