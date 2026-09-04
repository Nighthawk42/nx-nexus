// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP object handle table.
//
// MTP addresses everything by an opaque u32 "object handle" that must stay
// stable for the lifetime of a session. This maps handles to (storage, path)
// pairs and back. Handles are allocated monotonically from 1; 0 and
// 0xFFFFFFFF are reserved by the protocol.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_PATH_MAX 0x301  // FS_MAX_PATH in libnx

typedef struct {
    u32  handle;
    u32  storage_id;
    u32  parent;                  // MTP_HANDLE_ROOT when at a store root
    bool is_dir;
    bool valid;
    s64  size;                    // 0 for directories
    char path[NEXUS_PATH_MAX];    // storage-relative, always starts with '/'
} NexusObject;

/// Allocates the table. Call once at startup.
Result nexusObjectDbInit(void);

/// Frees the table and invalidates every handle.
void nexusObjectDbExit(void);

/// Drops every entry. Used when a session closes or a store is remounted.
void nexusObjectDbClear(void);

/// Drops every entry belonging to one storage.
void nexusObjectDbClearStorage(u32 storage_id);

/// Interns a (storage, path) pair and returns its handle. If the pair is
/// already known, the existing handle is returned so handles stay stable
/// across repeated directory listings.
/// Returns 0 on allocation failure.
u32 nexusObjectDbIntern(u32 storage_id, u32 parent, const char *path, bool is_dir, s64 size);

/// Looks up a handle. Returns NULL for unknown or invalidated handles.
/// The pointer is owned by the table and is invalidated by any Intern call
/// that grows the table -- copy what you need before interning again.
const NexusObject *nexusObjectDbGet(u32 handle);

/// Copies an entry out, which is safe across subsequent Intern calls.
bool nexusObjectDbGetCopy(u32 handle, NexusObject *out);

/// Marks a handle invalid (after DeleteObject).
void nexusObjectDbInvalidate(u32 handle);

/// Repoints a handle at a new path after a rename or move, keeping the handle
/// itself stable. Hosts cache handles across a rename and expect them to keep
/// working. Returns false for an unknown handle or an over-long path.
bool nexusObjectDbRename(u32 handle, const char *new_path);

/// Finds the handle for a (storage, path) pair, or 0 if not interned.
u32 nexusObjectDbFind(u32 storage_id, const char *path);

/// Number of live entries, for diagnostics.
size_t nexusObjectDbCount(void);
