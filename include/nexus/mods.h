// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- LayeredFS mods and cheat files.
//
// Atmosphere reads per-title content from:
//
//   sdmc:/atmosphere/contents/<16 hex title id>/
//       exefs/          executable replacements
//       romfs/          asset replacements
//       cheats/         <build id>.txt
//
// Disabling: Atmosphere only looks at directories whose names parse as a
// 16-character hex program id, so a folder renamed to "<id>.disabled" is
// skipped entirely. That is a convention of *this* tool rather than an
// Atmosphere feature, but it is reliable, reversible, and does not move a
// single byte of the mod itself.
//
// Toggling individual cheats at runtime is a different thing and is not
// possible here: dmnt:cht only works against a running game, and NX-Nexus is
// what is running.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_MODS_DIR      "sdmc:/atmosphere/contents"
#define NEXUS_MODS_MAX      256
#define NEXUS_MODS_SUFFIX   ".disabled"

typedef struct {
    u64  title_id;
    char name[96];         // resolved title name, or the id when unknown
    char dir[64];          // directory name as it exists on disk

    bool enabled;
    bool has_exefs;
    bool has_romfs;
    bool has_cheats;
    u32  cheat_files;
} NexusModEntry;

typedef struct {
    NexusModEntry entries[NEXUS_MODS_MAX];
    u32           count;
    bool          truncated;
} NexusModList;

/// Scans the contents directory. Titles are resolved to names where possible.
Result nexusModsScan(NexusModList *out);

/// Enables or disables one entry by renaming its directory.
/// The entry is updated in place on success.
Result nexusModsSetEnabled(NexusModEntry *entry, bool enabled);

/// Permanently deletes an entry's directory and everything in it.
Result nexusModsDelete(const NexusModEntry *entry);
