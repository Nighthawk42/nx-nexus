// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install a title straight from a URL.
//
// The bytes go from the network socket into the existing streaming installer
// without ever touching the SD card, exactly as the USB path does. Only the
// transport differs.
#pragma once

#include <switch.h>
#include <stdbool.h>

#include "nexus/installer.h"

typedef struct {
    bool active;
    char name[128];
    u64  received;
    u64  total;        // 0 when the server did not say
    char status[96];
} NexusNetInstallState;

/// Downloads url and installs it to target (a NexusInstallTarget).
/// Blocking; run it from a thread that can afford to wait.
Result nexusNetInstall(const char *url, const char *display_name, u8 target);

/// Live progress, for the UI to draw. Never NULL.
const NexusNetInstallState *nexusNetInstallGetState(void);

/// Asks an in-flight install to stop at the next chunk boundary.
void nexusNetInstallCancel(void);
