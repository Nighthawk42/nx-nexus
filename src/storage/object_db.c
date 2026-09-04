// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP object handle table.
//
// A flat growable array indexed by (handle - 1). Handles are never reused
// within a session, so a host that caches a handle across a delete gets a
// clean InvalidObjectHandle rather than a stale hit on a different file.
//
// Lookup by path is linear. That is fine for browsing an SD card a directory
// at a time; if a host ever enumerates tens of thousands of objects this is
// the first thing to replace with a hash index.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/object_db.h"
#include "nexus/log.h"

#define OBJDB_INITIAL_CAPACITY 256

static NexusObject *g_entries  = NULL;
static size_t       g_capacity = 0;
static size_t       g_used     = 0;   // highest allocated slot; handle == index + 1
static size_t       g_live     = 0;   // valid entries
static Mutex        g_mutex;

Result nexusObjectDbInit(void)
{
    mutexInit(&g_mutex);

    g_entries = (NexusObject *)calloc(OBJDB_INITIAL_CAPACITY, sizeof(NexusObject));
    if (g_entries == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    g_capacity = OBJDB_INITIAL_CAPACITY;
    g_used     = 0;
    g_live     = 0;
    return 0;
}

void nexusObjectDbExit(void)
{
    mutexLock(&g_mutex);
    free(g_entries);
    g_entries  = NULL;
    g_capacity = 0;
    g_used     = 0;
    g_live     = 0;
    mutexUnlock(&g_mutex);
}

void nexusObjectDbClear(void)
{
    mutexLock(&g_mutex);
    if (g_entries != NULL) memset(g_entries, 0, g_capacity * sizeof(NexusObject));
    g_used = 0;
    g_live = 0;
    mutexUnlock(&g_mutex);
}

void nexusObjectDbClearStorage(u32 storage_id)
{
    mutexLock(&g_mutex);
    for (size_t i = 0; i < g_used; i++) {
        if (g_entries[i].valid && g_entries[i].storage_id == storage_id) {
            g_entries[i].valid = false;
            g_live--;
        }
    }
    mutexUnlock(&g_mutex);
}

// Caller must hold the mutex.
static bool objdb_grow(void)
{
    const size_t next = g_capacity * 2;
    NexusObject *grown = (NexusObject *)realloc(g_entries, next * sizeof(NexusObject));
    if (grown == NULL) return false;

    memset(grown + g_capacity, 0, (next - g_capacity) * sizeof(NexusObject));
    g_entries  = grown;
    g_capacity = next;
    return true;
}

// Caller must hold the mutex.
static u32 objdb_find_locked(u32 storage_id, const char *path)
{
    for (size_t i = 0; i < g_used; i++) {
        const NexusObject *e = &g_entries[i];
        if (e->valid && e->storage_id == storage_id && strcmp(e->path, path) == 0) {
            return e->handle;
        }
    }
    return 0;
}

u32 nexusObjectDbIntern(u32 storage_id, u32 parent, const char *path, bool is_dir, s64 size)
{
    if (path == NULL || path[0] == '\0') return 0;
    if (strlen(path) >= NEXUS_PATH_MAX)  return 0;

    mutexLock(&g_mutex);

    u32 existing = objdb_find_locked(storage_id, path);
    if (existing != 0) {
        // Refresh the cached metadata; the file may have changed on disk.
        NexusObject *e = &g_entries[existing - 1];
        e->is_dir = is_dir;
        e->size   = is_dir ? 0 : size;
        e->parent = parent;
        mutexUnlock(&g_mutex);
        return existing;
    }

    if (g_used == g_capacity && !objdb_grow()) {
        mutexUnlock(&g_mutex);
        LOG_E("objdb: out of memory interning %s", path);
        return 0;
    }

    NexusObject *e = &g_entries[g_used];
    e->handle     = (u32)(g_used + 1);
    e->storage_id = storage_id;
    e->parent     = parent;
    e->is_dir     = is_dir;
    e->valid      = true;
    e->size       = is_dir ? 0 : size;
    snprintf(e->path, sizeof(e->path), "%s", path);

    g_used++;
    g_live++;

    const u32 handle = e->handle;
    mutexUnlock(&g_mutex);
    return handle;
}

const NexusObject *nexusObjectDbGet(u32 handle)
{
    if (handle == 0 || handle > g_used) return NULL;

    const NexusObject *e = &g_entries[handle - 1];
    return e->valid ? e : NULL;
}

bool nexusObjectDbGetCopy(u32 handle, NexusObject *out)
{
    if (out == NULL) return false;

    mutexLock(&g_mutex);
    bool ok = (handle != 0 && handle <= g_used && g_entries[handle - 1].valid);
    if (ok) memcpy(out, &g_entries[handle - 1], sizeof(*out));
    mutexUnlock(&g_mutex);
    return ok;
}

void nexusObjectDbInvalidate(u32 handle)
{
    mutexLock(&g_mutex);
    if (handle != 0 && handle <= g_used && g_entries[handle - 1].valid) {
        g_entries[handle - 1].valid = false;
        g_live--;
    }
    mutexUnlock(&g_mutex);
}

bool nexusObjectDbRename(u32 handle, const char *new_path)
{
    if (new_path == NULL || strlen(new_path) >= NEXUS_PATH_MAX) return false;

    mutexLock(&g_mutex);

    bool ok = (handle != 0 && handle <= g_used && g_entries[handle - 1].valid);
    if (ok) {
        NexusObject *e = &g_entries[handle - 1];

        // Children of a renamed directory keep stale paths. Rather than
        // rewriting the whole subtree, invalidate the descendants: the host
        // re-enumerates after a move, and a stale child handle would silently
        // point at a path that no longer exists.
        if (e->is_dir) {
            const size_t old_len = strlen(e->path);
            for (size_t i = 0; i < g_used; i++) {
                NexusObject *c = &g_entries[i];
                if (!c->valid || c == e) continue;
                if (c->storage_id != e->storage_id) continue;
                if (strncmp(c->path, e->path, old_len) == 0 && c->path[old_len] == '/') {
                    c->valid = false;
                    g_live--;
                }
            }
        }

        snprintf(e->path, sizeof(e->path), "%s", new_path);
    }

    mutexUnlock(&g_mutex);
    return ok;
}

u32 nexusObjectDbFind(u32 storage_id, const char *path)
{
    if (path == NULL) return 0;

    mutexLock(&g_mutex);
    u32 handle = objdb_find_locked(storage_id, path);
    mutexUnlock(&g_mutex);
    return handle;
}

size_t nexusObjectDbCount(void)
{
    mutexLock(&g_mutex);
    size_t n = g_live;
    mutexUnlock(&g_mutex);
    return n;
}
