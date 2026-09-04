// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP / PIMA 15740 protocol constants.
//
// These are plain protocol numbers transcribed from the public PIMA 15740:2000
// (PTP) specification and the Microsoft MTP extension. See docs/REFERENCES.md.
#pragma once

#include <switch.h>

// ---------------------------------------------------------------------------
// Container ("block") header -- every MTP packet starts with these 12 bytes.
// ---------------------------------------------------------------------------
#define MTP_CONTAINER_HEADER_SIZE 12u

typedef enum {
    MtpContainerType_Undefined = 0,
    MtpContainerType_Command   = 1,
    MtpContainerType_Data      = 2,
    MtpContainerType_Response  = 3,
    MtpContainerType_Event     = 4,
} MtpContainerType;

// A command/response block carries at most 5 u32 parameters.
#define MTP_MAX_PARAMS 5u

// ---------------------------------------------------------------------------
// Operation codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpOp_GetDeviceInfo            = 0x1001,
    MtpOp_OpenSession              = 0x1002,
    MtpOp_CloseSession             = 0x1003,
    MtpOp_GetStorageIDs            = 0x1004,
    MtpOp_GetStorageInfo           = 0x1005,
    MtpOp_GetNumObjects            = 0x1006,
    MtpOp_GetObjectHandles         = 0x1007,
    MtpOp_GetObjectInfo            = 0x1008,
    MtpOp_GetObject                = 0x1009,
    MtpOp_GetThumb                 = 0x100A,
    MtpOp_DeleteObject             = 0x100B,
    MtpOp_SendObjectInfo           = 0x100C,
    MtpOp_SendObject               = 0x100D,
    MtpOp_FormatStore              = 0x100F,
    MtpOp_GetDevicePropDesc        = 0x1014,
    MtpOp_GetDevicePropValue       = 0x1015,
    MtpOp_SetDevicePropValue       = 0x1016,
    MtpOp_ResetDevicePropValue     = 0x1017,
    MtpOp_MoveObject               = 0x1019,
    MtpOp_CopyObject               = 0x101A,
    MtpOp_GetPartialObject         = 0x101B,

    // MTP (Microsoft) extensions
    MtpOp_GetObjectPropsSupported  = 0x9801,
    MtpOp_GetObjectPropDesc        = 0x9802,
    MtpOp_GetObjectPropValue       = 0x9803,
    MtpOp_SetObjectPropValue       = 0x9804,
    MtpOp_GetObjectPropList        = 0x9805,
    MtpOp_GetObjectReferences      = 0x9810,
    MtpOp_SetObjectReferences      = 0x9811,
    MtpOp_GetPartialObject64       = 0x95C1,
    MtpOp_SendPartialObject        = 0x95C2,
    MtpOp_TruncateObject           = 0x95C3,
    MtpOp_BeginEditObject          = 0x95C4,
    MtpOp_EndEditObject            = 0x95C5,
} MtpOperationCode;

// ---------------------------------------------------------------------------
// Response codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpRes_Undefined                    = 0x2000,
    MtpRes_OK                           = 0x2001,
    MtpRes_GeneralError                 = 0x2002,
    MtpRes_SessionNotOpen               = 0x2003,
    MtpRes_InvalidTransactionID         = 0x2004,
    MtpRes_OperationNotSupported        = 0x2005,
    MtpRes_ParameterNotSupported        = 0x2006,
    MtpRes_IncompleteTransfer           = 0x2007,
    MtpRes_InvalidStorageID             = 0x2008,
    MtpRes_InvalidObjectHandle          = 0x2009,
    MtpRes_DevicePropNotSupported       = 0x200A,
    MtpRes_InvalidObjectFormatCode      = 0x200B,
    MtpRes_StoreFull                    = 0x200C,
    MtpRes_ObjectWriteProtected         = 0x200D,
    MtpRes_StoreReadOnly                = 0x200E,
    MtpRes_AccessDenied                 = 0x200F,
    MtpRes_NoThumbnailPresent           = 0x2010,
    MtpRes_StoreNotAvailable            = 0x2013,
    MtpRes_SpecByFormatUnsupported      = 0x2014,
    MtpRes_NoValidObjectInfo            = 0x2015,
    MtpRes_DeviceBusy                   = 0x2019,
    MtpRes_InvalidParentObject          = 0x201A,
    MtpRes_InvalidDevicePropFormat      = 0x201B,
    MtpRes_InvalidDevicePropValue       = 0x201C,
    MtpRes_InvalidParameter             = 0x201D,
    MtpRes_SessionAlreadyOpen           = 0x201E,
    MtpRes_TransactionCancelled         = 0x201F,
    MtpRes_SpecOfDestinationUnsupported = 0x2020,

    // MTP extensions
    MtpRes_InvalidObjectPropCode        = 0xA801,
    MtpRes_InvalidObjectPropFormat      = 0xA802,
    MtpRes_InvalidObjectPropValue       = 0xA803,
    MtpRes_InvalidObjectReference       = 0xA804,
    MtpRes_GroupNotSupported            = 0xA805,
    MtpRes_InvalidDataset               = 0xA806,
    MtpRes_ObjectPropNotSupported       = 0xA80A,
} MtpResponseCode;

// ---------------------------------------------------------------------------
// Event codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpEvent_CancelTransaction  = 0x4001,
    MtpEvent_ObjectAdded        = 0x4002,
    MtpEvent_ObjectRemoved      = 0x4003,
    MtpEvent_StoreAdded         = 0x4004,
    MtpEvent_StoreRemoved       = 0x4005,
    MtpEvent_DevicePropChanged  = 0x4006,
    MtpEvent_ObjectInfoChanged  = 0x4007,
    MtpEvent_StoreFull          = 0x400A,
    MtpEvent_StorageInfoChanged = 0x400C,
} MtpEventCode;

// ---------------------------------------------------------------------------
// Storage descriptors
// ---------------------------------------------------------------------------
typedef enum {
    MtpStorageType_Undefined    = 0x0000,
    MtpStorageType_FixedROM     = 0x0001,
    MtpStorageType_RemovableROM = 0x0002,
    MtpStorageType_FixedRAM     = 0x0003,
    MtpStorageType_RemovableRAM = 0x0004,
} MtpStorageType;

typedef enum {
    MtpFsType_Undefined           = 0x0000,
    MtpFsType_GenericFlat         = 0x0001,
    MtpFsType_GenericHierarchical = 0x0002,
    MtpFsType_DCF                 = 0x0003,
} MtpFilesystemType;

typedef enum {
    MtpAccess_ReadWrite                = 0x0000,
    MtpAccess_ReadOnly                 = 0x0001,
    MtpAccess_ReadOnlyWithObjectDelete = 0x0002,
} MtpAccessCapability;

// ---------------------------------------------------------------------------
// Object format codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpFormat_Undefined   = 0x3000,
    MtpFormat_Association = 0x3001,  // directory
    MtpFormat_Text        = 0x3004,
    MtpFormat_HTML        = 0x3005,
    MtpFormat_WAV         = 0x3008,
    MtpFormat_MP3         = 0x3009,
    MtpFormat_EXIF_JPEG   = 0x3801,
} MtpObjectFormat;

typedef enum {
    MtpProtection_None            = 0x0000,
    MtpProtection_ReadOnly        = 0x0001,
    MtpProtection_ReadOnlyData    = 0x8002,
    MtpProtection_NonTransferable = 0x8003,
} MtpProtectionStatus;

// ---------------------------------------------------------------------------
// Dataset value types (used by property descriptors)
// ---------------------------------------------------------------------------
typedef enum {
    MtpType_Undefined = 0x0000,
    MtpType_INT8      = 0x0001,
    MtpType_UINT8     = 0x0002,
    MtpType_INT16     = 0x0003,
    MtpType_UINT16    = 0x0004,
    MtpType_INT32     = 0x0005,
    MtpType_UINT32    = 0x0006,
    MtpType_INT64     = 0x0007,
    MtpType_UINT64    = 0x0008,
    MtpType_INT128    = 0x0009,
    MtpType_UINT128   = 0x000A,
    MtpType_AUINT8    = 0x4002,
    MtpType_AUINT16   = 0x4004,
    MtpType_AUINT32   = 0x4006,
    MtpType_STR       = 0xFFFF,
} MtpDataType;

// ---------------------------------------------------------------------------
// Object property codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpObjProp_StorageID        = 0xDC01,
    MtpObjProp_ObjectFormat     = 0xDC02,
    MtpObjProp_ProtectionStatus = 0xDC03,
    MtpObjProp_ObjectSize       = 0xDC04,
    MtpObjProp_ObjectFileName   = 0xDC07,
    MtpObjProp_DateCreated      = 0xDC08,
    MtpObjProp_DateModified     = 0xDC09,
    MtpObjProp_ParentObject     = 0xDC0B,
    MtpObjProp_PersistentUID    = 0xDC41,
    MtpObjProp_Name             = 0xDC44,
} MtpObjectPropCode;

// ---------------------------------------------------------------------------
// Device property codes
// ---------------------------------------------------------------------------
typedef enum {
    MtpDevProp_BatteryLevel           = 0x5001,
    MtpDevProp_SynchronizationPartner = 0xD401,
    MtpDevProp_DeviceFriendlyName     = 0xD402,
} MtpDevicePropCode;

// ---------------------------------------------------------------------------
// Well-known handle / wildcard values
// ---------------------------------------------------------------------------
#define MTP_HANDLE_ROOT   0xFFFFFFFFu  // "root of store" when used as a parent
#define MTP_HANDLE_ALL    0xFFFFFFFFu  // wildcard in GetObjectHandles
#define MTP_STORAGE_ALL   0xFFFFFFFFu  // wildcard in GetObjectHandles
#define MTP_FORMAT_ALL    0x00000000u  // wildcard format filter
