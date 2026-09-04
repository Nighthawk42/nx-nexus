// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- system firmware installation.
//
// SAFETY, and it is the whole reason this feature exists in this shape:
// installation is refused unless the console booted from an **emuMMC**. A
// firmware install that goes wrong on sysMMC leaves a console that will not
// boot and cannot easily be repaired from the console itself. The same failure
// on an emuMMC is an inconvenience: sysMMC still boots, and the emuMMC can be
// restored from a NAND backup.
//
// Horizon mounts exactly one NAND, so "update the emuMMC" simply means running
// this while booted into it -- the content goes to whichever BuiltInSystem is
// active. That is why the emuMMC check is the only gate that matters.
//
// This is clean-room work from the switchbrew documentation. Atmosphere's
// Daybreak is GPL-2.0-**only**, which is incompatible with this project's
// GPL-3.0-or-later, so no code was taken from it.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_FIRMWARE_DIR   "sdmc:/firmware"
#define NEXUS_FIRMWARE_MAX_NCA  512

typedef enum {
    NexusFw_Ok = 0,
    NexusFw_NotEmummc,        // refused: running on sysMMC
    NexusFw_NoFolder,
    NexusFw_NoContent,        // folder holds no NCAs
    NexusFw_NoSystemUpdate,   // no SystemUpdate meta -- not a firmware set
    NexusFw_TooManyFiles,
    NexusFw_ReadFailed,
    NexusFw_InstallFailed,
    NexusFw_NoBackend,
    NexusFw_Cancelled,
} NexusFwResult;

const char *nexusFwStr(NexusFwResult r);

typedef struct {
    char dir[FS_MAX_PATH];

    u32  nca_count;      // every .nca in the folder
    u32  meta_count;     // how many of those are .cnmt.nca
    u64  total_bytes;

    bool has_system_update;
    bool valid;
    char problem[160];   // why it is not valid, when it is not
} NexusFirmwareSet;

typedef struct {
    bool active;
    u32  files_done;
    u32  files_total;
    u64  bytes_done;
    u64  bytes_total;
    char current[96];
    char status[96];
} NexusFwProgress;

/// True when installing is permitted at all. False on sysMMC, always.
bool nexusFirmwareInstallAllowed(void);

/// Inspects a folder without changing anything. Populates set->problem when
/// the folder is not a usable firmware set.
NexusFwResult nexusFirmwareScan(const char *dir, NexusFirmwareSet *set);

/// Installs a scanned set to the active BuiltInSystem storage.
/// Refuses outright unless nexusFirmwareInstallAllowed().
/// Blocking; the console must be rebooted afterwards.
NexusFwResult nexusFirmwareInstall(const NexusFirmwareSet *set);

const NexusFwProgress *nexusFirmwareGetProgress(void);

void nexusFirmwareCancel(void);
