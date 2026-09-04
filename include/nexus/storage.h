// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- virtual storage backend interface.
//
// Every MTP store the console advertises is one NexusStorage. Phase 1 ships
// only the passthrough SD card backend; the streaming installer, gamecard and
// save backends plug in here without touching the MTP layer.
#pragma once

#include <switch.h>
#include <stdbool.h>
#include "nexus/object_db.h"

// Storage IDs. MTP splits these into a 16-bit physical and 16-bit logical id;
// we use one logical partition per physical store.
#define NEXUS_STORAGE_SDMC          0x00010001u  // read/write passthrough to sdmc:/
#define NEXUS_STORAGE_INSTALL_SD    0x00010002u  // write-only drop target -> SD install
#define NEXUS_STORAGE_INSTALL_NAND  0x00010003u  // write-only drop target -> NAND install
#define NEXUS_STORAGE_GAMECARD      0x00010004u  // read-only virtual cartridge dump
#define NEXUS_STORAGE_SAVES         0x00010005u  // virtual save backup/restore
#define NEXUS_STORAGE_TITLES        0x00010006u  // installed titles: browse, extract, delete
#define NEXUS_STORAGE_BIS           0x00010007u  // raw NAND partitions of the active MMC
#define NEXUS_STORAGE_ALBUM         0x00010008u  // screenshots and capture videos

#define NEXUS_MAX_STORAGES 12

typedef struct NexusStorage NexusStorage;

typedef struct {
    u64 capacity_bytes;       // 0xFFFFFFFFFFFFFFFF if not applicable
    u64 free_bytes;
    u32 free_objects;         // 0xFFFFFFFF if unknown
    u16 storage_type;         // MtpStorageType_*
    u16 filesystem_type;      // MtpFsType_*
    u16 access_capability;    // MtpAccess_*
} NexusStorageInfo;

/// Callback invoked once per child during an enumerate() call.
/// Return false to stop enumeration early.
typedef bool (*NexusEnumCallback)(void *user, const char *name, bool is_dir, s64 size);

typedef struct {
    /// Human-readable store name shown in the host file explorer.
    const char *(*description)(NexusStorage *self);

    /// Capacity / access reporting for GetStorageInfo.
    Result (*get_info)(NexusStorage *self, NexusStorageInfo *out);

    /// Lists the children of a storage-relative directory path ("/" for root).
    Result (*enumerate)(NexusStorage *self, const char *dir_path,
                        NexusEnumCallback cb, void *user);

    /// Stats one storage-relative path. Sets *out_is_dir and *out_size.
    Result (*stat)(NexusStorage *self, const char *path, bool *out_is_dir, s64 *out_size);

    /// Reads from a file. Backends that cannot seek arbitrarily (a streamed
    /// gamecard dump) may return an error for non-sequential offsets.
    Result (*read)(NexusStorage *self, const char *path, u64 offset,
                   void *buffer, size_t size, size_t *out_read);

    // --- write path; may be NULL on read-only stores ---

    /// Called on SendObjectInfo. Opens a write transaction for path with the
    /// declared total size (0xFFFFFFFF when the host does not know it).
    Result (*write_begin)(NexusStorage *self, const char *path, u64 declared_size);

    /// Called repeatedly as SendObject data arrives, in order from offset 0.
    Result (*write_chunk)(NexusStorage *self, const void *buffer, size_t size);

    /// Called once the declared byte count has arrived, or on host cancel with
    /// committed == false. Must release all transaction state either way.
    Result (*write_end)(NexusStorage *self, bool committed);

    /// Creates a directory. NULL when unsupported.
    Result (*mkdir)(NexusStorage *self, const char *path);

    /// Deletes a file or empty directory. NULL when unsupported.
    Result (*remove)(NexusStorage *self, const char *path, bool is_dir);

    /// Renames or moves within the same store. Backs both MTP's MoveObject and
    /// the rename that hosts perform via SetObjectPropValue(ObjectFileName).
    /// NULL when unsupported.
    Result (*move)(NexusStorage *self, const char *from, const char *to, bool is_dir);

    /// Copies a file within the same store. NULL when unsupported, in which
    /// case the MTP layer reports the operation as unsupported rather than
    /// emulating it with a round trip through the host.
    Result (*copy)(NexusStorage *self, const char *from, const char *to);

    /// Fills buffer with a JPEG thumbnail for path, if the store has one.
    /// This is how installed titles get real box art in the host's file
    /// browser without NX-Nexus needing a graphical UI of its own.
    /// NULL, or a failure return, means "no thumbnail" and is not an error.
    Result (*thumbnail)(NexusStorage *self, const char *path,
                        void *buffer, size_t cap, size_t *out_len);
} NexusStorageOps;

/// Switch title icons are always 256x256 JPEG.
#define NEXUS_THUMB_DIM       256
#define NEXUS_THUMB_MAX_BYTES (256u * 1024u)

struct NexusStorage {
    u32 storage_id;
    const NexusStorageOps *ops;
    void *impl;               // backend private state
    bool  present;            // false hides the store from GetStorageIDs
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

/// Initialises the registry and mounts the backends that are available.
Result nexusStorageRegistryInit(void);

void nexusStorageRegistryExit(void);

/// Re-checks removable media (SD, gamecard) and updates present flags.
/// Cheap enough to call on every GetStorageIDs.
void nexusStorageRegistryRefresh(void);

/// Called once when an MTP session opens. Does the expensive invalidation
/// that must not run on every operation.
void nexusStorageRegistryOnSessionOpen(void);

/// Number of registered stores, including absent ones.
size_t nexusStorageCount(void);

/// Store by index, or NULL when out of range.
NexusStorage *nexusStorageAt(size_t index);

/// Store by MTP storage id, or NULL when unknown.
NexusStorage *nexusStorageById(u32 storage_id);

/// True when the store exists and its media is present.
bool nexusStorageIsPresent(u32 storage_id);

// ---------------------------------------------------------------------------
// Backend constructors (implemented per backend translation unit)
// ---------------------------------------------------------------------------

/// Passthrough backend over a libnx devoptab mount (e.g. "sdmc:").
Result nexusStorageSdmcCreate(NexusStorage *out, u32 storage_id,
                              const char *mount, const char *description);

/// Screenshots and capture videos. Mounts the album image directory under its
/// own devoptab name and then reuses the passthrough backend, which is why
/// deleting and copying come for free.
Result nexusStorageAlbumCreate(NexusStorage *out, u32 storage_id,
                               const char *description);

void nexusStorageAlbumDestroy(void);

/// Write-only drop target that streams dropped NSPs through the installer.
/// target is a NexusInstallTarget (NcmStorageId) value.
Result nexusStorageInstallCreate(NexusStorage *out, u32 storage_id, u8 target,
                                 const char *description);

/// Read-only view of an inserted game card, one folder per partition.
Result nexusStorageGamecardCreate(NexusStorage *out, u32 storage_id,
                                  const char *description);

/// Re-checks whether a card is present and drops any stale mount.
void nexusStorageGamecardRefresh(NexusStorage *self);

/// Read-only view of installed game saves, one folder per save.
Result nexusStorageSavesCreate(NexusStorage *out, u32 storage_id,
                               const char *description);

/// Drops the cached save listing so the next access re-enumerates. Called when
/// a new MTP session opens, so a save created since launch shows up.
void nexusStorageSavesInvalidate(NexusStorage *self);

/// Installed titles: browse, extract as NSP, delete.
Result nexusStorageTitlesCreate(NexusStorage *out, u32 storage_id,
                                const char *description);

/// Drops the cached title listing.
void nexusStorageTitlesInvalidate(NexusStorage *self);

/// Raw BIS partitions of whichever NAND the system booted from.
Result nexusStorageBisCreate(NexusStorage *out, u32 storage_id);
