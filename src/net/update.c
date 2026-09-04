// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- self-update.
//
// Checks a user-configured JSON manifest for a newer build and replaces the
// running .nro. The new file is downloaded to a scratch name and only moved
// into place once it has arrived complete and looks like an NRO -- a truncated
// download that overwrote the running app would leave an unbootable homebrew
// entry with no obvious way to fix it from the console.
//
// The manifest is deliberately simple:
//   { "version": "0.2.0", "url": "https://.../NX-Nexus.nro", "notes": "..." }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nexus/update.h"
#include "nexus/json.h"
#include "nexus/http.h"
#include "nexus/sources.h"
#include "nexus/log.h"

#define MANIFEST_MAX 8192

static NexusUpdateState g_state;

const char *nexusUpdateVersion(void) { return NEXUS_VERSION; }

const NexusUpdateState *nexusUpdateGetState(void) { return &g_state; }

// Compares dotted numeric versions. Returns >0 when a is newer than b.
static int version_cmp(const char *a, const char *b)
{
    while (*a != '\0' || *b != '\0') {
        long va = 0, vb = 0;

        while (*a >= '0' && *a <= '9') va = (va * 10) + (*a++ - '0');
        while (*b >= '0' && *b <= '9') vb = (vb * 10) + (*b++ - '0');

        if (va != vb) return (va > vb) ? 1 : -1;

        // Skip one separator on each side; anything else ends the comparison.
        if (*a == '.') a++; else if (*a != '\0') break;
        if (*b == '.') b++; else if (*b != '\0') break;
    }
    return 0;
}

Result nexusUpdateCheck(void)
{
    memset(&g_state, 0, sizeof(g_state));
    snprintf(g_state.installed, sizeof(g_state.installed), "%s", NEXUS_VERSION);

    const NexusSourcesConfig *cfg = nexusSourcesGet();
    if (cfg->update_url[0] == '\0') {
        snprintf(g_state.status, sizeof(g_state.status),
                 "no update_url set in sources.json");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    if (!nexusHttpIsReady()) {
        snprintf(g_state.status, sizeof(g_state.status), "networking is not up");
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    char manifest[MANIFEST_MAX];
    size_t len = 0;
    long status = 0;

    const NexusHttpResult hr = nexusHttpGetBuffer(cfg->update_url, manifest,
                                                  sizeof(manifest) - 1, &len, &status);
    if (hr != NexusHttp_Ok) {
        LOG_E("update: manifest fetch failed -- %s", nexusHttpStr(hr));
        snprintf(g_state.status, sizeof(g_state.status), "%s", nexusHttpStr(hr));
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    manifest[len] = '\0';

    JsonDoc *doc = (JsonDoc *)malloc(sizeof(JsonDoc));
    if (doc == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    if (jsonParse(doc, manifest, len) != NexusFmt_Ok) {
        free(doc);
        snprintf(g_state.status, sizeof(g_state.status), "manifest is not valid JSON");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const u32 root = jsonRoot(doc);
    jsonGetString(doc, jsonObjectGet(doc, root, "version"),
                  g_state.available, sizeof(g_state.available));
    jsonGetString(doc, jsonObjectGet(doc, root, "url"),
                  g_state.url, sizeof(g_state.url));
    jsonGetString(doc, jsonObjectGet(doc, root, "notes"),
                  g_state.notes, sizeof(g_state.notes));
    free(doc);

    if (g_state.available[0] == '\0' || g_state.url[0] == '\0') {
        snprintf(g_state.status, sizeof(g_state.status), "manifest is missing version or url");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    g_state.checked   = true;
    g_state.available_is_newer = version_cmp(g_state.available, NEXUS_VERSION) > 0;

    snprintf(g_state.status, sizeof(g_state.status), "%s",
             g_state.available_is_newer ? "update available" : "up to date");

    LOG_I("update: installed %s, available %s -- %s",
          NEXUS_VERSION, g_state.available, g_state.status);
    return 0;
}

typedef struct {
    FILE  *f;
    size_t written;
    bool   failed;
} DownloadCtx;

static bool download_sink(void *user, const void *data, size_t len)
{
    DownloadCtx *ctx = (DownloadCtx *)user;

    if (fwrite(data, 1, len, ctx->f) != len) {
        ctx->failed = true;
        return false;
    }
    ctx->written += len;
    return true;
}

static void update_progress(void *user, u64 received, u64 total)
{
    (void)user;
    g_state.received = received;
    g_state.total    = total;
}

Result nexusUpdateApply(void)
{
    if (!g_state.checked || !g_state.available_is_newer) {
        snprintf(g_state.status, sizeof(g_state.status), "nothing to install");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // Download beside the target rather than over it.
    const char *tmp   = NEXUS_UPDATE_PATH ".new";
    const char *final = NEXUS_UPDATE_PATH;

    remove(tmp);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        snprintf(g_state.status, sizeof(g_state.status), "cannot write to the SD card");
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    DownloadCtx ctx = { .f = f, .written = 0, .failed = false };
    snprintf(g_state.status, sizeof(g_state.status), "downloading");

    const NexusHttpResult hr = nexusHttpGet(g_state.url, download_sink, &ctx,
                                            update_progress, NULL, NULL);
    fclose(f);

    if (hr != NexusHttp_Ok || ctx.failed || ctx.written == 0) {
        remove(tmp);
        LOG_E("update: download failed -- %s", nexusHttpStr(hr));
        snprintf(g_state.status, sizeof(g_state.status), "download failed");
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    // Sanity check before replacing a working binary: an NRO carries "NRO0" at
    // offset 0x10. An HTML error page saved as .nro would brick the entry.
    char magic[4] = {0};
    f = fopen(tmp, "rb");
    if (f == NULL || fseek(f, 0x10, SEEK_SET) != 0 || fread(magic, 1, 4, f) != 4
        || memcmp(magic, "NRO0", 4) != 0) {
        if (f != NULL) fclose(f);
        remove(tmp);
        LOG_E("update: downloaded file is not an NRO -- refusing to install it");
        snprintf(g_state.status, sizeof(g_state.status), "downloaded file is not an NRO");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    fclose(f);

    // Keep the previous build so a bad update can be undone by hand.
    remove(NEXUS_UPDATE_PATH ".bak");
    rename(final, NEXUS_UPDATE_PATH ".bak");

    if (rename(tmp, final) != 0) {
        // Put the old one back rather than leaving nothing in place.
        rename(NEXUS_UPDATE_PATH ".bak", final);
        remove(tmp);
        snprintf(g_state.status, sizeof(g_state.status), "could not replace the .nro");
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    LOG_I("update: installed %s (%zu bytes); restart NX-Nexus to run it",
          g_state.available, ctx.written);
    snprintf(g_state.status, sizeof(g_state.status), "installed -- restart to apply");
    g_state.applied = true;
    return 0;
}
