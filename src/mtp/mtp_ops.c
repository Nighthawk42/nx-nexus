// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP operation handlers.
//
// Each handler runs the data phase (if the operation has one) and then sends
// the response block. Protocol-level failures are reported to the host as a
// response code and return success, because the transaction itself completed;
// only USB failures propagate as an error Result.

#include <stdio.h>
#include <string.h>

#include "mtp_internal.h"
#include "nexus/log.h"

#define MTP_STANDARD_VERSION      100
#define MTP_VENDOR_EXTENSION_ID   0x00000006u   // Microsoft MTP
#define MTP_VENDOR_EXTENSION_VER  100

// Windows keys MTP support off this exact string.
#define MTP_VENDOR_EXTENSION_DESC "microsoft.com: 1.0;"

// Operations advertised in GetDeviceInfo. Keep in sync with the dispatch
// switch in mtpHandleCommand -- a host that sees an operation here will use it.
static const u16 g_supported_ops[] = {
    MtpOp_GetDeviceInfo,
    MtpOp_OpenSession,
    MtpOp_CloseSession,
    MtpOp_GetStorageIDs,
    MtpOp_GetStorageInfo,
    MtpOp_GetNumObjects,
    MtpOp_GetObjectHandles,
    MtpOp_GetObjectInfo,
    MtpOp_GetObject,
    MtpOp_GetThumb,
    MtpOp_GetPartialObject,
    MtpOp_DeleteObject,
    MtpOp_SendObjectInfo,
    MtpOp_SendObject,
    MtpOp_GetDevicePropDesc,
    MtpOp_GetDevicePropValue,
    MtpOp_GetObjectPropsSupported,
    MtpOp_GetObjectPropDesc,
    MtpOp_GetObjectPropValue,
    MtpOp_SetObjectPropValue,
    MtpOp_GetObjectPropList,
    MtpOp_MoveObject,
    MtpOp_CopyObject,
};

static const u16 g_supported_events[] = {
    MtpEvent_ObjectAdded,
    MtpEvent_ObjectRemoved,
    MtpEvent_StoreAdded,
    MtpEvent_StoreRemoved,
};

static const u16 g_supported_device_props[] = {
    MtpDevProp_DeviceFriendlyName,
    MtpDevProp_BatteryLevel,
};

static const u16 g_supported_object_props[] = {
    MtpObjProp_StorageID,
    MtpObjProp_ObjectFormat,
    MtpObjProp_ProtectionStatus,
    MtpObjProp_ObjectSize,
    MtpObjProp_ObjectFileName,
    MtpObjProp_ParentObject,
    MtpObjProp_PersistentUID,
    MtpObjProp_Name,
};

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

bool mtpNameIsSafe(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;

    // A host should never send a separator inside a single name component;
    // if it does, treat it as an attempt to escape the store root.
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    return true;
}

bool mtpPathJoin(const char *dir, const char *name, char *out, size_t out_size)
{
    if (!mtpNameIsSafe(name)) return false;

    const bool dir_has_slash = (dir[0] != '\0' && dir[strlen(dir) - 1] == '/');
    const int n = snprintf(out, out_size, "%s%s%s", dir, dir_has_slash ? "" : "/", name);
    return n > 0 && (size_t)n < out_size;
}

// Maps an object handle to its store and a copy of its entry.
// Returns an MTP response code.
static u16 resolve_handle(u32 handle, NexusStorage **out_store, NexusObject *out_obj)
{
    if (!nexusObjectDbGetCopy(handle, out_obj)) return MtpRes_InvalidObjectHandle;

    NexusStorage *s = nexusStorageById(out_obj->storage_id);
    if (s == NULL || !s->present) return MtpRes_StoreNotAvailable;

    *out_store = s;
    return MtpRes_OK;
}

// Guesses an MTP format code. Only the directory/file distinction actually
// matters to hosts for file transfer; everything else is Undefined.
static u16 format_for(bool is_dir)
{
    return is_dir ? MtpFormat_Association : MtpFormat_Undefined;
}

// ---------------------------------------------------------------------------
// Simple handlers
// ---------------------------------------------------------------------------

static Result op_get_device_info(const MtpContainer *cmd)
{
    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);

    mtpWriteU16(&w, MTP_STANDARD_VERSION);
    mtpWriteU32(&w, MTP_VENDOR_EXTENSION_ID);
    mtpWriteU16(&w, MTP_VENDOR_EXTENSION_VER);
    mtpWriteString(&w, MTP_VENDOR_EXTENSION_DESC);
    mtpWriteU16(&w, 0);   // FunctionalMode: standard

    mtpWriteU16Array(&w, g_supported_ops,
                     (u32)(sizeof(g_supported_ops) / sizeof(u16)));
    mtpWriteU16Array(&w, g_supported_events,
                     (u32)(sizeof(g_supported_events) / sizeof(u16)));
    mtpWriteU16Array(&w, g_supported_device_props,
                     (u32)(sizeof(g_supported_device_props) / sizeof(u16)));
    mtpWriteU16Array(&w, NULL, 0);   // CaptureFormats: none, this is not a camera

    // ImageFormats doubles as "object formats we accept" for many hosts.
    static const u16 image_formats[] = { MtpFormat_Undefined, MtpFormat_Association };
    mtpWriteU16Array(&w, image_formats, (u32)(sizeof(image_formats) / sizeof(u16)));

    mtpWriteString(&w, "NX-Nexus");
    mtpWriteString(&w, "Nintendo Switch");
    mtpWriteString(&w, "0.1.0");
    mtpWriteString(&w, "0000000000000001");

    if (!mtpWriterOk(&w)) {
        LOG_E("mtp: device info overflowed the payload buffer");
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_open_session(const MtpContainer *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0] == 0) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }
    if (g_mtp.session_open) {
        u32 p = g_mtp.session_id;
        return mtpSendResponse(MtpRes_SessionAlreadyOpen, cmd->transaction_id, &p, 1);
    }

    g_mtp.session_open = true;
    g_mtp.session_id   = cmd->params[0];
    nexusObjectDbClear();
    nexusStorageRegistryRefresh();
    nexusStorageRegistryOnSessionOpen();

    LOG_I("mtp: session %u opened", g_mtp.session_id);
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_close_session(const MtpContainer *cmd)
{
    LOG_I("mtp: session %u closed", g_mtp.session_id);
    mtpServerResetSession();
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_storage_ids(const MtpContainer *cmd)
{
    u32 ids[NEXUS_MAX_STORAGES];
    u32 count = 0;

    nexusStorageRegistryRefresh();
    for (size_t i = 0; i < nexusStorageCount() && count < NEXUS_MAX_STORAGES; i++) {
        const NexusStorage *s = nexusStorageAt(i);
        if (s != NULL && s->present) ids[count++] = s->storage_id;
    }

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);
    mtpWriteU32Array(&w, ids, count);
    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_storage_info(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = nexusStorageById(cmd->params[0]);
    if (s == NULL) {
        return mtpSendResponse(MtpRes_InvalidStorageID, cmd->transaction_id, NULL, 0);
    }
    if (!s->present) {
        return mtpSendResponse(MtpRes_StoreNotAvailable, cmd->transaction_id, NULL, 0);
    }

    NexusStorageInfo info;
    if (R_FAILED(s->ops->get_info(s, &info))) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);
    mtpWriteU16(&w, info.storage_type);
    mtpWriteU16(&w, info.filesystem_type);
    mtpWriteU16(&w, info.access_capability);
    mtpWriteU64(&w, info.capacity_bytes);
    mtpWriteU64(&w, info.free_bytes);
    mtpWriteU32(&w, info.free_objects);
    mtpWriteString(&w, s->ops->description(s));   // StorageDescription
    mtpWriteString(&w, "");                       // VolumeIdentifier

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// ---------------------------------------------------------------------------
// Object enumeration
// ---------------------------------------------------------------------------

typedef struct {
    u32          storage_id;
    u32          parent_handle;
    const char  *dir_path;
    u32          format_filter;   // MTP_FORMAT_ALL for no filter
    u32         *handles;
    u32          capacity;
    u32          count;
    bool         count_only;
} EnumCtx;

static bool enum_collect(void *user, const char *name, bool is_dir, s64 size)
{
    EnumCtx *ctx = (EnumCtx *)user;

    if (ctx->format_filter != MTP_FORMAT_ALL && ctx->format_filter != format_for(is_dir)) {
        return true;  // filtered out, keep going
    }

    if (ctx->count_only) {
        ctx->count++;
        return true;
    }

    if (ctx->count >= ctx->capacity) {
        LOG_W("mtp: object handle list truncated at %u entries", ctx->capacity);
        return false;
    }

    char child[NEXUS_PATH_MAX];
    if (!mtpPathJoin(ctx->dir_path, name, child, sizeof(child))) return true;  // skip

    const u32 handle = nexusObjectDbIntern(ctx->storage_id, ctx->parent_handle,
                                           child, is_dir, size);
    if (handle == 0) return false;   // out of memory; stop cleanly

    ctx->handles[ctx->count++] = handle;
    return true;
}

// Resolves the (storage, parent) pair a host passed to GetObjectHandles or
// GetNumObjects into a store and a directory path.
static u16 resolve_parent(u32 storage_id, u32 parent, NexusStorage **out_store,
                          char *out_dir, size_t out_dir_size, u32 *out_parent_handle)
{
    NexusStorage *s = nexusStorageById(storage_id);
    if (s == NULL) return MtpRes_InvalidStorageID;
    if (!s->present) return MtpRes_StoreNotAvailable;
    *out_store = s;

    // 0 and 0xFFFFFFFF both mean "the root of the store" depending on the
    // host; treat them the same.
    if (parent == 0 || parent == MTP_HANDLE_ROOT) {
        snprintf(out_dir, out_dir_size, "/");
        *out_parent_handle = MTP_HANDLE_ROOT;
        return MtpRes_OK;
    }

    NexusObject obj;
    if (!nexusObjectDbGetCopy(parent, &obj)) return MtpRes_InvalidParentObject;
    if (!obj.is_dir)                         return MtpRes_InvalidParentObject;
    if (obj.storage_id != storage_id)        return MtpRes_InvalidParentObject;

    snprintf(out_dir, out_dir_size, "%s", obj.path);
    *out_parent_handle = parent;
    return MtpRes_OK;
}

static Result op_get_object_handles(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    const u32 storage_id = cmd->params[0];
    const u32 format     = (cmd->param_count > 1) ? cmd->params[1] : MTP_FORMAT_ALL;
    const u32 parent     = (cmd->param_count > 2) ? cmd->params[2] : MTP_HANDLE_ROOT;

    // A wildcard storage id would mean "every store"; not supported, and hosts
    // fall back to per-store queries when told so.
    if (storage_id == MTP_STORAGE_ALL) {
        return mtpSendResponse(MtpRes_SpecByFormatUnsupported, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    char dir[NEXUS_PATH_MAX];
    u32  parent_handle = MTP_HANDLE_ROOT;
    const u16 res = resolve_parent(storage_id, parent, &s, dir, sizeof(dir), &parent_handle);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    // The handle array is built directly in the payload buffer, after the
    // container header and the u32 element count.
    u8 *const array_base = g_mtp_payload + MTP_CONTAINER_HEADER_SIZE + 4;
    const u32 capacity = (u32)((MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE - 4)
                               / sizeof(u32));

    EnumCtx ctx = {
        .storage_id    = storage_id,
        .parent_handle = parent_handle,
        .dir_path      = dir,
        .format_filter = format,
        .handles       = (u32 *)array_base,
        .capacity      = capacity,
        .count         = 0,
        .count_only    = false,
    };

    if (R_FAILED(s->ops->enumerate(s, dir, enum_collect, &ctx))) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    // Prepend the element count, then the payload is already contiguous.
    memcpy(g_mtp_payload + MTP_CONTAINER_HEADER_SIZE, &ctx.count, 4);
    const size_t payload_len = 4 + ((size_t)ctx.count * sizeof(u32));

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, payload_len);
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_num_objects(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    const u32 storage_id = cmd->params[0];
    const u32 format     = (cmd->param_count > 1) ? cmd->params[1] : MTP_FORMAT_ALL;
    const u32 parent     = (cmd->param_count > 2) ? cmd->params[2] : MTP_HANDLE_ROOT;

    NexusStorage *s = NULL;
    char dir[NEXUS_PATH_MAX];
    u32  parent_handle = MTP_HANDLE_ROOT;
    const u16 res = resolve_parent(storage_id, parent, &s, dir, sizeof(dir), &parent_handle);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    EnumCtx ctx = {
        .storage_id    = storage_id,
        .parent_handle = parent_handle,
        .dir_path      = dir,
        .format_filter = format,
        .count_only    = true,
    };

    if (R_FAILED(s->ops->enumerate(s, dir, enum_collect, &ctx))) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    // GetNumObjects has no data phase; the count rides in the response.
    const u32 count = ctx.count;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, &count, 1);
}

// ---------------------------------------------------------------------------
// Object metadata
// ---------------------------------------------------------------------------

// Extracts the last path component. Returns a pointer into path.
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

// Cheap probe: does this object have artwork at all? Generating the thumbnail
// to find out would make every directory listing expensive, so stores answer
// by declaring the hook and the actual fetch happens only on GetThumb.
static bool object_has_thumbnail(NexusStorage *s, const NexusObject *obj)
{
    return s->ops->thumbnail != NULL && obj->is_dir;
}

static Result op_get_object_info(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    // Re-stat so the host sees the current size rather than a cached one.
    bool is_dir = obj.is_dir;
    s64  size   = obj.size;
    if (R_FAILED(s->ops->stat(s, obj.path, &is_dir, &size))) {
        return mtpSendResponse(MtpRes_InvalidObjectHandle, cmd->transaction_id, NULL, 0);
    }

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);

    mtpWriteU32(&w, obj.storage_id);
    mtpWriteU16(&w, format_for(is_dir));
    mtpWriteU16(&w, MtpProtection_None);
    // ObjectCompressedSize is 32-bit in this legacy dataset; hosts read the
    // real size from the ObjectSize property for files above 4 GiB.
    mtpWriteU32(&w, (size > 0xFFFFFFFFll) ? 0xFFFFFFFFu : (u32)size);
    // Advertising a thumbnail here is what makes a host bother to ask for one
    // with GetThumb. The size is not known without generating it, and hosts
    // treat this field as advisory, so a nominal value is enough.
    const bool has_thumb = object_has_thumbnail(s, &obj);

    mtpWriteU16(&w, has_thumb ? MtpFormat_EXIF_JPEG : 0);          // ThumbFormat
    mtpWriteU32(&w, has_thumb ? NEXUS_THUMB_MAX_BYTES : 0);        // ThumbCompressedSize
    mtpWriteU32(&w, has_thumb ? NEXUS_THUMB_DIM : 0);              // ThumbPixWidth
    mtpWriteU32(&w, has_thumb ? NEXUS_THUMB_DIM : 0);              // ThumbPixHeight
    mtpWriteU32(&w, 0);          // ImagePixWidth
    mtpWriteU32(&w, 0);          // ImagePixHeight
    mtpWriteU32(&w, 0);          // ImageBitDepth
    mtpWriteU32(&w, obj.parent == MTP_HANDLE_ROOT ? 0 : obj.parent);
    mtpWriteU16(&w, is_dir ? 1 : 0);   // AssociationType: 1 == generic folder
    mtpWriteU32(&w, 0);          // AssociationDesc
    mtpWriteU32(&w, 0);          // SequenceNumber
    mtpWriteString(&w, path_basename(obj.path));
    mtpWriteString(&w, "");      // DateCreated
    mtpWriteString(&w, "");      // DateModified
    mtpWriteString(&w, "");      // Keywords

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// Sends a title's icon as the object's thumbnail. Hosts call this while
// drawing a folder listing, so it has to be cheap and must never block for
// long -- the icon comes out of ns, which caches it.
static Result op_get_thumb(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    if (s->ops->thumbnail == NULL) {
        return mtpSendResponse(MtpRes_NoThumbnailPresent, cmd->transaction_id, NULL, 0);
    }

    u8 *const buf = g_mtp_payload + MTP_CONTAINER_HEADER_SIZE;
    const size_t cap = (MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE < NEXUS_THUMB_MAX_BYTES)
        ? MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE
        : NEXUS_THUMB_MAX_BYTES;

    size_t len = 0;
    if (R_FAILED(s->ops->thumbnail(s, obj.path, buf, cap, &len)) || len == 0) {
        return mtpSendResponse(MtpRes_NoThumbnailPresent, cmd->transaction_id, NULL, 0);
    }

    const Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, len);
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// ---------------------------------------------------------------------------
// Object data transfer
// ---------------------------------------------------------------------------

// Streams a byte range of an object as a data block. Used by both GetObject
// (whole file) and GetPartialObject (a range).
static Result send_object_range(const MtpContainer *cmd, NexusStorage *s,
                                const NexusObject *obj, u64 offset, u64 length,
                                u32 *out_actual)
{
    u8 *const chunk_buf = g_mtp_payload + MTP_CONTAINER_HEADER_SIZE;
    const size_t first_capacity = MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE;

    // Read the first chunk so it can share a transfer with the header.
    size_t first_len = (length < first_capacity) ? (size_t)length : first_capacity;
    size_t got = 0;
    if (first_len > 0) {
        if (R_FAILED(s->ops->read(s, obj->path, offset, chunk_buf, first_len, &got))) {
            return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
        }
    }

    // The file may be shorter than the host asked for; the data block must
    // declare what we will actually send.
    u64 total = got;
    if (got == first_len && length > first_len) {
        total = length;   // provisional: refined as we read
    }

    Result rc = mtpSendDataStreamBegin(cmd->code, cmd->transaction_id, total, got);
    if (R_FAILED(rc)) return rc;

    u64 sent = got;
    const u64 start_tick = armGetSystemTick();

    while (sent < length) {
        u64 want = length - sent;
        if (want > MTP_PAYLOAD_BUF_SIZE) want = MTP_PAYLOAD_BUF_SIZE;

        size_t n = 0;
        if (R_FAILED(s->ops->read(s, obj->path, offset + sent, g_mtp_payload,
                                  (size_t)want, &n))) {
            break;   // truncated transfer; the host will see a short data phase
        }
        if (n == 0) break;   // end of file

        rc = mtpSendDataStreamChunk(n);
        if (R_FAILED(rc)) return rc;
        sent += n;
    }

    rc = mtpSendDataStreamEnd();
    if (R_FAILED(rc)) return rc;

    const u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
    if (elapsed_ns > 0) g_mtp.stats.last_rate_bps = (sent * 1000000000ull) / elapsed_ns;

    if (out_actual) *out_actual = (sent > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)sent;
    return 0;
}

static Result op_get_object(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);
    if (obj.is_dir) {
        return mtpSendResponse(MtpRes_InvalidObjectHandle, cmd->transaction_id, NULL, 0);
    }

    bool is_dir = false;
    s64  size   = 0;
    if (R_FAILED(s->ops->stat(s, obj.path, &is_dir, &size))) {
        return mtpSendResponse(MtpRes_InvalidObjectHandle, cmd->transaction_id, NULL, 0);
    }

    Result rc = send_object_range(cmd, s, &obj, 0, (u64)size, NULL);
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_partial_object(const MtpContainer *cmd)
{
    if (cmd->param_count < 3) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);
    if (obj.is_dir) {
        return mtpSendResponse(MtpRes_InvalidObjectHandle, cmd->transaction_id, NULL, 0);
    }

    const u64 offset = cmd->params[1];
    const u64 length = cmd->params[2];

    u32 actual = 0;
    Result rc = send_object_range(cmd, s, &obj, offset, length, &actual);
    if (R_FAILED(rc)) return rc;

    // GetPartialObject reports the byte count actually sent.
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, &actual, 1);
}

// ---------------------------------------------------------------------------
// Upload: SendObjectInfo then SendObject
// ---------------------------------------------------------------------------

static Result op_send_object_info(const MtpContainer *cmd)
{
    const u32 storage_id = (cmd->param_count > 0) ? cmd->params[0] : 0;
    const u32 parent     = (cmd->param_count > 1) ? cmd->params[1] : MTP_HANDLE_ROOT;

    size_t payload_len = 0;
    Result rc = mtpRecvDataToPayload(&payload_len);
    if (R_FAILED(rc)) {
        LOG_E("mtp: SendObjectInfo dataset read failed (0x%x)", rc);
        return mtpSendResponse(MtpRes_IncompleteTransfer, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    char dir[NEXUS_PATH_MAX];
    u32  parent_handle = MTP_HANDLE_ROOT;
    const u16 res = resolve_parent(storage_id, parent, &s, dir, sizeof(dir), &parent_handle);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    if (s->ops->write_begin == NULL) {
        return mtpSendResponse(MtpRes_StoreReadOnly, cmd->transaction_id, NULL, 0);
    }

    // Parse the ObjectInfo dataset. Only a handful of fields matter here.
    MtpReader r;
    mtpReaderInit(&r, g_mtp_payload, payload_len);

    mtpReadU32(&r);                        // StorageID (host echo; params win)
    const u16 format        = mtpReadU16(&r);
    mtpReadU16(&r);                        // ProtectionStatus
    const u32 compressed_sz = mtpReadU32(&r);
    mtpReadU16(&r);                        // ThumbFormat
    mtpReadU32(&r);                        // ThumbCompressedSize
    mtpReadU32(&r);                        // ThumbPixWidth
    mtpReadU32(&r);                        // ThumbPixHeight
    mtpReadU32(&r);                        // ImagePixWidth
    mtpReadU32(&r);                        // ImagePixHeight
    mtpReadU32(&r);                        // ImageBitDepth
    mtpReadU32(&r);                        // ParentObject (host echo)
    mtpReadU16(&r);                        // AssociationType
    mtpReadU32(&r);                        // AssociationDesc
    mtpReadU32(&r);                        // SequenceNumber

    char filename[NEXUS_PATH_MAX];
    mtpReadString(&r, filename, sizeof(filename));

    if (!mtpReaderOk(&r) || !mtpNameIsSafe(filename)) {
        LOG_W("mtp: rejected SendObjectInfo (malformed dataset or unsafe name)");
        return mtpSendResponse(MtpRes_InvalidDataset, cmd->transaction_id, NULL, 0);
    }

    char path[NEXUS_PATH_MAX];
    if (!mtpPathJoin(dir, filename, path, sizeof(path))) {
        return mtpSendResponse(MtpRes_InvalidDataset, cmd->transaction_id, NULL, 0);
    }

    // A folder is created immediately; there is no SendObject phase for one.
    if (format == MtpFormat_Association) {
        if (s->ops->mkdir == NULL) {
            return mtpSendResponse(MtpRes_OperationNotSupported, cmd->transaction_id, NULL, 0);
        }
        if (R_FAILED(s->ops->mkdir(s, path))) {
            return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
        }

        const u32 handle = nexusObjectDbIntern(storage_id, parent_handle, path, true, 0);
        if (handle == 0) {
            return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
        }

        const u32 params[3] = { storage_id, parent_handle, handle };
        LOG_I("mtp: created folder %s", path);
        return mtpSendResponse(MtpRes_OK, cmd->transaction_id, params, 3);
    }

    // File: reserve a handle now and open the write transaction. The bytes
    // arrive in the following SendObject operation.
    const u32 handle = nexusObjectDbIntern(storage_id, parent_handle, path, false, 0);
    if (handle == 0) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    if (R_FAILED(s->ops->write_begin(s, path, compressed_sz))) {
        nexusObjectDbInvalidate(handle);
        return mtpSendResponse(MtpRes_AccessDenied, cmd->transaction_id, NULL, 0);
    }

    g_mtp.pending_send       = true;
    g_mtp.pending_storage_id = storage_id;
    g_mtp.pending_parent     = parent_handle;
    g_mtp.pending_size       = compressed_sz;
    g_mtp.pending_handle     = handle;
    snprintf(g_mtp.pending_path, sizeof(g_mtp.pending_path), "%s", path);

    const u32 params[3] = { storage_id, parent_handle, handle };
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, params, 3);
}

typedef struct {
    NexusStorage *store;
    Result        error;
} SendSinkCtx;

static Result send_object_sink(void *user, const void *data, size_t size)
{
    SendSinkCtx *ctx = (SendSinkCtx *)user;
    Result rc = ctx->store->ops->write_chunk(ctx->store, data, size);
    if (R_FAILED(rc)) ctx->error = rc;
    return rc;
}

static Result op_send_object(const MtpContainer *cmd)
{
    if (!g_mtp.pending_send) {
        // SendObject without a preceding SendObjectInfo.
        return mtpSendResponse(MtpRes_NoValidObjectInfo, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = nexusStorageById(g_mtp.pending_storage_id);
    if (s == NULL || s->ops->write_chunk == NULL) {
        g_mtp.pending_send = false;
        return mtpSendResponse(MtpRes_StoreNotAvailable, cmd->transaction_id, NULL, 0);
    }

    SendSinkCtx ctx = { .store = s, .error = 0 };
    u64 received = 0;
    Result rc = mtpRecvDataStreamed(send_object_sink, &ctx, &received);

    const bool io_failed  = R_FAILED(rc) || R_FAILED(ctx.error);
    const bool short_xfer = (g_mtp.pending_size != 0xFFFFFFFFu)
                            && (received < g_mtp.pending_size);

    s->ops->write_end(s, !io_failed && !short_xfer);

    if (io_failed) {
        LOG_E("mtp: SendObject failed for %s (0x%x)", g_mtp.pending_path,
              R_FAILED(ctx.error) ? ctx.error : rc);
        nexusObjectDbInvalidate(g_mtp.pending_handle);
        g_mtp.pending_send = false;
        // A USB-level failure must propagate so the loop can resynchronise.
        if (R_FAILED(rc)) return rc;
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    if (short_xfer) {
        LOG_W("mtp: %s truncated (%llu of %llu bytes)", g_mtp.pending_path,
              (unsigned long long)received, (unsigned long long)g_mtp.pending_size);
        nexusObjectDbInvalidate(g_mtp.pending_handle);
        g_mtp.pending_send = false;
        return mtpSendResponse(MtpRes_IncompleteTransfer, cmd->transaction_id, NULL, 0);
    }

    LOG_I("mtp: received %s (%llu bytes, %llu KiB/s)", g_mtp.pending_path,
          (unsigned long long)received,
          (unsigned long long)(g_mtp.stats.last_rate_bps / 1024ull));

    g_mtp.pending_send = false;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_delete_object(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }
    if (cmd->params[0] == MTP_HANDLE_ALL) {
        // Deleting every object on a store is a foot-gun we decline to offer.
        return mtpSendResponse(MtpRes_OperationNotSupported, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    if (s->ops->remove == NULL) {
        return mtpSendResponse(MtpRes_StoreReadOnly, cmd->transaction_id, NULL, 0);
    }
    if (R_FAILED(s->ops->remove(s, obj.path, obj.is_dir))) {
        return mtpSendResponse(MtpRes_AccessDenied, cmd->transaction_id, NULL, 0);
    }

    nexusObjectDbInvalidate(obj.handle);
    LOG_I("mtp: deleted %s", obj.path);
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

static Result op_get_object_props_supported(const MtpContainer *cmd)
{
    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);
    mtpWriteU16Array(&w, g_supported_object_props,
                     (u32)(sizeof(g_supported_object_props) / sizeof(u16)));
    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_object_prop_value(const MtpContainer *cmd)
{
    if (cmd->param_count < 2) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    bool is_dir = obj.is_dir;
    s64  size   = obj.size;
    s->ops->stat(s, obj.path, &is_dir, &size);   // best effort refresh

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);

    switch (cmd->params[1]) {
        case MtpObjProp_StorageID:
            mtpWriteU32(&w, obj.storage_id);
            break;
        case MtpObjProp_ObjectFormat:
            mtpWriteU16(&w, format_for(is_dir));
            break;
        case MtpObjProp_ProtectionStatus:
            mtpWriteU16(&w, MtpProtection_None);
            break;
        case MtpObjProp_ObjectSize:
            mtpWriteU64(&w, (u64)size);
            break;
        case MtpObjProp_ParentObject:
            mtpWriteU32(&w, obj.parent == MTP_HANDLE_ROOT ? 0 : obj.parent);
            break;
        case MtpObjProp_PersistentUID:
            // The handle is stable for a session, which is the most this
            // backend can honestly promise.
            mtpWriteU128(&w, obj.handle, obj.storage_id);
            break;
        case MtpObjProp_ObjectFileName:
        case MtpObjProp_Name:
            mtpWriteString(&w, path_basename(obj.path));
            break;
        default:
            return mtpSendResponse(MtpRes_ObjectPropNotSupported, cmd->transaction_id, NULL, 0);
    }

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_object_prop_desc(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    const u16 prop = (u16)cmd->params[0];
    u16 type;
    bool writable = false;

    switch (prop) {
        case MtpObjProp_StorageID:        type = MtpType_UINT32; break;
        case MtpObjProp_ObjectFormat:     type = MtpType_UINT16; break;
        case MtpObjProp_ProtectionStatus: type = MtpType_UINT16; break;
        case MtpObjProp_ObjectSize:       type = MtpType_UINT64; break;
        case MtpObjProp_ParentObject:     type = MtpType_UINT32; break;
        case MtpObjProp_PersistentUID:    type = MtpType_UINT128; break;
        case MtpObjProp_ObjectFileName:   type = MtpType_STR; writable = true; break;
        case MtpObjProp_Name:             type = MtpType_STR; break;
        default:
            return mtpSendResponse(MtpRes_ObjectPropNotSupported, cmd->transaction_id, NULL, 0);
    }

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);
    mtpWriteU16(&w, prop);
    mtpWriteU16(&w, type);
    mtpWriteU8(&w, writable ? 0x01 : 0x00);   // GetSet

    // Default value, typed to match.
    if (type == MtpType_STR)          mtpWriteString(&w, "");
    else if (type == MtpType_UINT128) mtpWriteU128(&w, 0, 0);
    else if (type == MtpType_UINT64)  mtpWriteU64(&w, 0);
    else if (type == MtpType_UINT32)  mtpWriteU32(&w, 0);
    else                              mtpWriteU16(&w, 0);

    mtpWriteU32(&w, 0);   // GroupCode
    mtpWriteU8(&w, 0);    // FormFlag: none

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_device_prop_value(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);

    switch (cmd->params[0]) {
        case MtpDevProp_DeviceFriendlyName:
            mtpWriteString(&w, "NX-Nexus");
            break;
        case MtpDevProp_BatteryLevel: {
            u32 charge = 0;
            // psmGetBatteryChargePercentage needs psmInitialize(); report 100
            // when the service is unavailable rather than failing the call.
            if (R_FAILED(psmGetBatteryChargePercentage(&charge))) charge = 100;
            mtpWriteU8(&w, (u8)charge);
            break;
        }
        default:
            return mtpSendResponse(MtpRes_DevicePropNotSupported, cmd->transaction_id, NULL, 0);
    }

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

static Result op_get_device_prop_desc(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    const u16 prop = (u16)cmd->params[0];
    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE);

    switch (prop) {
        case MtpDevProp_DeviceFriendlyName:
            mtpWriteU16(&w, prop);
            mtpWriteU16(&w, MtpType_STR);
            mtpWriteU8(&w, 0x00);        // read-only
            mtpWriteString(&w, "NX-Nexus");   // factory default
            mtpWriteString(&w, "NX-Nexus");   // current value
            mtpWriteU8(&w, 0);           // FormFlag
            break;
        case MtpDevProp_BatteryLevel:
            mtpWriteU16(&w, prop);
            mtpWriteU16(&w, MtpType_UINT8);
            mtpWriteU8(&w, 0x00);
            mtpWriteU8(&w, 100);
            mtpWriteU8(&w, 100);
            mtpWriteU8(&w, 0x01);        // FormFlag: range
            mtpWriteU8(&w, 0);           // min
            mtpWriteU8(&w, 100);         // max
            mtpWriteU8(&w, 1);           // step
            break;
        default:
            return mtpSendResponse(MtpRes_DevicePropNotSupported, cmd->transaction_id, NULL, 0);
    }

    if (!mtpWriterOk(&w)) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, mtpWriterLength(&w));
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// ---------------------------------------------------------------------------
// Rename, move and copy
//
// Hosts rename through SetObjectPropValue(ObjectFileName) rather than a
// dedicated operation, so without that a file manager cannot rename anything.
// ---------------------------------------------------------------------------

// Replaces the final component of path with name.
static bool path_with_new_name(const char *path, const char *name,
                               char *out, size_t out_size)
{
    if (!mtpNameIsSafe(name)) return false;

    const char *slash = strrchr(path, '/');
    if (slash == NULL) return false;

    const size_t dir_len = (size_t)(slash - path);   // excludes the slash
    const int n = snprintf(out, out_size, "%.*s/%s", (int)dir_len, path, name);
    return n > 0 && (size_t)n < out_size;
}

static Result op_set_object_prop_value(const MtpContainer *cmd)
{
    if (cmd->param_count < 2) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *s = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &s, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    // The new value arrives in a data phase, which must be consumed even when
    // the property is one we refuse, or the transaction desynchronises.
    size_t payload_len = 0;
    Result rc = mtpRecvDataToPayload(&payload_len);
    if (R_FAILED(rc)) {
        return mtpSendResponse(MtpRes_IncompleteTransfer, cmd->transaction_id, NULL, 0);
    }

    if (cmd->params[1] != MtpObjProp_ObjectFileName
        && cmd->params[1] != MtpObjProp_Name) {
        return mtpSendResponse(MtpRes_ObjectPropNotSupported, cmd->transaction_id, NULL, 0);
    }

    if (s->ops->move == NULL) {
        return mtpSendResponse(MtpRes_StoreReadOnly, cmd->transaction_id, NULL, 0);
    }

    MtpReader r;
    mtpReaderInit(&r, g_mtp_payload, payload_len);

    char new_name[NEXUS_PATH_MAX];
    mtpReadString(&r, new_name, sizeof(new_name));
    if (!mtpReaderOk(&r) || !mtpNameIsSafe(new_name)) {
        return mtpSendResponse(MtpRes_InvalidObjectPropValue, cmd->transaction_id, NULL, 0);
    }

    char new_path[NEXUS_PATH_MAX];
    if (!path_with_new_name(obj.path, new_name, new_path, sizeof(new_path))) {
        return mtpSendResponse(MtpRes_InvalidObjectPropValue, cmd->transaction_id, NULL, 0);
    }

    if (R_FAILED(s->ops->move(s, obj.path, new_path, obj.is_dir))) {
        return mtpSendResponse(MtpRes_AccessDenied, cmd->transaction_id, NULL, 0);
    }

    // The handle must keep pointing at the same object under its new name, or
    // the host's view goes stale immediately after a rename.
    nexusObjectDbRename(obj.handle, new_path);

    LOG_I("mtp: renamed %s -> %s", obj.path, new_path);
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// Shared body for MoveObject and CopyObject: both take
// (handle, target storage, target parent).
static Result move_or_copy(const MtpContainer *cmd, bool is_move)
{
    if (cmd->param_count < 3) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    NexusStorage *src = NULL;
    NexusObject obj;
    const u16 res = resolve_handle(cmd->params[0], &src, &obj);
    if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

    NexusStorage *dst = NULL;
    char dir[NEXUS_PATH_MAX];
    u32  parent_handle = MTP_HANDLE_ROOT;
    const u16 pres = resolve_parent(cmd->params[1], cmd->params[2], &dst,
                                    dir, sizeof(dir), &parent_handle);
    if (pres != MtpRes_OK) return mtpSendResponse(pres, cmd->transaction_id, NULL, 0);

    // Crossing stores would mean streaming the object through this process.
    // Hosts fall back to a read-then-write when told this is unsupported,
    // which is both simpler and no slower.
    if (dst != src) {
        return mtpSendResponse(MtpRes_SpecOfDestinationUnsupported,
                               cmd->transaction_id, NULL, 0);
    }

    char new_path[NEXUS_PATH_MAX];
    if (!mtpPathJoin(dir, path_basename(obj.path), new_path, sizeof(new_path))) {
        return mtpSendResponse(MtpRes_InvalidParentObject, cmd->transaction_id, NULL, 0);
    }

    if (is_move) {
        if (src->ops->move == NULL) {
            return mtpSendResponse(MtpRes_StoreReadOnly, cmd->transaction_id, NULL, 0);
        }
        if (R_FAILED(src->ops->move(src, obj.path, new_path, obj.is_dir))) {
            return mtpSendResponse(MtpRes_AccessDenied, cmd->transaction_id, NULL, 0);
        }

        nexusObjectDbRename(obj.handle, new_path);
        LOG_I("mtp: moved %s -> %s", obj.path, new_path);
        return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
    }

    if (src->ops->copy == NULL) {
        return mtpSendResponse(MtpRes_OperationNotSupported, cmd->transaction_id, NULL, 0);
    }
    if (obj.is_dir) {
        // Recursive directory copy is not offered; hosts handle it by walking
        // the tree themselves.
        return mtpSendResponse(MtpRes_OperationNotSupported, cmd->transaction_id, NULL, 0);
    }
    if (R_FAILED(src->ops->copy(src, obj.path, new_path))) {
        return mtpSendResponse(MtpRes_AccessDenied, cmd->transaction_id, NULL, 0);
    }

    // CopyObject returns the new object's handle.
    const u32 handle = nexusObjectDbIntern(dst->storage_id, parent_handle,
                                           new_path, false, obj.size);
    if (handle == 0) {
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    LOG_I("mtp: copied %s -> %s", obj.path, new_path);
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, &handle, 1);
}

static Result op_move_object(const MtpContainer *cmd) { return move_or_copy(cmd, true); }
static Result op_copy_object(const MtpContainer *cmd) { return move_or_copy(cmd, false); }

// ---------------------------------------------------------------------------
// GetObjectPropList
//
// Windows uses this to fetch every property of every child in one round trip.
// Without it, browsing a directory costs several transactions per file, which
// is the difference between a folder opening instantly and taking seconds.
// ---------------------------------------------------------------------------

typedef struct {
    MtpWriter   *w;
    NexusStorage *store;
    const char  *dir_path;
    u32          parent_handle;
    u32          element_count;
} PropListCtx;

// Appends one (handle, property, type, value) quadruple.
static void proplist_emit_u32(MtpWriter *w, u32 handle, u16 prop, u16 type, u32 value)
{
    mtpWriteU32(w, handle);
    mtpWriteU16(w, prop);
    mtpWriteU16(w, type);
    mtpWriteU32(w, value);
}

static void proplist_emit_for(PropListCtx *ctx, u32 handle, const char *name,
                              bool is_dir, s64 size, u32 parent)
{
    MtpWriter *w = ctx->w;

    proplist_emit_u32(w, handle, MtpObjProp_StorageID, MtpType_UINT32,
                      ctx->store->storage_id);
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_ObjectFormat);
    mtpWriteU16(w, MtpType_UINT16);
    mtpWriteU16(w, format_for(is_dir));
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_ProtectionStatus);
    mtpWriteU16(w, MtpType_UINT16);
    mtpWriteU16(w, MtpProtection_None);
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_ObjectSize);
    mtpWriteU16(w, MtpType_UINT64);
    mtpWriteU64(w, (u64)size);
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_ObjectFileName);
    mtpWriteU16(w, MtpType_STR);
    mtpWriteString(w, name);
    ctx->element_count++;

    proplist_emit_u32(w, handle, MtpObjProp_ParentObject, MtpType_UINT32,
                      parent == MTP_HANDLE_ROOT ? 0 : parent);
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_PersistentUID);
    mtpWriteU16(w, MtpType_UINT128);
    mtpWriteU128(w, handle, ctx->store->storage_id);
    ctx->element_count++;

    mtpWriteU32(w, handle);
    mtpWriteU16(w, MtpObjProp_Name);
    mtpWriteU16(w, MtpType_STR);
    mtpWriteString(w, name);
    ctx->element_count++;
}

static bool proplist_collect(void *user, const char *name, bool is_dir, s64 size)
{
    PropListCtx *ctx = (PropListCtx *)user;

    char child[NEXUS_PATH_MAX];
    if (!mtpPathJoin(ctx->dir_path, name, child, sizeof(child))) return true;

    const u32 handle = nexusObjectDbIntern(ctx->store->storage_id, ctx->parent_handle,
                                           child, is_dir, size);
    if (handle == 0) return false;

    proplist_emit_for(ctx, handle, name, is_dir, size, ctx->parent_handle);

    // Stop cleanly once the payload is full rather than truncating mid-value.
    return mtpWriterOk(ctx->w);
}

static Result op_get_object_prop_list(const MtpContainer *cmd)
{
    if (cmd->param_count < 1) {
        return mtpSendResponse(MtpRes_InvalidParameter, cmd->transaction_id, NULL, 0);
    }

    // Params: object handle, format, property code, group, depth.
    const u32 handle = cmd->params[0];
    const u32 depth  = (cmd->param_count > 4) ? cmd->params[4] : 0;

    MtpWriter w;
    mtpWriterInit(&w, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE + 4,
                  MTP_PAYLOAD_BUF_SIZE - MTP_CONTAINER_HEADER_SIZE - 4);

    PropListCtx ctx = { .w = &w, .element_count = 0 };

    // A wildcard handle means "every object on the store", which for a large
    // SD card would not fit in one data phase. Declining it makes the host
    // fall back to per-folder queries, which is what we want anyway.
    if (handle == MTP_HANDLE_ALL || handle == 0) {
        return mtpSendResponse(MtpRes_SpecByFormatUnsupported,
                               cmd->transaction_id, NULL, 0);
    }

    if (depth == 1) {
        // Every child of one folder -- the form hosts use when opening a
        // directory, and the reason this operation is worth having.
        NexusObject obj;
        NexusStorage *s = NULL;

        const u16 res = resolve_handle(handle, &s, &obj);
        if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);
        if (!obj.is_dir) {
            return mtpSendResponse(MtpRes_InvalidObjectHandle, cmd->transaction_id, NULL, 0);
        }

        char dir[NEXUS_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", obj.path);

        ctx.store         = s;
        ctx.dir_path      = dir;
        ctx.parent_handle = obj.handle;

        if (R_FAILED(s->ops->enumerate(s, dir, proplist_collect, &ctx))) {
            return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
        }
    } else {
        // A single object's properties.
        NexusStorage *s = NULL;
        NexusObject obj;
        const u16 res = resolve_handle(handle, &s, &obj);
        if (res != MtpRes_OK) return mtpSendResponse(res, cmd->transaction_id, NULL, 0);

        bool is_dir = obj.is_dir;
        s64  size   = obj.size;
        s->ops->stat(s, obj.path, &is_dir, &size);

        ctx.store         = s;
        ctx.dir_path      = obj.path;
        ctx.parent_handle = obj.parent;

        proplist_emit_for(&ctx, obj.handle, path_basename(obj.path),
                          is_dir, size, obj.parent);
    }

    if (!mtpWriterOk(&w)) {
        LOG_W("mtp: prop list overflowed, falling back to per-object queries");
        return mtpSendResponse(MtpRes_GeneralError, cmd->transaction_id, NULL, 0);
    }

    // The dataset is a u32 element count followed by the quadruples, which are
    // already in place immediately after it.
    memcpy(g_mtp_payload + MTP_CONTAINER_HEADER_SIZE, &ctx.element_count, 4);
    const size_t payload_len = 4 + mtpWriterLength(&w);

    Result rc = mtpSendDataFromPayload(cmd->code, cmd->transaction_id, payload_len);
    if (R_FAILED(rc)) return rc;
    return mtpSendResponse(MtpRes_OK, cmd->transaction_id, NULL, 0);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

Result mtpHandleCommand(const MtpContainer *cmd)
{
    // Everything except GetDeviceInfo and OpenSession requires a session.
    if (!g_mtp.session_open
        && cmd->code != MtpOp_GetDeviceInfo
        && cmd->code != MtpOp_OpenSession) {
        LOG_W("mtp: op 0x%04x rejected, no session", cmd->code);
        return mtpSendResponse(MtpRes_SessionNotOpen, cmd->transaction_id, NULL, 0);
    }

    // SendObject must directly follow SendObjectInfo. Any other operation
    // arriving in between abandons the pending upload.
    if (g_mtp.pending_send && cmd->code != MtpOp_SendObject) {
        LOG_W("mtp: op 0x%04x abandoned pending upload of %s",
              cmd->code, g_mtp.pending_path);
        NexusStorage *s = nexusStorageById(g_mtp.pending_storage_id);
        if (s != NULL && s->ops->write_end != NULL) s->ops->write_end(s, false);
        nexusObjectDbInvalidate(g_mtp.pending_handle);
        g_mtp.pending_send = false;
    }

    switch (cmd->code) {
        case MtpOp_GetDeviceInfo:           return op_get_device_info(cmd);
        case MtpOp_OpenSession:             return op_open_session(cmd);
        case MtpOp_CloseSession:            return op_close_session(cmd);
        case MtpOp_GetStorageIDs:           return op_get_storage_ids(cmd);
        case MtpOp_GetStorageInfo:          return op_get_storage_info(cmd);
        case MtpOp_GetNumObjects:           return op_get_num_objects(cmd);
        case MtpOp_GetObjectHandles:        return op_get_object_handles(cmd);
        case MtpOp_GetObjectInfo:           return op_get_object_info(cmd);
        case MtpOp_GetObject:               return op_get_object(cmd);
        case MtpOp_GetThumb:                return op_get_thumb(cmd);
        case MtpOp_GetPartialObject:        return op_get_partial_object(cmd);
        case MtpOp_DeleteObject:            return op_delete_object(cmd);
        case MtpOp_SendObjectInfo:          return op_send_object_info(cmd);
        case MtpOp_SendObject:              return op_send_object(cmd);
        case MtpOp_GetObjectPropsSupported: return op_get_object_props_supported(cmd);
        case MtpOp_GetObjectPropDesc:       return op_get_object_prop_desc(cmd);
        case MtpOp_GetObjectPropValue:      return op_get_object_prop_value(cmd);
        case MtpOp_SetObjectPropValue:      return op_set_object_prop_value(cmd);
        case MtpOp_GetObjectPropList:       return op_get_object_prop_list(cmd);
        case MtpOp_MoveObject:              return op_move_object(cmd);
        case MtpOp_CopyObject:              return op_copy_object(cmd);
        case MtpOp_GetDevicePropDesc:       return op_get_device_prop_desc(cmd);
        case MtpOp_GetDevicePropValue:      return op_get_device_prop_value(cmd);

        default:
            LOG_D("mtp: unsupported op 0x%04x", cmd->code);
            return mtpSendResponse(MtpRes_OperationNotSupported, cmd->transaction_id, NULL, 0);
    }
}
