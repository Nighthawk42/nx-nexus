// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- self-update.
//
// The update source is whatever the user puts in sources.json as "update_url".
// Nothing is baked in, and nothing is checked automatically at startup: an
// update happens when someone asks for one from the menu.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_VERSION      "0.1.0"

/// Where the .nro is assumed to live when the launcher does not say.
#define NEXUS_UPDATE_PATH  "sdmc:/switch/NX-Nexus.nro"

typedef struct {
    bool checked;
    bool available_is_newer;
    bool applied;

    char installed[24];
    char available[24];
    char url[512];
    char notes[160];
    char status[96];

    u64 received;
    u64 total;
} NexusUpdateState;

const char *nexusUpdateVersion(void);

/// Tells the updater where the running .nro actually is, from argv[0].
///
/// This matters more than it looks: installing through the Homebrew App Store
/// puts the file in switch/NX-Nexus/, not switch/. Updating a hardcoded path
/// would write a second copy that never runs, leaving the user on an old build
/// that reports itself as updated. Ignores anything that is not an .nro path.
void nexusUpdateSetSelfPath(const char *argv0);

/// The path the updater will replace. Never NULL.
const char *nexusUpdateSelfPath(void);

/// Fetches and parses the manifest. Does not download anything large.
Result nexusUpdateCheck(void);

/// Downloads and installs the build the last check found. Verifies the file is
/// an NRO before replacing the current one, and keeps a .bak.
Result nexusUpdateApply(void);

const NexusUpdateState *nexusUpdateGetState(void);
