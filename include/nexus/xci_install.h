// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install a title straight from an XCI or an inserted gamecard.
//
// The XCI's "secure" partition already holds exactly the NCAs an NSP would
// carry, so rather than writing a second installer this synthesises a PFS0
// header over those entries and feeds the existing streaming installer. From
// the installer's point of view it is reading an ordinary NSP.
//
// The consequence worth stating: installing from a card never stages anything.
// No 30 GB dump to the SD card first, and no free space needed beyond the
// installed footprint itself.
#pragma once

#include <switch.h>
#include <stdbool.h>

#include "nexus/installer.h"
#include "nexus/xci.h"

typedef struct {
    bool active;
    char name[128];
    char status[96];
    u64  received;
    u64  total;
    u32  content_count;
} NexusXciState;

/// Describes an XCI without installing anything.
typedef struct {
    bool valid;
    char problem[160];

    u32 content_count;    // NCAs in the secure partition
    u64 total_bytes;
    u64 cart_size;
    bool has_meta;        // a .cnmt.nca is present
} NexusXciInfo;

/// Inspects an XCI file on disk.
Result nexusXciInspectFile(const char *path, NexusXciInfo *out);

/// Inspects the gamecard currently in the slot.
Result nexusXciInspectGameCard(NexusXciInfo *out);

/// Installs from an XCI file on disk. Blocking.
Result nexusXciInstallFile(const char *path, u8 target);

/// Installs from the inserted gamecard. Blocking.
Result nexusXciInstallGameCard(u8 target);

const NexusXciState *nexusXciGetState(void);

void nexusXciCancel(void);
