// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- which storage the running system actually booted from.
//
// This matters for labelling. Horizon only ever mounts one NAND: if you booted
// into an emuMMC, then ncm, ns and the BIS partitions all describe the emuMMC,
// and the real sysMMC is invisible from inside the running system. There is no
// way to browse both at once short of reimplementing the filesystem and crypto
// offline, so instead NX-Nexus states plainly which one it is looking at.
#pragma once

#include <switch.h>
#include <stdbool.h>

/// Brings up spl and set:sys. Never fatal -- callers degrade to "unknown".
Result nexusSysInfoInit(void);

void nexusSysInfoExit(void);

/// True when Atmosphere booted from an emuMMC rather than the internal eMMC.
bool nexusSysInfoIsEmummc(void);

/// "emuMMC" or "sysMMC", or "unknown" when detection failed.
const char *nexusSysInfoStorageName(void);

/// System firmware version string, e.g. "18.1.0". Empty when unavailable.
const char *nexusSysInfoFirmware(void);
