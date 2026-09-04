// SPDX-License-Identifier: GPL-3.0-or-later
//
// NX-Nexus -- MTP server and streaming installer for the Nintendo Switch.
// Copyright (C) 2026 NX-Nexus contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// NX-Nexus -- entry point and console UI.
//
// The USB interface is NOT brought up at launch. Publishing an MTP device the
// moment the app opens means the console appears on whatever it is plugged
// into without anyone asking for it, so the server is started explicitly from
// the menu and torn down again on stop.
//
// While the server runs the MTP responder lives on its own thread with
// blocking USB reads; the main thread only draws and reads input.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/log.h"
#include "nexus/menu.h"
#include "nexus/usb_transport.h"
#include "nexus/mtp_server.h"
#include "nexus/storage.h"
#include "nexus/object_db.h"
#include "nexus/install_horizon.h"
#include "nexus/sysinfo.h"
#include "nexus/compat.h"
#include "nexus/http.h"
#include "nexus/sources.h"
#include "nexus/netinstall.h"
#include "nexus/update.h"
#include "nexus/maintenance.h"
#include "nexus/firmware.h"
#include "nexus/verify.h"
#include "nexus/local_install.h"
#include "nexus/xci_install.h"
#include "nexus/title_list.h"
#include "nexus/mods.h"

#define WORKER_STACK_SIZE (64 * 1024)
#define WORKER_PRIORITY   0x2C          // slightly above the default 0x2D

#define UI_WIDTH      79
#define UI_BODY_ROWS  22

#define C_RESET  "\x1b[0m"
#define C_DIM    "\x1b[37m"
#define C_TITLE  "\x1b[36m"
#define C_OK     "\x1b[32m"
#define C_WARN   "\x1b[33m"
#define C_ERR    "\x1b[31m"
#define C_VALUE  "\x1b[97m"

// ---------------------------------------------------------------------------
// MTP worker
// ---------------------------------------------------------------------------

static Thread        g_worker;
static volatile bool g_worker_run = false;
static bool          g_worker_started = false;
static bool          g_server_up = false;

static void worker_main(void *arg)
{
    (void)arg;
    LOG_I("worker: started");

    while (g_worker_run) {
        if (!usbTransportIsReady()) {
            if (mtpServerGetState()->session_open) mtpServerResetSession();
            svcSleepThread(100000000ull);   // 100 ms
            continue;
        }

        Result rc = mtpServerRunOnce(UINT64_MAX);
        if (R_FAILED(rc) && g_worker_run) {
            LOG_D("worker: transaction ended with 0x%x", rc);
            svcSleepThread(50000000ull);
        }
    }

    LOG_I("worker: stopped");
}

// Brings up USB and the responder. Nothing is published to a host until this
// runs, which is the whole point of not auto-starting.
static bool server_start(void)
{
    if (g_server_up) return true;

    if (R_FAILED(usbTransportInit())) {
        LOG_E("server: USB init failed -- is another USB homebrew running?");
        return false;
    }
    if (R_FAILED(mtpServerInit())) {
        LOG_E("server: MTP init failed");
        usbTransportExit();
        return false;
    }

    g_worker_run = true;
    if (R_FAILED(threadCreate(&g_worker, worker_main, NULL, NULL,
                              WORKER_STACK_SIZE, WORKER_PRIORITY, -2))
        || R_FAILED(threadStart(&g_worker))) {
        LOG_E("server: could not start the worker thread");
        g_worker_run = false;
        mtpServerExit();
        usbTransportExit();
        return false;
    }

    g_worker_started = true;
    g_server_up      = true;
    LOG_I("server: started -- the console is now visible over USB");
    return true;
}

static void server_stop(void)
{
    if (!g_server_up) return;

    if (g_worker_started) {
        g_worker_run = false;
        usbTransportCancel();       // unblock the parked USB read
        threadWaitForExit(&g_worker);
        threadClose(&g_worker);
        g_worker_started = false;
    }

    mtpServerExit();
    usbTransportExit();
    g_server_up = false;
    LOG_I("server: stopped -- no longer visible over USB");
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static const char *speed_name(UsbDeviceSpeed speed)
{
    switch (speed) {
        case UsbDeviceSpeed_Low:   return "USB 1.0";
        case UsbDeviceSpeed_Full:  return "USB 1.1";
        case UsbDeviceSpeed_High:  return "USB 2.0";
        case UsbDeviceSpeed_Super: return "USB 3.0";
        default:                   return "-";
    }
}

static void fmt_bytes(char *out, size_t out_size, u64 bytes)
{
    if (bytes >= (1ull << 30)) {
        snprintf(out, out_size, "%llu.%01llu GiB",
                 (unsigned long long)(bytes >> 30),
                 (unsigned long long)(((bytes % (1ull << 30)) * 10) >> 30));
    } else if (bytes >= (1ull << 20)) {
        snprintf(out, out_size, "%llu.%01llu MiB",
                 (unsigned long long)(bytes >> 20),
                 (unsigned long long)(((bytes % (1ull << 20)) * 10) >> 20));
    } else if (bytes >= 1024) {
        snprintf(out, out_size, "%llu KiB", (unsigned long long)(bytes >> 10));
    } else {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    }
}

static const char *store_label(u32 storage_id)
{
    switch (storage_id) {
        case NEXUS_STORAGE_SDMC:         return "1: MicroSD";
        case NEXUS_STORAGE_INSTALL_SD:   return "2: MicroSD Install";
        case NEXUS_STORAGE_INSTALL_NAND: return "3: System Install";
        case NEXUS_STORAGE_GAMECARD:     return "4: Game Card";
        case NEXUS_STORAGE_SAVES:        return "5: Saves";
        case NEXUS_STORAGE_TITLES:       return "6: Installed Titles";
        case NEXUS_STORAGE_BIS:          return "7: NAND";
        case NEXUS_STORAGE_ALBUM:        return "8: Album";
        default:                         return "Store";
    }
}

static void draw_rule(char edge)
{
    putchar(' ');
    putchar(edge);
    for (int i = 0; i < UI_WIDTH - 2; i++) putchar('-');
    putchar(edge);
    putchar('\n');
}

static void draw_header(const char *screen)
{
    draw_rule('=');
    printf(" " C_TITLE "NX-Nexus" C_RESET " " C_DIM "0.1.0-dev" C_RESET
           "   %-22s " C_DIM "%s | fw %s" C_RESET "\n",
           screen,
           nexusSysInfoStorageName(),
           nexusSysInfoFirmware()[0] != '\0' ? nexusSysInfoFirmware() : "?");

    // One always-visible line for server state, so whether the console is
    // exposed over USB is never a surprise.
    if (!g_server_up) {
        printf(" " C_DIM "Server: stopped -- not visible over USB" C_RESET "\n");
    } else if (usbTransportIsReady()) {
        const MtpServerState *st = mtpServerGetState();
        char rate[24];
        fmt_bytes(rate, sizeof(rate), st->stats.last_rate_bps);
        printf(" " C_OK "Server: connected" C_RESET "  %s  %s/s\n",
               speed_name(usbTransportGetSpeed()), rate);
    } else {
        printf(" " C_WARN "Server: running -- waiting for a host" C_RESET "\n");
    }

    draw_rule('=');
}

static void draw_footer(const char *hints)
{
    draw_rule('=');
    printf(" " C_DIM "%s" C_RESET "\n", hints);
    printf(" " C_DIM "GPLv3+, ABSOLUTELY NO WARRANTY. See LICENSE." C_RESET "\n");
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

typedef enum {
    Screen_Main = 0,
    Screen_Stores,
    Screen_Transfer,
    Screen_Sources,
    Screen_Items,
    Screen_Maintenance,
    Screen_Update,
    Screen_Firmware,
    Screen_Local,
    Screen_Titles,
    Screen_Verify,
    Screen_Mods,
    Screen_Log,
    Screen_About,
    Screen_Quit,
} Screen;

enum {
    Act_ToggleServer = 1,
    Act_Stores,
    Act_Transfer,
    Act_Sources,
    Act_Maintenance,
    Act_Update,
    Act_Firmware,
    Act_Local,
    Act_Gamecard,
    Act_Titles,
    Act_Mods,
    Act_VerifyAll,
    Act_Log,
    Act_About,
    Act_Quit,

    // Maintenance actions.
    Act_Mnt_Scan = 100,
    Act_Mnt_Placeholders,
    Act_Mnt_Orphans,

    // Update actions.
    Act_Upd_Check = 200,
    Act_Upd_Apply,

    // Firmware actions. Installing is deliberately two steps.
    Act_Fw_Scan = 300,
    Act_Fw_Arm,
    Act_Fw_Install,

    // Local install actions.
    Act_Loc_Rescan = 400,

    // Rows that stand for one entry in a list are a base plus an index.

    // Source rows are Act_Source_Base + index; item rows are
    // Act_Item_Base + index.
    Act_Source_Base = 1000,
    Act_Item_Base   = 5000,
    Act_Local_Base  = 9000,
    Act_Title_Base  = 20000,
    Act_Mod_Base    = 40000,
};

// Where a network install should land. The SD card is the sane default and the
// only one most people want.
#define NET_INSTALL_TARGET NexusInstallTarget_SdCard

static NexusSourceItem *g_items = NULL;
static u32              g_item_count = 0;
static int              g_source_index = -1;
static char             g_action_msg[128] = "";
static NexusFirmwareSet g_fw_set;
static bool             g_fw_scanned = false;
static bool             g_fw_armed = false;   // second-step confirmation

// Local install (SD card) state.
static NexusLocalList  *g_local = NULL;

// Installed-title browser state.
static NexusTitleList  *g_titles = NULL;
static u8               g_title_sort   = NexusTitleSort_Name;
static u8               g_title_filter = NexusTitleFilter_All;
static int              g_title_armed  = -1;   // index armed for deletion

// Mods and cheats.
static NexusModList    *g_mods = NULL;
static int              g_mod_armed = -1;

// Verification.
static NexusVerifyReport g_verify;
static bool              g_verify_done = false;

// Where an install from the SD card or a gamecard lands. Same default as the
// network path: the SD card is what nearly everyone wants.
#define LOCAL_INSTALL_TARGET NexusInstallTarget_SdCard


// ---------------------------------------------------------------------------
// Network screens
// ---------------------------------------------------------------------------

static void build_sources_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 4);

    const NexusSourcesConfig *cfg = nexusSourcesGet();
    if (cfg->count == 0) {
        menuAddHeading(m, "No sources configured.");
        menuAddSpacer(m);
        menuAddHeading(m, "Edit sdmc:/switch/nx-nexus/sources.json and add your own.");
        return;
    }

    for (u32 i = 0; i < cfg->count; i++) {
        menuAdd(m, (int)(Act_Source_Base + i), cfg->sources[i].name, "");
    }
}

// Fetches and parses a source index into g_items.
static void load_source_items(u32 index)
{
    const NexusSourcesConfig *cfg = nexusSourcesGet();
    if (index >= cfg->count) return;

    free(g_items);
    g_items = NULL;
    g_item_count = 0;

    if (!nexusHttpIsReady()) {
        snprintf(g_action_msg, sizeof(g_action_msg), "networking is not up");
        return;
    }

    char *buf = (char *)malloc(NEXUS_SOURCE_INDEX_MAX);
    if (buf == NULL) {
        snprintf(g_action_msg, sizeof(g_action_msg), "out of memory");
        return;
    }

    size_t len = 0;
    long status = 0;
    const NexusHttpResult hr = nexusHttpGetBuffer(cfg->sources[index].url, buf,
                                                  NEXUS_SOURCE_INDEX_MAX - 1, &len, &status);
    if (hr != NexusHttp_Ok) {
        snprintf(g_action_msg, sizeof(g_action_msg), "%s (%ld)", nexusHttpStr(hr), status);
        free(buf);
        return;
    }

    g_items = (NexusSourceItem *)calloc(NEXUS_SOURCE_ITEMS_MAX, sizeof(NexusSourceItem));
    if (g_items == NULL) {
        snprintf(g_action_msg, sizeof(g_action_msg), "out of memory");
        free(buf);
        return;
    }

    const NexusFmtResult r = nexusSourcesParseIndex(buf, len, g_items,
                                                    NEXUS_SOURCE_ITEMS_MAX, &g_item_count);
    free(buf);

    if (r != NexusFmt_Ok) {
        snprintf(g_action_msg, sizeof(g_action_msg), "index: %s", nexusFmtStr(r));
        free(g_items);
        g_items = NULL;
        g_item_count = 0;
        return;
    }

    snprintf(g_action_msg, sizeof(g_action_msg), "%u item(s)", g_item_count);
    LOG_I("sources: %s listed %u item(s)", cfg->sources[index].name, g_item_count);
}

static void build_items_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 4);

    if (g_item_count == 0) {
        menuAddHeading(m, g_action_msg[0] != '\0' ? g_action_msg : "Nothing listed.");
        return;
    }

    for (u32 i = 0; i < g_item_count; i++) {
        char detail[MENU_DETAIL_LEN];
        if (g_items[i].size > 0) fmt_bytes(detail, sizeof(detail), g_items[i].size);
        else                     detail[0] = '\0';

        menuAdd(m, (int)(Act_Item_Base + i), g_items[i].name, detail);
    }
}

static void build_maintenance_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 4);
    menuAdd(m, Act_Mnt_Scan,         "Scan for leftover data",       "");
    menuAdd(m, Act_Mnt_Placeholders, "Clear stray placeholders",     "");
    menuAdd(m, Act_Mnt_Orphans,      "Remove redundant title data",  "");
}

static void build_update_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 6);

    const NexusUpdateState *u = nexusUpdateGetState();
    menuAdd(m, Act_Upd_Check, "Check now", "");

    if (u->checked && u->available_is_newer) {
        menuAdd(m, Act_Upd_Apply, "Download and install", u->available);
    }
}

static void draw_message_block(void)
{
    if (g_action_msg[0] == '\0') return;
    printf("\n   " C_WARN "%s" C_RESET "\n", g_action_msg);
}

static void draw_sources_intro(void)
{
    // The policy is stated where the feature is used, not only in the docs.
    printf("\n   " C_DIM "Sources are yours alone. Nothing ships configured, and" C_RESET "\n");
    printf("   " C_DIM "public \"free shops\" are not supported or endorsed." C_RESET "\n");
    printf("   " C_DIM "Point this at your own server." C_RESET "\n\n");
}

static void draw_update(void)
{
    const NexusUpdateState *u = nexusUpdateGetState();

    printf("\n   Installed   " C_VALUE "%s" C_RESET "\n", u->installed);

    if (u->checked) {
        printf("   Available   " C_VALUE "%s" C_RESET "\n", u->available);
        if (u->notes[0] != '\0') {
            printf("   Notes       " C_DIM "%s" C_RESET "\n", u->notes);
        }
    }
    if (u->status[0] != '\0') {
        printf("   Status      %s\n", u->status);
    }

    if (u->total > 0 && u->received < u->total) {
        char got[24], tot[24];
        fmt_bytes(got, sizeof(got), u->received);
        fmt_bytes(tot, sizeof(tot), u->total);
        printf("   Progress    " C_VALUE "%s / %s" C_RESET "\n", got, tot);
    }

    printf("\n   " C_DIM "The update source is \"update_url\" in sources.json." C_RESET "\n");
    printf("   " C_DIM "Nothing is ever checked automatically." C_RESET "\n");
}

static void draw_maintenance(const NexusMaintenanceReport *rep, bool scanned)
{
    printf("\n");

    if (scanned) {
        char ph[24], freesp[24];
        fmt_bytes(ph, sizeof(ph), rep->placeholder_bytes);
        fmt_bytes(freesp, sizeof(freesp), rep->free_bytes);

        printf("   Stray placeholders  " C_VALUE "%u" C_RESET " (%s)\n",
               rep->placeholders, ph);
        printf("   Free space          " C_VALUE "%s" C_RESET "\n\n", freesp);
    } else {
        printf("   " C_DIM "Run a scan to see what is reclaimable." C_RESET "\n\n");
    }
}


static void build_firmware_menu(Menu *m)
{
    menuReset(m, 6);

    menuAdd(m, Act_Fw_Scan, "Scan " NEXUS_FIRMWARE_DIR, "");

    if (!nexusFirmwareInstallAllowed()) return;   // nothing else is offered

    if (g_fw_scanned && g_fw_set.valid) {
        if (!g_fw_armed) {
            menuAdd(m, Act_Fw_Arm, "I have a NAND backup - continue", "");
        } else {
            menuAdd(m, Act_Fw_Install, "INSTALL FIRMWARE NOW", "irreversible");
        }
    }
}

static void draw_firmware(void)
{
    printf("\n");

    if (!nexusFirmwareInstallAllowed()) {
        printf("   " C_ERR "Blocked: this console booted from %s." C_RESET "\n\n",
               nexusSysInfoStorageName());
        printf("   " C_DIM "Firmware installation is only offered on an emuMMC." C_RESET "\n");
        printf("   " C_DIM "A bad firmware install on sysMMC leaves a console that" C_RESET "\n");
        printf("   " C_DIM "will not boot. On an emuMMC, sysMMC still boots and the" C_RESET "\n");
        printf("   " C_DIM "emuMMC can be restored from a backup." C_RESET "\n\n");
        printf("   " C_DIM "Boot into your emuMMC and run this again." C_RESET "\n");
        return;
    }

    printf("   Target      " C_VALUE "%s system partition" C_RESET "\n",
           nexusSysInfoStorageName());
    printf("   Current fw  " C_VALUE "%s" C_RESET "\n\n",
           nexusSysInfoFirmware()[0] != '\0' ? nexusSysInfoFirmware() : "unknown");

    if (!g_fw_scanned) {
        printf("   " C_DIM "Put a firmware folder at " NEXUS_FIRMWARE_DIR " and scan." C_RESET "\n\n");
    } else if (!g_fw_set.valid) {
        printf("   " C_ERR "%s" C_RESET "\n\n", g_fw_set.problem);
    } else {
        char total[24];
        fmt_bytes(total, sizeof(total), g_fw_set.total_bytes);
        printf("   Found       " C_VALUE "%u NCAs" C_RESET " (%u meta), %s\n\n",
               g_fw_set.nca_count, g_fw_set.meta_count, total);

        printf("   " C_WARN "Back up your NAND first." C_RESET " " C_DIM
               "Store 7 dumps the raw" C_RESET "\n");
        printf("   " C_DIM "partitions; a failed install is only recoverable from one."
               C_RESET "\n\n");
        printf("   " C_DIM "Do not power off during the install. Reboot afterwards."
               C_RESET "\n\n");

        if (g_fw_armed) {
            printf("   " C_ERR "Armed. The next action writes to the system partition."
                   C_RESET "\n\n");
        }
    }

    const NexusFwProgress *pr = nexusFirmwareGetProgress();
    if (pr->status[0] != '\0') {
        printf("   Status      %s\n", pr->status);
    }
}


// ---------------------------------------------------------------------------
// Install from the SD card or a gamecard
// ---------------------------------------------------------------------------

static void rescan_local(void)
{
    if (g_local == NULL) g_local = (NexusLocalList *)calloc(1, sizeof(*g_local));
    if (g_local != NULL) nexusLocalScan(NEXUS_LOCAL_DIR, g_local);
}

static void build_local_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 6);

    NexusXciInfo card;
    const bool have_card = R_SUCCEEDED(nexusXciInspectGameCard(&card)) && card.valid;

    menuAddHeading(m, "Gamecard");
    menuAdd(m, Act_Gamecard, "Install from the inserted gamecard",
            have_card ? "ready" : "no card");

    menuAddSpacer(m);
    menuAddHeading(m, "SD card -- " NEXUS_LOCAL_DIR);
    menuAdd(m, Act_Loc_Rescan, "Rescan", "");

    if (g_local == NULL || g_local->count == 0) return;

    for (u32 i = 0; i < g_local->count; i++) {
        const NexusLocalItem *it = &g_local->items[i];

        char detail[MENU_DETAIL_LEN];
        char size[24];
        fmt_bytes(size, sizeof(size), it->size);

        if (it->kind == NexusLocalKind_SplitNsp) {
            snprintf(detail, sizeof(detail), "%u parts, %.12s", it->parts, size);
        } else {
            snprintf(detail, sizeof(detail), "%.6s %.14s",
                     nexusLocalKindStr(it->kind), size);
        }

        menuAdd(m, (int)(Act_Local_Base + i), it->name, detail);
    }
}

static void draw_local(void)
{
    printf("\n");

    if (g_local == NULL) {
        printf("   " C_DIM "Choose Rescan to look for installable files." C_RESET "\n\n");
        return;
    }

    printf("   " C_DIM "Put .nsp or .xci files in " NEXUS_LOCAL_DIR ". A split NSP is a"
           C_RESET "\n");
    printf("   " C_DIM "folder of numbered parts and works with or without the archive"
           C_RESET "\n");
    printf("   " C_DIM "bit set. Nothing is copied: bytes go straight into ncm."
           C_RESET "\n\n");

    if (g_local->count == 0) {
        printf("   " C_WARN "Nothing installable found." C_RESET "\n\n");
    }
}

// ---------------------------------------------------------------------------
// Installed titles
// ---------------------------------------------------------------------------

static void rebuild_titles(void)
{
    if (g_titles == NULL) g_titles = (NexusTitleList *)calloc(1, sizeof(*g_titles));
    if (g_titles == NULL) return;

    nexusTitleListBuild(g_titles);
    nexusTitleListSort(g_titles, g_title_sort);
    g_title_armed = -1;
}

static void build_titles_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 4);

    if (g_titles == NULL) return;

    for (u32 i = 0; i < g_titles->count; i++) {
        const NexusTitleInfo *t = &g_titles->items[i];
        if (!nexusTitleListMatches(t, g_title_filter)) continue;

        char detail[MENU_DETAIL_LEN];
        if ((int)i == g_title_armed) {
            snprintf(detail, sizeof(detail), "X again = DELETE");
        } else {
            fmt_bytes(detail, sizeof(detail), t->size);
        }

        menuAdd(m, (int)(Act_Title_Base + i), t->name, detail);
    }
}

static void draw_titles(void)
{
    printf("\n");

    if (g_titles == NULL || g_titles->count == 0) {
        printf("   " C_DIM "No installed titles found." C_RESET "\n\n");
        return;
    }

    char total[24];
    fmt_bytes(total, sizeof(total), g_titles->total_bytes);

    printf("   %u title%s, %s installed        " C_DIM "sort: " C_RESET "%s"
           "   " C_DIM "show: " C_RESET "%s\n\n",
           g_titles->count, g_titles->count == 1 ? "" : "s", total,
           nexusTitleSortStr(g_title_sort), nexusTitleFilterStr(g_title_filter));
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

// Redrawing on every chunk would cost more than the hashing, so the screen is
// refreshed on a coarse counter. The same callback polls for a cancel, which is
// why verification can be interrupted at all.
static bool verify_tick(void *user, const NexusVerifyReport *r)
{
    PadState *pad = (PadState *)user;
    static u32 ticks = 0;

    if ((ticks++ & 0x0F) != 0) return true;

    padUpdate(pad);
    if (padGetButtonsDown(pad) & HidNpadButton_B) {
        LOG_W("verify: cancelled by the user");
        return false;
    }

    char done[24];
    fmt_bytes(done, sizeof(done), r->bytes_done);

    consoleClear();
    draw_header("Verifying");
    printf("\n   Title  " C_VALUE "%u of %u" C_RESET "\n", r->titles_done + 1,
           r->titles_total);
    printf("   Now    %.60s\n", r->current);
    printf("   Hashed " C_VALUE "%s" C_RESET "\n\n", done);
    printf("   Good   " C_OK "%u" C_RESET "     Bad  %s%u" C_RESET "\n",
           r->contents_ok, r->contents_bad > 0 ? C_ERR : C_DIM, r->contents_bad);
    printf("\n   " C_DIM "Reading every installed NCA and checking it against the"
           C_RESET "\n");
    printf("   " C_DIM "hash in its manifest. This takes a while." C_RESET "\n");
    draw_footer("[B] stop");
    consoleUpdate(NULL);
    return true;
}

static void draw_verify(void)
{
    printf("\n");

    if (!g_verify_done) {
        printf("   " C_DIM "Nothing checked yet." C_RESET "\n");
        return;
    }

    char done[24];
    fmt_bytes(done, sizeof(done), g_verify.bytes_done);

    printf("   Checked     " C_VALUE "%u title(s)" C_RESET ", %s hashed\n",
           g_verify.titles_done, done);
    printf("   Good        " C_OK "%u" C_RESET "\n", g_verify.contents_ok);
    printf("   Bad         %s%u" C_RESET "\n\n",
           g_verify.contents_bad > 0 ? C_ERR : C_DIM, g_verify.contents_bad);

    if (g_verify.cancelled) {
        printf("   " C_WARN "Stopped early -- the rest was not checked." C_RESET "\n\n");
    }

    if (g_verify.contents_bad == 0 && !g_verify.cancelled) {
        printf("   " C_OK "Everything matches its manifest." C_RESET "\n");
        return;
    }

    for (u32 i = 0; i < g_verify.issue_count && i < 10; i++) {
        const NexusVerifyIssue *e = &g_verify.issues[i];
        printf("   " C_ERR "%-10s" C_RESET " %.34s  " C_DIM "%.16s" C_RESET "\n",
               nexusVerifyIssueStr(e->kind), e->title, e->content);
    }

    if (g_verify.issue_count > 10 || g_verify.issues_truncated) {
        printf("   " C_DIM "...see the log for the rest." C_RESET "\n");
    }

    printf("\n   " C_DIM "Reinstall anything listed here. Corruption on an SD card"
           C_RESET "\n");
    printf("   " C_DIM "usually means the card itself is failing." C_RESET "\n");
}

// ---------------------------------------------------------------------------
// Mods and cheats
// ---------------------------------------------------------------------------

static void rescan_mods(void)
{
    if (g_mods == NULL) g_mods = (NexusModList *)calloc(1, sizeof(*g_mods));
    if (g_mods != NULL) nexusModsScan(g_mods);
    g_mod_armed = -1;
}

static void build_mods_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS - 5);

    if (g_mods == NULL) return;

    for (u32 i = 0; i < g_mods->count; i++) {
        const NexusModEntry *e = &g_mods->entries[i];

        char label[MENU_LABEL_LEN];
        snprintf(label, sizeof(label), "%.28s %s%s%s",
                 e->name,
                 e->has_exefs  ? "exefs "  : "",
                 e->has_romfs  ? "romfs "  : "",
                 e->has_cheats ? "cheats"  : "");

        char detail[MENU_DETAIL_LEN];
        if ((int)i == g_mod_armed) {
            snprintf(detail, sizeof(detail), "X again = DELETE");
        } else {
            snprintf(detail, sizeof(detail), "%s", e->enabled ? "enabled" : "disabled");
        }

        menuAdd(m, (int)(Act_Mod_Base + i), label, detail);
    }
}

static void draw_mods(void)
{
    printf("\n");

    if (g_mods == NULL || g_mods->count == 0) {
        printf("   " C_DIM "No LayeredFS mods or cheat files found in" C_RESET "\n");
        printf("   " C_DIM NEXUS_MODS_DIR "." C_RESET "\n\n");
        return;
    }

    printf("   " C_DIM "Disabling renames the folder so Atmosphere skips it. Nothing"
           C_RESET "\n");
    printf("   " C_DIM "is moved or rewritten, and enabling puts the name back."
           C_RESET "\n\n");
}

static void build_main_menu(Menu *m)
{
    menuReset(m, UI_BODY_ROWS);

    char detail[MENU_DETAIL_LEN];
    snprintf(detail, sizeof(detail), "%s", g_server_up ? "running" : "stopped");
    menuAdd(m, Act_ToggleServer,
            g_server_up ? "Stop MTP server" : "Start MTP server", detail);

    menuAddSpacer(m);

    snprintf(detail, sizeof(detail), "%zu", nexusStorageCount());
    menuAdd(m, Act_Stores,   "Stores",          detail);
    menuAdd(m, Act_Transfer, "Transfer status", "");

    menuAddSpacer(m);
    menuAddHeading(m, "Install");

    if (g_local != NULL && g_local->count > 0) {
        snprintf(detail, sizeof(detail), "%u found", g_local->count);
    } else {
        detail[0] = '\0';
    }
    menuAdd(m, Act_Local, "Install from SD card or gamecard", detail);

    const NexusSourcesConfig *cfg = nexusSourcesGet();
    snprintf(detail, sizeof(detail), "%u configured", cfg->count);
    menuAdd(m, Act_Sources, "Install from a source", detail);

    menuAddSpacer(m);
    menuAddHeading(m, "Manage");

    menuAdd(m, Act_Titles,    "Installed titles", "browse, delete");
    menuAdd(m, Act_Mods,      "Mods and cheats",  "");
    menuAdd(m, Act_VerifyAll, "Verify installed content", "");
    menuAdd(m, Act_Maintenance, "Maintenance",  "");

    menuAddSpacer(m);
    menuAddHeading(m, "System");
    menuAdd(m, Act_Firmware, "Install firmware",
            nexusFirmwareInstallAllowed() ? "emuMMC" : "sysMMC - blocked");
    menuAdd(m, Act_Update,   "Check for updates",
            nexusHttpIsReady() ? "" : "no network");
    menuAdd(m, Act_Log,      "Log",          "");
    menuAdd(m, Act_About,    "About",        "");

    menuAddSpacer(m);
    menuAdd(m, Act_Quit, "Exit", "");
}

static void draw_stores(void)
{
    printf("\n");
    for (size_t i = 0; i < nexusStorageCount(); i++) {
        NexusStorage *s = nexusStorageAt(i);
        if (s == NULL) continue;

        printf("   %-22s %s%-13s" C_RESET,
               store_label(s->storage_id),
               s->present ? C_OK : C_DIM,
               s->present ? "available" : "unavailable");

        if (s->present && s->ops->get_info != NULL) {
            NexusStorageInfo info;
            if (R_SUCCEEDED(s->ops->get_info(s, &info)) && info.capacity_bytes > 0) {
                char cap[24], freesp[24];
                fmt_bytes(cap, sizeof(cap), info.capacity_bytes);
                fmt_bytes(freesp, sizeof(freesp), info.free_bytes);
                printf(C_DIM "%s free of %s" C_RESET, freesp, cap);
            }
        }
        printf("\n");
    }

    printf("\n   " C_DIM "Stores appear on the host once the server is running." C_RESET "\n");
}

static void draw_transfer(void)
{
    const MtpServerState *st = mtpServerGetState();
    char in_s[24], out_s[24], rate_s[24];

    fmt_bytes(in_s,   sizeof(in_s),   st->stats.bytes_in);
    fmt_bytes(out_s,  sizeof(out_s),  st->stats.bytes_out);
    fmt_bytes(rate_s, sizeof(rate_s), st->stats.last_rate_bps);

    printf("\n");
    printf("   Session     %s%s" C_RESET "\n",
           st->session_open ? C_OK : C_DIM,
           st->session_open ? "open" : "closed");
    printf("   Received    " C_VALUE "%s" C_RESET "\n", in_s);
    printf("   Sent        " C_VALUE "%s" C_RESET "\n", out_s);
    printf("   Last rate   " C_VALUE "%s/s" C_RESET "\n", rate_s);
    printf("   Operations  " C_VALUE "%u" C_RESET "\n", st->stats.operations);
    printf("   Errors      %s%u" C_RESET "\n",
           st->stats.errors > 0 ? C_ERR : C_DIM, st->stats.errors);
    printf("   Handles     " C_VALUE "%zu" C_RESET "\n", nexusObjectDbCount());
}

// The log view scrolls independently, so a long install can be read back.
static void draw_log(u32 scroll)
{
    const size_t have = nexusLogGetLineCount();

    printf("\n");
    for (u32 row = 0; row < UI_BODY_ROWS; row++) {
        // Index 0 is the newest line, so the window walks backwards from the
        // scroll position and prints oldest-first within the page.
        const size_t idx = scroll + (UI_BODY_ROWS - 1 - row);
        char line[NEXUS_LOG_LINE_MAX];
        if (nexusLogGetLine(idx, line, sizeof(line))) {
            printf("   " C_DIM "%.*s" C_RESET "\n", UI_WIDTH - 5, line);
        } else {
            printf("\n");
        }
    }

    if (have > UI_BODY_ROWS) {
        printf("   " C_DIM "-- %zu lines, offset %u --" C_RESET "\n", have, scroll);
    }
}

static void draw_about(void)
{
    // Deliberately plain ASCII: the console font is not guaranteed to carry
    // box-drawing or block glyphs, and a broken banner looks worse than none.
    printf("\n");
    printf("   " C_TITLE "  _  ___  __  _  _ ___ __  _ _ ___ " C_RESET "\n");
    printf("   " C_TITLE " | \\| \\ \\/ / | \\| | __|\\ \\/ / | / __|" C_RESET "\n");
    printf("   " C_TITLE " | .` |>  <  | .` | _|  >  <| || \\__ \\" C_RESET "\n");
    printf("   " C_TITLE " |_|\\_/_/\\_\\ |_|\\_|___|/_/\\_\\\\_,_|___/" C_RESET "\n");
    printf("\n");
    printf("   An open-source MTP server and streaming installer.\n\n");

    printf("   Version       " C_VALUE "%s" C_RESET "\n", nexusUpdateVersion());
    printf("   Booted from   " C_VALUE "%s" C_RESET "\n", nexusSysInfoStorageName());
    printf("   Firmware      " C_VALUE "%s" C_RESET "\n",
           nexusSysInfoFirmware()[0] != '\0' ? nexusSysInfoFirmware() : "unknown");
    printf("   Network       %s\n",
           nexusHttpIsReady() ? (nexusHttpHasCaBundle() ? "ready (TLS verified)"
                                                        : "ready (no CA bundle)")
                              : "unavailable");
    printf("\n");
    printf("   Copyright (C) 2026 NX-Nexus contributors.\n");
    printf("   Free software under the GNU GPL, version 3 or later.\n");
    printf("   This program comes with ABSOLUTELY NO WARRANTY.\n\n");
    printf("   " C_DIM "Not affiliated with Nintendo. Contains no keys and no" C_RESET "\n");
    printf("   " C_DIM "copyrighted Nintendo material." C_RESET "\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    // argv[0] is where the launcher found this .nro, which is where a
    // self-update has to write. Installing via the Homebrew App Store puts it
    // somewhere other than the conventional path.
    if (argc > 0 && argv != NULL) nexusUpdateSetSelfPath(argv[0]);

    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    nexusLogInit(NexusLogLevel_Info);

    // Which NAND we booted from decides how several stores label themselves,
    // so this has to happen before the registry is built.
    nexusSysInfoInit();
    nexusCompatInit();

    if (R_FAILED(psmInitialize())) LOG_W("psm: unavailable, battery will report 100");

    int status = EXIT_SUCCESS;

    bool install_ready = true;
    if (R_FAILED(nexusInstallServicesInit())) {
        LOG_W("init: install services unavailable, install stores disabled");
        install_ready = false;
    }

    Result rc = nexusObjectDbInit();
    if (R_FAILED(rc)) {
        LOG_E("init: object db failed (0x%x)", rc);
        status = EXIT_FAILURE;
        goto cleanup_install;
    }

    rc = nexusStorageRegistryInit();
    if (R_FAILED(rc)) {
        LOG_E("init: storage registry failed (0x%x)", rc);
        status = EXIT_FAILURE;
        goto cleanup_objdb;
    }

    // Networking is only used on request, but the config decides TLS policy
    // so it has to be read before any fetch happens.
    if (R_SUCCEEDED(nexusHttpInit())) nexusSourcesLoad(NULL);
    else LOG_W("init: networking unavailable, source and update features are off");

    LOG_I("ready -- choose \"Start MTP server\" to expose the console over USB");

    {
        Screen screen = Screen_Main;
        Menu   menu;
        Menu   sub;                 // whichever sub-screen list is showing
        u32    log_scroll = 0;

        NexusMaintenanceReport report;
        bool scanned = false;
        memset(&report, 0, sizeof(report));
        menuReset(&sub, UI_BODY_ROWS);

        build_main_menu(&menu);

        while (appletMainLoop()) {
            padUpdate(&pad);
            const u64 down = padGetButtonsDown(&pad);

            if (down & HidNpadButton_Plus) break;

            if (screen == Screen_Main) {
                if (down & (HidNpadButton_Up   | HidNpadButton_StickLUp))   menuMove(&menu, -1);
                if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) menuMove(&menu, 1);
                if (down & HidNpadButton_L) menuPage(&menu, -1);
                if (down & HidNpadButton_R) menuPage(&menu, 1);

                if (down & HidNpadButton_A) {
                    switch (menuSelectedId(&menu)) {
                        case Act_ToggleServer:
                            if (g_server_up) server_stop();
                            else             server_start();
                            build_main_menu(&menu);
                            break;
                        case Act_Stores:   screen = Screen_Stores;   break;
                        case Act_Transfer: screen = Screen_Transfer; break;
                        case Act_Log:      screen = Screen_Log; log_scroll = 0; break;
                        case Act_About:    screen = Screen_About;    break;
                        case Act_Quit:     screen = Screen_Quit;     break;

                        case Act_Sources:
                            g_action_msg[0] = 0;
                            build_sources_menu(&sub);
                            screen = Screen_Sources;
                            break;

                        case Act_Maintenance:
                            scanned = false;
                            g_action_msg[0] = 0;
                            build_maintenance_menu(&sub);
                            screen = Screen_Maintenance;
                            break;

                        case Act_Update:
                            g_action_msg[0] = 0;
                            build_update_menu(&sub);
                            screen = Screen_Update;
                            break;

                        case Act_Firmware:
                            g_action_msg[0] = 0;
                            g_fw_armed = false;
                            build_firmware_menu(&sub);
                            screen = Screen_Firmware;
                            break;

                        case Act_Local:
                            g_action_msg[0] = 0;
                            consoleClear();
                            draw_header("Install");
                            printf("\n   Scanning...\n");
                            consoleUpdate(NULL);
                            rescan_local();
                            build_local_menu(&sub);
                            screen = Screen_Local;
                            break;

                        case Act_Titles:
                            g_action_msg[0] = 0;
                            consoleClear();
                            draw_header("Installed titles");
                            printf("\n   Reading the title database...\n");
                            consoleUpdate(NULL);
                            rebuild_titles();
                            build_titles_menu(&sub);
                            screen = Screen_Titles;
                            break;

                        case Act_Mods:
                            g_action_msg[0] = 0;
                            rescan_mods();
                            build_mods_menu(&sub);
                            screen = Screen_Mods;
                            break;

                        case Act_VerifyAll:
                            nexusVerifyEverything(&g_verify, verify_tick, &pad);
                            g_verify_done = true;
                            screen = Screen_Verify;
                            break;

                        default: break;
                    }
                }
            } else if (screen == Screen_Sources || screen == Screen_Items
                       || screen == Screen_Maintenance || screen == Screen_Update
                       || screen == Screen_Firmware || screen == Screen_Local
                       || screen == Screen_Titles || screen == Screen_Mods) {

                if (down & (HidNpadButton_Up   | HidNpadButton_StickLUp))   menuMove(&sub, -1);
                if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) menuMove(&sub, 1);
                if (down & HidNpadButton_L) menuPage(&sub, -1);
                if (down & HidNpadButton_R) menuPage(&sub, 1);

                if (down & HidNpadButton_B) {
                    // The item list steps back to the source list; everything
                    // else returns to the main menu.
                    if (screen == Screen_Items) {
                        g_action_msg[0] = 0;
                        build_sources_menu(&sub);
                        screen = Screen_Sources;
                    } else {
                        screen = Screen_Main;
                        build_main_menu(&menu);
                    }
                }

                // Sorting and filtering live on the titles screen only. A
                // change of either invalidates the armed deletion, since the
                // row under the cursor is about to be a different title.
                if (screen == Screen_Titles && g_titles != NULL) {
                    if (down & HidNpadButton_Y) {
                        g_title_sort = (u8)((g_title_sort + 1) % 3);
                        nexusTitleListSort(g_titles, g_title_sort);
                        g_title_armed = -1;
                        build_titles_menu(&sub);
                    }
                    if (down & HidNpadButton_Minus) {
                        g_title_filter = (u8)((g_title_filter + 1) % 4);
                        g_title_armed = -1;
                        build_titles_menu(&sub);
                    }
                }

                // Deleting is two presses of X on the same row: the first arms
                // it and says so in the row itself, the second carries it out.
                if (down & HidNpadButton_X) {
                    const int id = menuSelectedId(&sub);

                    if (screen == Screen_Titles && g_titles != NULL
                        && id >= Act_Title_Base) {
                        const int idx = id - Act_Title_Base;

                        if (idx != g_title_armed) {
                            g_title_armed = idx;
                            snprintf(g_action_msg, sizeof(g_action_msg),
                                     "press X again to delete -- this cannot be undone");
                        } else {
                            const NexusTitleInfo *t = &g_titles->items[idx];
                            char name[64];
                            snprintf(name, sizeof(name), "%.60s", t->name);

                            consoleClear();
                            draw_header("Deleting");
                            printf("\n   %s\n", name);
                            consoleUpdate(NULL);

                            const Result dr = nexusTitleListDelete(t);
                            snprintf(g_action_msg, sizeof(g_action_msg), "%.50s: %s",
                                     name, R_SUCCEEDED(dr) ? "deleted" : "delete failed");

                            rebuild_titles();
                            build_titles_menu(&sub);
                        }
                    } else if (screen == Screen_Mods && g_mods != NULL
                               && id >= Act_Mod_Base) {
                        const int idx = id - Act_Mod_Base;

                        if (idx != g_mod_armed) {
                            g_mod_armed = idx;
                            snprintf(g_action_msg, sizeof(g_action_msg),
                                     "press X again to delete this mod folder");
                        } else {
                            const Result dr = nexusModsDelete(&g_mods->entries[idx]);
                            snprintf(g_action_msg, sizeof(g_action_msg), "%s",
                                     R_SUCCEEDED(dr) ? "deleted" : "delete failed");
                            rescan_mods();
                            build_mods_menu(&sub);
                        }
                    }
                }

                if (down & HidNpadButton_A) {
                    const int id = menuSelectedId(&sub);

                    if (id >= Act_Mod_Base) {
                        const u32 idx = (u32)(id - Act_Mod_Base);
                        if (g_mods != NULL && idx < g_mods->count) {
                            NexusModEntry *e = &g_mods->entries[idx];
                            const bool want = !e->enabled;

                            if (R_SUCCEEDED(nexusModsSetEnabled(e, want))) {
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "%.40s %s", e->name,
                                         want ? "enabled" : "disabled");
                            } else {
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "could not rename the folder");
                            }
                            g_mod_armed = -1;
                            build_mods_menu(&sub);
                        }
                    } else if (id >= Act_Title_Base) {
                        const u32 idx = (u32)(id - Act_Title_Base);
                        if (g_titles != NULL && idx < g_titles->count) {
                            const NexusTitleInfo *t = &g_titles->items[idx];

                            memset(&g_verify, 0, sizeof(g_verify));
                            g_verify.titles_total = 1;
                            nexusVerifyContentMeta(&t->key, t->storage_id, t->name,
                                                   &g_verify, verify_tick, &pad);
                            g_verify.titles_done = 1;
                            g_verify_done = true;
                            g_title_armed = -1;
                            screen = Screen_Verify;
                        }
                    } else if (id >= Act_Local_Base) {
                        const u32 idx = (u32)(id - Act_Local_Base);
                        if (g_local != NULL && idx < g_local->count) {
                            const NexusLocalItem *it = &g_local->items[idx];

                            consoleClear();
                            draw_header("Installing");
                            printf("\n   %.60s\n\n", it->name);
                            printf("   Streaming into ncm. Nothing is copied first.\n");
                            consoleUpdate(NULL);

                            nexusLocalInstall(it, LOCAL_INSTALL_TARGET);

                            // An XCI runs through the gamecard path and keeps
                            // its status there rather than in the local state.
                            const char *st = (it->kind == NexusLocalKind_Xci)
                                ? nexusXciGetState()->status
                                : nexusLocalGetState()->status;

                            snprintf(g_action_msg, sizeof(g_action_msg), "%.40s: %.60s",
                                     it->name, st);
                        }
                    } else if (id >= Act_Item_Base) {
                        const u32 idx = (u32)(id - Act_Item_Base);
                        if (idx < g_item_count) {
                            // Blocking on purpose: the screen says what is
                            // happening, and the bytes go straight from the
                            // socket into ncm with nothing staged on the SD.
                            consoleClear();
                            draw_header("Installing");
                            printf("\n   %s\n\n", g_items[idx].name);
                            printf("   Streaming from the network into ncm...\n");
                            consoleUpdate(NULL);

                            nexusNetInstall(g_items[idx].url, g_items[idx].name,
                                            NET_INSTALL_TARGET);
                            // Bound both halves so neither can push the other
                            // out: the status is the part worth reading, and
                            // the name only identifies which item it was.
                            snprintf(g_action_msg, sizeof(g_action_msg), "%.40s: %.60s",
                                     g_items[idx].name,
                                     nexusNetInstallGetState()->status);
                        }
                    } else if (id >= Act_Source_Base) {
                        const u32 idx = (u32)(id - Act_Source_Base);
                        g_source_index = (int)idx;

                        consoleClear();
                        draw_header("Sources");
                        printf("\n   Fetching index...\n");
                        consoleUpdate(NULL);

                        load_source_items(idx);
                        build_items_menu(&sub);
                        screen = Screen_Items;
                    } else {
                        switch (id) {
                            case Act_Mnt_Scan:
                                nexusMaintenanceScan(&report);
                                scanned = true;
                                snprintf(g_action_msg, sizeof(g_action_msg), "scan complete");
                                break;

                            case Act_Mnt_Placeholders: {
                                u32 n = 0;
                                nexusMaintenanceCleanPlaceholders(&n);
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "cleared %u placeholder(s)", n);
                                nexusMaintenanceScan(&report);
                                scanned = true;
                                break;
                            }

                            case Act_Mnt_Orphans:
                                if (R_SUCCEEDED(nexusMaintenanceCleanOrphans(NULL))) {
                                    snprintf(g_action_msg, sizeof(g_action_msg),
                                             "removed redundant title data");
                                } else {
                                    snprintf(g_action_msg, sizeof(g_action_msg),
                                             "nothing to remove, or not permitted");
                                }
                                nexusMaintenanceScan(&report);
                                scanned = true;
                                break;

                            case Act_Loc_Rescan:
                                rescan_local();
                                build_local_menu(&sub);
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "%u item(s) found",
                                         g_local != NULL ? g_local->count : 0);
                                break;

                            case Act_Gamecard: {
                                NexusXciInfo card;
                                if (R_FAILED(nexusXciInspectGameCard(&card))
                                    || !card.valid) {
                                    snprintf(g_action_msg, sizeof(g_action_msg),
                                             "%.100s", card.problem[0] != '\0'
                                                 ? card.problem : "no gamecard");
                                    break;
                                }

                                consoleClear();
                                draw_header("Installing");
                                printf("\n   Gamecard -- %u content(s)\n\n",
                                       card.content_count);
                                printf("   Reading the secure partition straight into"
                                       " ncm.\n");
                                printf("   Leave the card in the slot.\n");
                                consoleUpdate(NULL);

                                nexusXciInstallGameCard(LOCAL_INSTALL_TARGET);
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "gamecard: %.90s",
                                         nexusXciGetState()->status);
                                build_local_menu(&sub);
                                break;
                            }

                            case Act_Upd_Check:
                                consoleClear();
                                draw_header("Update");
                                printf("\n   Checking...\n");
                                consoleUpdate(NULL);
                                nexusUpdateCheck();
                                build_update_menu(&sub);
                                break;

                            case Act_Fw_Scan:
                                nexusFirmwareScan(NEXUS_FIRMWARE_DIR, &g_fw_set);
                                g_fw_scanned = true;
                                g_fw_armed   = false;
                                // problem[] is longer than the message slot,
                                // so bound it rather than let it truncate.
                                snprintf(g_action_msg, sizeof(g_action_msg), "%.120s",
                                         g_fw_set.valid ? "firmware set looks usable"
                                                        : g_fw_set.problem);
                                build_firmware_menu(&sub);
                                break;

                            case Act_Fw_Arm:
                                // Arming is its own step so the install can
                                // never be one careless button press away.
                                g_fw_armed = true;
                                snprintf(g_action_msg, sizeof(g_action_msg),
                                         "armed - select again to install");
                                build_firmware_menu(&sub);
                                break;

                            case Act_Fw_Install: {
                                consoleClear();
                                draw_header("Installing firmware");
                                printf("\n   Writing to the %s system partition.\n",
                                       nexusSysInfoStorageName());
                                printf("   DO NOT POWER OFF.\n");
                                consoleUpdate(NULL);

                                const NexusFwResult fr = nexusFirmwareInstall(&g_fw_set);
                                snprintf(g_action_msg, sizeof(g_action_msg), "%s",
                                         nexusFwStr(fr));
                                g_fw_armed = false;
                                build_firmware_menu(&sub);
                                break;
                            }

                            case Act_Upd_Apply:
                                consoleClear();
                                draw_header("Update");
                                printf("\n   Downloading...\n");
                                consoleUpdate(NULL);
                                nexusUpdateApply();
                                build_update_menu(&sub);
                                break;

                            default: break;
                        }
                    }
                }

            } else {
                if (down & HidNpadButton_B) screen = Screen_Main;

                if (screen == Screen_Log) {
                    const size_t have = nexusLogGetLineCount();
                    const u32 max_scroll = (have > UI_BODY_ROWS)
                                         ? (u32)(have - UI_BODY_ROWS) : 0;

                    if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                        if (log_scroll < max_scroll) log_scroll++;
                    }
                    if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                        if (log_scroll > 0) log_scroll--;
                    }
                    if (down & HidNpadButton_L) {
                        log_scroll = (log_scroll + UI_BODY_ROWS < max_scroll)
                                   ? log_scroll + UI_BODY_ROWS : max_scroll;
                    }
                    if (down & HidNpadButton_R) {
                        log_scroll = (log_scroll > UI_BODY_ROWS)
                                   ? log_scroll - UI_BODY_ROWS : 0;
                    }
                }
            }

            if (screen == Screen_Quit) break;

            consoleClear();

            switch (screen) {
                case Screen_Main:
                    draw_header("Main menu");
                    printf("\n");
                    menuDraw(&menu);
                    draw_footer("[A] select   [Up/Down] move   [+] quit");
                    break;

                case Screen_Stores:
                    draw_header("Stores");
                    draw_stores();
                    draw_footer("[B] back   [+] quit");
                    break;

                case Screen_Transfer:
                    draw_header("Transfer");
                    draw_transfer();
                    draw_footer("[B] back   [+] quit");
                    break;

                case Screen_Sources:
                    draw_header("Sources");
                    draw_sources_intro();
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] open   [B] back   [Up/Down] move");
                    break;

                case Screen_Items:
                    draw_header("Install from source");
                    printf("\n");
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] install   [B] back   [Up/Down] move");
                    break;

                case Screen_Maintenance:
                    draw_header("Maintenance");
                    draw_maintenance(&report, scanned);
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] run   [B] back   [Up/Down] move");
                    break;

                case Screen_Update:
                    draw_header("Update");
                    draw_update();
                    printf("\n");
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] run   [B] back   [Up/Down] move");
                    break;

                case Screen_Firmware:
                    draw_header("Firmware");
                    draw_firmware();
                    printf("\n");
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] run   [B] back   [Up/Down] move");
                    break;

                case Screen_Local:
                    draw_header("Install");
                    draw_local();
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] install   [B] back   [Up/Down] move");
                    break;

                case Screen_Titles:
                    draw_header("Installed titles");
                    draw_titles();
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] verify  [X] delete  [Y] sort  [-] filter  [B] back");
                    break;

                case Screen_Verify:
                    draw_header("Verify");
                    draw_verify();
                    draw_footer("[B] back   [+] quit");
                    break;

                case Screen_Mods:
                    draw_header("Mods and cheats");
                    draw_mods();
                    menuDraw(&sub);
                    draw_message_block();
                    draw_footer("[A] enable/disable   [X] delete   [B] back");
                    break;

                case Screen_Log:
                    draw_header("Log");
                    draw_log(log_scroll);
                    draw_footer("[B] back   [Up/Down] scroll   [L/R] page");
                    break;

                case Screen_About:
                    draw_header("About");
                    draw_about();
                    draw_footer("[B] back   [+] quit");
                    break;

                default:
                    break;
            }

            consoleUpdate(NULL);
            svcSleepThread(33000000ull);   // ~30 fps: responsive, not busy-waiting
        }
    }

    server_stop();
    free(g_items);
    nexusHttpExit();
    nexusStorageRegistryExit();

cleanup_objdb:
    nexusObjectDbExit();
cleanup_install:
    if (install_ready) nexusInstallServicesExit();
    psmExit();

    nexusLogExit();
    consoleExit(NULL);
    return status;
}
