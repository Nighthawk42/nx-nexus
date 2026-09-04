// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- "will this actually launch?" checks.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nexus/compat.h"
#include "nexus/log.h"

// Exosphere config item 65000 is ExosphereApiVersion. The encoding puts the
// release version in the top bytes. From Atmosphere's exosphere ABI, as used
// by nxdumptool (GPL-3.0) -- see NOTICE.
#define SPL_CONFIG_EXOSPHERE_API_VERSION ((SplConfigItem)65000)

// Where Atmosphere looks for the patches that let unsigned content boot. A
// user who never copied them has empty directories, or none at all.
static const char *const g_patch_dirs[] = {
    "sdmc:/atmosphere/exefs_patches",
    "sdmc:/atmosphere/kip_patches",
};

static char            g_ams_version[16] = {0};
static NexusPatchState g_patch_state     = NexusPatches_Unknown;
static u32             g_firmware_value  = 0;

const char *nexusPatchStateStr(u8 state)
{
    switch (state) {
        case NexusPatches_Present: return "present";
        case NexusPatches_Absent:  return "not installed";
        default:                   return "unknown";
    }
}

// True when the directory exists and holds at least one entry. Atmosphere
// keeps each patch in its own subdirectory, so the shape does not matter --
// only that something is there.
static bool dir_has_entries(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) return false;

    bool found = false;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        found = true;
        break;
    }

    closedir(d);
    return found;
}

Result nexusCompatInit(void)
{
    if (R_SUCCEEDED(splInitialize())) {
        u64 version = 0;
        if (R_SUCCEEDED(splGetConfig(SPL_CONFIG_EXOSPHERE_API_VERSION, &version))) {
            snprintf(g_ams_version, sizeof(g_ams_version), "%u.%u.%u",
                     (unsigned)((version >> 56) & 0xFF),
                     (unsigned)((version >> 48) & 0xFF),
                     (unsigned)((version >> 40) & 0xFF));
        } else {
            LOG_W("compat: Exosphere version unavailable -- not Atmosphere?");
        }
        splExit();
    }

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            // Horizon packs a system version as major<<26 | minor<<20 | micro<<16.
            g_firmware_value = ((u32)fw.major << 26)
                             | ((u32)fw.minor << 20)
                             | ((u32)fw.micro << 16);
        }
        setsysExit();
    }

    bool any = false;
    for (size_t i = 0; i < sizeof(g_patch_dirs) / sizeof(g_patch_dirs[0]); i++) {
        if (dir_has_entries(g_patch_dirs[i])) { any = true; break; }
    }
    g_patch_state = any ? NexusPatches_Present : NexusPatches_Absent;

    LOG_I("compat: atmosphere %s, signature patches %s",
          g_ams_version[0] != '\0' ? g_ams_version : "unknown",
          nexusPatchStateStr((u8)g_patch_state));

    if (g_patch_state == NexusPatches_Absent) {
        LOG_W("compat: no patch files found -- installed titles may refuse to launch");
    }

    return 0;
}

const char *nexusCompatAtmosphereVersion(void)
{
    return g_ams_version;
}

NexusPatchState nexusCompatPatchState(void)
{
    return g_patch_state;
}

u32 nexusCompatFirmwareValue(void)
{
    return g_firmware_value;
}

void nexusCompatFormatVersion(u32 packed, char *out, size_t out_size)
{
    snprintf(out, out_size, "%u.%u.%u",
             (unsigned)((packed >> 26) & 0x3F),
             (unsigned)((packed >> 20) & 0x3F),
             (unsigned)((packed >> 16) & 0x0F));
}

bool nexusCompatTitleTooNew(u32 required_version)
{
    // Unknown either way means no useful comparison; saying nothing beats
    // warning about something that may be fine.
    if (required_version == 0 || g_firmware_value == 0) return false;

    return required_version > g_firmware_value;
}
