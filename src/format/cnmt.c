// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- CNMT (PackagedContentMeta) parsing.

#include <stdio.h>
#include <string.h>

#include "nexus/cnmt.h"

// PackagedContentMetaHeader field offsets.
#define HDR_TITLE_ID          0x00
#define HDR_VERSION           0x08
#define HDR_META_TYPE         0x0C
#define HDR_META_PLATFORM     0x0D
#define HDR_EXT_HEADER_SIZE   0x0E
#define HDR_CONTENT_COUNT     0x10
#define HDR_META_COUNT        0x12
#define HDR_META_ATTRIBUTES   0x14
#define HDR_REQ_DL_SYS_VER    0x18

// PackagedContentInfo field offsets.
#define CI_HASH               0x00
#define CI_CONTENT_ID         0x20
#define CI_SIZE               0x30  // 40-bit
#define CI_CONTENT_ATTRIBUTES 0x35  // [15.0.0+]; part of Size before that
#define CI_CONTENT_TYPE       0x36
#define CI_ID_OFFSET          0x37

NexusFmtResult cnmtInit(CnmtContext *ctx, const void *data, size_t len)
{
    if (ctx == NULL || data == NULL) return NexusFmt_Truncated;
    if (len < CNMT_HEADER_SIZE)      return NexusFmt_Truncated;

    memset(ctx, 0, sizeof(*ctx));

    const u16 ext_header_size = nexusRdU16(data, HDR_EXT_HEADER_SIZE);
    const u16 content_count   = nexusRdU16(data, HDR_CONTENT_COUNT);
    const u16 meta_count      = nexusRdU16(data, HDR_META_COUNT);

    if (content_count > CNMT_MAX_CONTENTS) return NexusFmt_TooLarge;

    // The content info array sits after the header and the type-specific
    // extended header. Every size here is 16-bit so the arithmetic cannot
    // overflow 64 bits, but it can still exceed the buffer.
    const u64 content_info_off = (u64)CNMT_HEADER_SIZE + ext_header_size;
    const u64 content_bytes    = (u64)content_count * CNMT_CONTENT_INFO_SIZE;
    const u64 meta_bytes       = (u64)meta_count * CNMT_META_INFO_SIZE;

    u64 required = content_info_off;
    if (nexusAddOverflows(required, content_bytes, &required)) return NexusFmt_Overflow;
    if (nexusAddOverflows(required, meta_bytes, &required))    return NexusFmt_Overflow;

    if (len < required) return NexusFmt_Truncated;

    ctx->data                             = (const u8 *)data;
    ctx->len                              = len;
    ctx->title_id                         = nexusRdU64(data, HDR_TITLE_ID);
    ctx->version                          = nexusRdU32(data, HDR_VERSION);
    ctx->meta_type                        = nexusRdU8(data, HDR_META_TYPE);
    ctx->meta_platform                    = nexusRdU8(data, HDR_META_PLATFORM);
    ctx->extended_header_size             = ext_header_size;
    ctx->content_count                    = content_count;
    ctx->content_meta_count               = meta_count;
    ctx->content_meta_attributes          = nexusRdU8(data, HDR_META_ATTRIBUTES);
    ctx->required_download_system_version  = nexusRdU32(data, HDR_REQ_DL_SYS_VER);
    ctx->content_info_off                 = (size_t)content_info_off;

    return NexusFmt_Ok;
}

u16 cnmtGetContentCount(const CnmtContext *ctx)
{
    return (ctx != NULL) ? ctx->content_count : 0;
}

NexusFmtResult cnmtGetContentInfo(const CnmtContext *ctx, u16 index, CnmtContentInfo *out)
{
    if (ctx == NULL || out == NULL)   return NexusFmt_Truncated;
    if (index >= ctx->content_count)  return NexusFmt_OutOfRange;

    const size_t base = ctx->content_info_off + ((size_t)index * CNMT_CONTENT_INFO_SIZE);

    memcpy(out->hash,       ctx->data + base + CI_HASH,       CNMT_HASH_SIZE);
    memcpy(out->content_id, ctx->data + base + CI_CONTENT_ID, CNMT_CONTENT_ID_SIZE);

    // Size is a 40-bit field. Firmware before 15.0.0 defined it as 48 bits with
    // no attributes byte, but no content approaches 2^40 bytes (1 TiB), so the
    // upper byte is always zero and reading 40 bits is correct for both.
    out->size               = nexusRdU40(ctx->data, base + CI_SIZE);
    out->content_attributes = nexusRdU8(ctx->data, base + CI_CONTENT_ATTRIBUTES);
    out->content_type       = nexusRdU8(ctx->data, base + CI_CONTENT_TYPE);
    out->id_offset          = nexusRdU8(ctx->data, base + CI_ID_OFFSET);

    return NexusFmt_Ok;
}

NexusFmtResult cnmtFindContentByType(const CnmtContext *ctx, u8 content_type,
                                     CnmtContentInfo *out)
{
    if (ctx == NULL || out == NULL) return NexusFmt_Truncated;

    for (u16 i = 0; i < ctx->content_count; i++) {
        CnmtContentInfo info;
        if (cnmtGetContentInfo(ctx, i, &info) != NexusFmt_Ok) continue;
        if (info.content_type == content_type) {
            *out = info;
            return NexusFmt_Ok;
        }
    }
    return NexusFmt_NotFound;
}

void cnmtFormatContentId(const u8 id[CNMT_CONTENT_ID_SIZE], char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    if (out == NULL || out_size == 0) return;

    // Each byte needs two characters plus the terminator.
    const size_t needed = (CNMT_CONTENT_ID_SIZE * 2) + 1;
    if (out_size < needed) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0; i < CNMT_CONTENT_ID_SIZE; i++) {
        out[i * 2]       = hex[(id[i] >> 4) & 0xF];
        out[(i * 2) + 1] = hex[id[i] & 0xF];
    }
    out[CNMT_CONTENT_ID_SIZE * 2] = '\0';
}

NexusFmtResult cnmtGetTotalContentSize(const CnmtContext *ctx, u64 *out_total)
{
    if (ctx == NULL || out_total == NULL) return NexusFmt_Truncated;

    u64 total = 0;
    for (u16 i = 0; i < ctx->content_count; i++) {
        CnmtContentInfo info;
        NexusFmtResult r = cnmtGetContentInfo(ctx, i, &info);
        if (r != NexusFmt_Ok) return r;
        if (nexusAddOverflows(total, info.size, &total)) return NexusFmt_Overflow;
    }

    *out_total = total;
    return NexusFmt_Ok;
}

const char *cnmtMetaTypeStr(u8 meta_type)
{
    switch (meta_type) {
        case CnmtMetaType_SystemProgram:        return "SystemProgram";
        case CnmtMetaType_SystemData:           return "SystemData";
        case CnmtMetaType_SystemUpdate:         return "SystemUpdate";
        case CnmtMetaType_BootImagePackage:     return "BootImagePackage";
        case CnmtMetaType_BootImagePackageSafe: return "BootImagePackageSafe";
        case CnmtMetaType_Application:          return "Application";
        case CnmtMetaType_Patch:                return "Patch";
        case CnmtMetaType_AddOnContent:         return "AddOnContent";
        case CnmtMetaType_Delta:                return "Delta";
        case CnmtMetaType_DataPatch:            return "DataPatch";
        default:                                return "Unknown";
    }
}

const char *cnmtContentTypeStr(u8 content_type)
{
    switch (content_type) {
        case CnmtContentType_Meta:             return "Meta";
        case CnmtContentType_Program:          return "Program";
        case CnmtContentType_Data:             return "Data";
        case CnmtContentType_Control:          return "Control";
        case CnmtContentType_HtmlDocument:     return "HtmlDocument";
        case CnmtContentType_LegalInformation: return "LegalInformation";
        case CnmtContentType_DeltaFragment:    return "DeltaFragment";
        default:                               return "Unknown";
    }
}
