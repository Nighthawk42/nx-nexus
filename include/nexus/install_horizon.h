// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- the ncm/es/ns install backend.
//
// Implements NexusInstallBackendOps against the real system services. All
// sequencing lives in installer.c; this is only the translation layer.
#pragma once

#include <switch.h>

#include "nexus/installer.h"

typedef struct NexusHorizonBackend NexusHorizonBackend;

/// Brings up ncm, ns and es. Call once at startup, before creating a backend.
Result nexusInstallServicesInit(void);

void nexusInstallServicesExit(void);

/// Opens the content storage and meta database for one target
/// (NexusInstallTarget_SdCard or _BuiltInUser). Returns NULL on failure.
NexusHorizonBackend *nexusHorizonBackendCreate(u8 target_storage);

void nexusHorizonBackendDestroy(NexusHorizonBackend *b);

/// The op table to hand to nexusInstallBegin.
const NexusInstallBackendOps *nexusHorizonBackendOps(void);
