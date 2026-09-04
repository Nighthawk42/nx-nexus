// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- storage registry.
//
// Phase 1 registers only the SD card passthrough store. The install, gamecard
// and save backends are registered here as they land; the MTP layer needs no
// changes when they do.

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nexus/storage.h"
#include "nexus/installer.h"
#include "nexus/log.h"

static NexusStorage g_storages[NEXUS_MAX_STORAGES];
static size_t       g_count       = 0;
static bool         g_initialized = false;

Result nexusStorageRegistryInit(void)
{
    if (g_initialized) return 0;

    memset(g_storages, 0, sizeof(g_storages));
    g_count = 0;

    // --- Store 1: SD card passthrough ---
    Result rc = nexusStorageSdmcCreate(&g_storages[g_count], NEXUS_STORAGE_SDMC,
                                       "sdmc:", "1: MicroSD");
    if (R_FAILED(rc)) {
        LOG_E("storage: failed to create SD store (0x%x)", rc);
        return rc;
    }
    g_count++;

    // --- Store 2: install to SD ---
    rc = nexusStorageInstallCreate(&g_storages[g_count], NEXUS_STORAGE_INSTALL_SD,
                                   NexusInstallTarget_SdCard, "2: MicroSD Install");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        // A missing install target is not fatal -- the SD passthrough store is
        // still perfectly usable on its own.
        LOG_W("storage: SD install target unavailable (0x%x)", rc);
    }

    // --- Store 3: install to internal storage ---
    rc = nexusStorageInstallCreate(&g_storages[g_count], NEXUS_STORAGE_INSTALL_NAND,
                                   NexusInstallTarget_BuiltInUser, "3: System Install");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: NAND install target unavailable (0x%x)", rc);
    }

    // --- Store 4: game card, read-only ---
    rc = nexusStorageGamecardCreate(&g_storages[g_count], NEXUS_STORAGE_GAMECARD,
                                    "4: Game Card");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: game card store unavailable (0x%x)", rc);
    }

    // --- Store 5: saves ---
    rc = nexusStorageSavesCreate(&g_storages[g_count], NEXUS_STORAGE_SAVES,
                                 "5: Saves");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: saves store unavailable (0x%x)", rc);
    }

    // --- Store 6: installed titles ---
    rc = nexusStorageTitlesCreate(&g_storages[g_count], NEXUS_STORAGE_TITLES,
                                  "6: Installed Titles");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: titles store unavailable (0x%x)", rc);
    }

    // --- Store 7: raw NAND of the active MMC ---
    rc = nexusStorageBisCreate(&g_storages[g_count], NEXUS_STORAGE_BIS);
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: NAND store unavailable (0x%x)", rc);
    }

    // --- Store 8: album ---
    rc = nexusStorageAlbumCreate(&g_storages[g_count], NEXUS_STORAGE_ALBUM,
                                 "8: Album");
    if (R_SUCCEEDED(rc)) {
        g_count++;
    } else {
        LOG_W("storage: album store unavailable (0x%x)", rc);
    }

    g_initialized = true;
    LOG_I("storage: %zu store(s) registered", g_count);
    return 0;
}

void nexusStorageRegistryExit(void)
{
    if (!g_initialized) return;

    for (size_t i = 0; i < g_count; i++) {
        // Backends that hold an open write transaction get a chance to close
        // it cleanly before their state is freed.
        NexusStorage *s = &g_storages[i];
        if (s->ops != NULL && s->ops->write_end != NULL) {
            s->ops->write_end(s, false);
        }
        free(s->impl);
        s->impl = NULL;
    }

    memset(g_storages, 0, sizeof(g_storages));
    g_count       = 0;
    g_initialized = false;

    // The album holds a devoptab mount of its own, which outlives the store
    // struct and has to be released separately.
    nexusStorageAlbumDestroy();
}

void nexusStorageRegistryRefresh(void)
{
    for (size_t i = 0; i < g_count; i++) {
        NexusStorage *s = &g_storages[i];

        switch (s->storage_id) {
        case NEXUS_STORAGE_SDMC: {
            // libnx keeps sdmc: mounted for the life of the process, so a stat
            // of the root is enough to notice a card that has gone away.
            struct stat sb;
            const bool present = (stat("sdmc:/", &sb) == 0);
            if (present != s->present) {
                LOG_W("storage: SD card %s", present ? "appeared" : "disappeared");
                s->present = present;
                if (!present) nexusObjectDbClearStorage(s->storage_id);
            }
            break;
        }

        case NEXUS_STORAGE_GAMECARD: {
            // A swapped card must invalidate cached handles, or the host would
            // keep browsing the previous cartridge's listing.
            const bool was = s->present;
            nexusStorageGamecardRefresh(s);
            if (was != s->present) nexusObjectDbClearStorage(s->storage_id);
            break;
        }

        default:
            // Install and save stores do not change availability while running.
            break;
        }
    }
}

void nexusStorageRegistryOnSessionOpen(void)
{
    // Rebuilding the save listing means an ns lookup per title, which is far
    // too slow to do on every GetStorageIDs. Once per session is the right
    // cadence: it picks up saves created since launch without making routine
    // browsing crawl.
    for (size_t i = 0; i < g_count; i++) {
        if (g_storages[i].storage_id == NEXUS_STORAGE_SAVES) {
            nexusStorageSavesInvalidate(&g_storages[i]);
        } else if (g_storages[i].storage_id == NEXUS_STORAGE_TITLES) {
            // A title installed or deleted since the last session must show up.
            nexusStorageTitlesInvalidate(&g_storages[i]);
        }
    }
}

size_t nexusStorageCount(void)
{
    return g_count;
}

NexusStorage *nexusStorageAt(size_t index)
{
    return (index < g_count) ? &g_storages[index] : NULL;
}

NexusStorage *nexusStorageById(u32 storage_id)
{
    for (size_t i = 0; i < g_count; i++) {
        if (g_storages[i].storage_id == storage_id) return &g_storages[i];
    }
    return NULL;
}

bool nexusStorageIsPresent(u32 storage_id)
{
    const NexusStorage *s = nexusStorageById(storage_id);
    return s != NULL && s->present;
}
