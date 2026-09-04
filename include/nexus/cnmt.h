// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- CNMT (PackagedContentMeta) parsing.
//
// The CNMT is the manifest that says which NCAs make up a title, what each one
// is, and how big it is. It lives inside the Meta NCA, which is encrypted --
// but Horizon will decrypt it for us: after the meta NCA has been written to a
// placeholder and registered, ncmContentStorageGetPath plus
// fsOpenFileSystemWithId(FsFileSystemType_ContentMeta) yields the plaintext
// .cnmt. That is why NX-Nexus needs no keys of its own.
//
// Structure (nn::ncm::PackagedContentMeta):
//
//   0x00  PackagedContentMetaHeader   0x20 bytes
//   0x20  extended header             ExtendedHeaderSize bytes, type-dependent
//   ...   PackagedContentInfo[]       ContentCount * 0x38
//   ...   ContentMetaInfo[]           ContentMetaCount * 0x10
//
// Verified against https://switchbrew.org/wiki/CNMT.
#pragma once

#include "nexus/format.h"

#define CNMT_HEADER_SIZE        0x20u
#define CNMT_CONTENT_INFO_SIZE  0x38u
#define CNMT_META_INFO_SIZE     0x10u
#define CNMT_CONTENT_ID_SIZE    0x10u
#define CNMT_HASH_SIZE          0x20u

// A title with more than a few hundred contents does not exist in practice;
// this bound keeps a malformed header from driving a huge loop.
#define CNMT_MAX_CONTENTS 0x1000u

// nn::ncm::ContentMetaType
typedef enum {
    CnmtMetaType_Unknown              = 0x00,
    CnmtMetaType_SystemProgram        = 0x01,
    CnmtMetaType_SystemData           = 0x02,
    CnmtMetaType_SystemUpdate         = 0x03,
    CnmtMetaType_BootImagePackage     = 0x04,
    CnmtMetaType_BootImagePackageSafe = 0x05,
    CnmtMetaType_Application          = 0x80,
    CnmtMetaType_Patch                = 0x81,
    CnmtMetaType_AddOnContent         = 0x82,
    CnmtMetaType_Delta                = 0x83,
    CnmtMetaType_DataPatch            = 0x84,
} CnmtMetaType;

// nn::ncm::ContentType
typedef enum {
    CnmtContentType_Meta             = 0,
    CnmtContentType_Program          = 1,
    CnmtContentType_Data             = 2,
    CnmtContentType_Control          = 3,
    CnmtContentType_HtmlDocument     = 4,
    CnmtContentType_LegalInformation = 5,
    CnmtContentType_DeltaFragment    = 6,
} CnmtContentType;

typedef struct {
    u8  hash[CNMT_HASH_SIZE];             // SHA-256 of the referenced NCA
    u8  content_id[CNMT_CONTENT_ID_SIZE]; // also the NCA's filename stem
    u64 size;                             // 40-bit field on the wire
    u8  content_attributes;               // [15.0.0+]; reserved before that
    u8  content_type;                     // CnmtContentType
    u8  id_offset;
} CnmtContentInfo;

typedef struct {
    const u8 *data;
    size_t    len;

    u64 title_id;
    u32 version;
    u8  meta_type;          // CnmtMetaType
    u8  meta_platform;      // [17.0.0+]; reserved before that
    u16 extended_header_size;
    u16 content_count;
    u16 content_meta_count;
    u8  content_meta_attributes;
    u32 required_download_system_version;

    size_t content_info_off;   // start of the PackagedContentInfo array
} CnmtContext;

/// Parses a complete .cnmt blob. The blob is borrowed, not copied.
NexusFmtResult cnmtInit(CnmtContext *ctx, const void *data, size_t len);

u16 cnmtGetContentCount(const CnmtContext *ctx);

NexusFmtResult cnmtGetContentInfo(const CnmtContext *ctx, u16 index, CnmtContentInfo *out);

/// Finds the first content of a given type. Most titles have exactly one
/// Program and one Control; NexusFmt_NotFound if absent.
NexusFmtResult cnmtFindContentByType(const CnmtContext *ctx, u8 content_type,
                                     CnmtContentInfo *out);

/// Formats a content id as the 32-character lowercase hex string that is the
/// NCA's filename stem inside the NSP. out must hold at least 33 bytes.
void cnmtFormatContentId(const u8 id[CNMT_CONTENT_ID_SIZE], char *out, size_t out_size);

/// Sums the sizes of every content entry, which is the installed footprint of
/// the title. Returns NexusFmt_Overflow rather than wrapping.
NexusFmtResult cnmtGetTotalContentSize(const CnmtContext *ctx, u64 *out_total);

const char *cnmtMetaTypeStr(u8 meta_type);
const char *cnmtContentTypeStr(u8 content_type);
