// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- boot storage and firmware detection.

#include <stdio.h>
#include <string.h>

#include "nexus/sysinfo.h"
#include "nexus/log.h"

// Exosphere exposes its configuration through spl. Item 65007 is
// ExosphereEmummcType: zero means the internal eMMC, non-zero an emuMMC.
// The item number comes from Atmosphere's exosphere ABI, as used by
// nxdumptool (GPL-3.0) -- see NOTICE.
#define SPL_CONFIG_EXOSPHERE_EMUMMC_TYPE ((SplConfigItem)65007)

static bool g_is_emummc  = false;
static bool g_emummc_known = false;
static char g_firmware[32] = {0};

Result nexusSysInfoInit(void)
{
    // spl: emuMMC detection.
    if (R_SUCCEEDED(splInitialize())) {
        u64 type = 0;
        if (R_SUCCEEDED(splGetConfig(SPL_CONFIG_EXOSPHERE_EMUMMC_TYPE, &type))) {
            g_is_emummc    = (type != 0);
            g_emummc_known = true;
        } else {
            // Expected on stock or on a CFW that is not Atmosphere.
            LOG_W("sysinfo: Exosphere emuMMC config unavailable");
        }
        splExit();
    } else {
        LOG_W("sysinfo: splInitialize failed");
    }

    // set:sys: firmware version.
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            snprintf(g_firmware, sizeof(g_firmware), "%u.%u.%u",
                     fw.major, fw.minor, fw.micro);
        }
        setsysExit();
    }

    LOG_I("sysinfo: running on %s, firmware %s",
          nexusSysInfoStorageName(),
          g_firmware[0] != '\0' ? g_firmware : "unknown");
    return 0;
}

void nexusSysInfoExit(void)
{
    // Both services are closed immediately after use in init; nothing to do.
}

bool nexusSysInfoIsEmummc(void)
{
    return g_is_emummc;
}

const char *nexusSysInfoStorageName(void)
{
    if (!g_emummc_known) return "unknown";
    return g_is_emummc ? "emuMMC" : "sysMMC";
}

const char *nexusSysInfoFirmware(void)
{
    return g_firmware;
}
