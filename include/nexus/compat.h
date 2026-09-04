// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- "will this actually launch?" checks.
//
// Two things account for most of the "it installed but it does not work"
// reports in this scene, and both are knowable before the user is confused:
//
//   1. The title needs newer firmware than the console runs.
//   2. The console has no signature patches, so anything not signed by
//      Nintendo refuses to boot.
//
// The second cannot be tested directly from inside a running system -- the
// patches are applied by the bootloader long before this code exists. What can
// be checked is whether patch files are installed at all, which catches the
// common case of a user who never copied them. That is a heuristic and is
// labelled as one everywhere it is surfaced; it is not a guarantee.
#pragma once

#include <switch.h>
#include <stdbool.h>

typedef enum {
    NexusPatches_Present = 0,   // patch files exist in the expected places
    NexusPatches_Absent,        // none found -- unsigned content will not boot
    NexusPatches_Unknown,       // could not tell
} NexusPatchState;

const char *nexusPatchStateStr(u8 state);

/// Brings up what the checks need. Cheap; never fatal.
Result nexusCompatInit(void);

/// Atmosphere's version as "1.7.1", or empty when not running Atmosphere.
const char *nexusCompatAtmosphereVersion(void);

/// Whether signature patches appear to be installed. See the header note:
/// this inspects files on the SD card, it does not verify that patching worked.
NexusPatchState nexusCompatPatchState(void);

/// Packs a firmware version the way Horizon does, so the values the CNMT
/// carries can be compared with the running system.
u32 nexusCompatFirmwareValue(void);

/// Formats a packed system version as "18.1.0". out needs 16 bytes.
void nexusCompatFormatVersion(u32 packed, char *out, size_t out_size);

/// True when a title requiring required_version cannot run on this console.
/// A required_version of 0 means the title did not say, and is never too new.
bool nexusCompatTitleTooNew(u32 required_version);
