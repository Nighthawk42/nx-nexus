// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- storage maintenance.
//
// Cleans up what interrupted installs leave behind. Everything here is scoped
// to debris; nothing touches content a title still references.
#pragma once

#include <switch.h>

typedef struct {
    u32 placeholders;        // stray placeholder files
    u64 placeholder_bytes;   // space they occupy
    u64 free_bytes;          // combined free space across content storages
} NexusMaintenanceReport;

/// Looks for debris without changing anything.
Result nexusMaintenanceScan(NexusMaintenanceReport *out);

/// Deletes every placeholder on both content storages.
Result nexusMaintenanceCleanPlaceholders(u32 *out_removed);

/// Asks ns to drop application entities with no record behind them.
Result nexusMaintenanceCleanOrphans(u32 *out_removed);

// Note: libnx exposes no wrapper for invalidating the application control
// cache (title names and icons), so refreshing that is not offered here rather
// than hand-rolling IPC for something purely cosmetic.
