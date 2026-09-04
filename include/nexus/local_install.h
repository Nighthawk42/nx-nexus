// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install from files already on the SD card.
//
// Three shapes are recognised in the install folder:
//
//   Game.nsp / .nsz   an NSP, optionally with zstd-compressed contents
//   Game.xci / .xcz   a gamecard image; handled by xci_install
//   Game.nsp/         a *split* NSP: a directory of numbered parts
//                       00, 01, 02, ...
//
// The split form exists because FAT32 cannot hold a file over 4 GiB, so tools
// break large titles into parts and set the archive bit on the directory so
// Horizon presents it as one file. NX-Nexus reads the parts itself and
// concatenates them into a single stream, which means it works whether or not
// the archive bit is set -- a detail that trips people up constantly.
#pragma once

#include <switch.h>
#include <stdbool.h>

#include "nexus/installer.h"

#define NEXUS_LOCAL_DIR       "sdmc:/nsp"
#define NEXUS_LOCAL_MAX       256
#define NEXUS_LOCAL_MAX_PARTS 64

typedef enum {
    NexusLocalKind_Nsp = 0,
    NexusLocalKind_SplitNsp,
    NexusLocalKind_Xci,
} NexusLocalKind;

const char *nexusLocalKindStr(u8 kind);

typedef struct {
    char name[192];
    char path[FS_MAX_PATH];
    u8   kind;      // NexusLocalKind
    u64  size;
    u32  parts;     // split NSPs only
} NexusLocalItem;

typedef struct {
    NexusLocalItem items[NEXUS_LOCAL_MAX];
    u32            count;
    bool           truncated;
} NexusLocalList;

typedef struct {
    bool active;
    char name[192];
    char status[96];
    u64  received;
    u64  total;
} NexusLocalState;

/// Lists installable items in a folder. Never recurses: a directory is either
/// a split NSP or it is ignored.
Result nexusLocalScan(const char *dir, NexusLocalList *out);

/// Installs one scanned item. Blocking.
Result nexusLocalInstall(const NexusLocalItem *item, u8 target);

const NexusLocalState *nexusLocalGetState(void);

void nexusLocalCancel(void);
