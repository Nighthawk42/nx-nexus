// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install a title straight from a URL.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/netinstall.h"
#include "nexus/install_horizon.h"
#include "nexus/http.h"
#include "nexus/log.h"

static NexusNetInstallState g_state;
static volatile bool        g_cancel = false;

typedef struct {
    NexusInstaller *ins;
    bool            failed;
} SinkCtx;

// Each network chunk goes straight into the installer. Returning false aborts
// the transfer, which is how a cancel or an installer error stops the download
// rather than letting gigabytes keep arriving.
static bool net_sink(void *user, const void *data, size_t len)
{
    SinkCtx *ctx = (SinkCtx *)user;

    if (g_cancel) return false;

    const NexusInstallResult r = nexusInstallFeed(ctx->ins, data, len);
    if (r != NexusInstall_InProgress && r != NexusInstall_Ok) {
        LOG_E("netinstall: %s", nexusInstallStr(r));
        snprintf(g_state.status, sizeof(g_state.status), "failed: %s", nexusInstallStr(r));
        ctx->failed = true;
        return false;
    }

    return true;
}

static void net_progress(void *user, u64 received, u64 total)
{
    (void)user;
    g_state.received = received;
    g_state.total    = total;
}

Result nexusNetInstall(const char *url, const char *display_name, u8 target)
{
    if (url == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (!nexusHttpIsReady()) {
        LOG_E("netinstall: networking is not up");
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    memset(&g_state, 0, sizeof(g_state));
    g_cancel = false;
    g_state.active = true;
    snprintf(g_state.name, sizeof(g_state.name), "%s",
             display_name != NULL ? display_name : url);
    snprintf(g_state.status, sizeof(g_state.status), "connecting");

    NexusHorizonBackend *backend = nexusHorizonBackendCreate(target);
    if (backend == NULL) {
        snprintf(g_state.status, sizeof(g_state.status), "no install target");
        g_state.active = false;
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    // The installer embeds its staging buffers and is far too big for a stack.
    NexusInstaller *ins = (NexusInstaller *)calloc(1, sizeof(NexusInstaller));
    if (ins == NULL) {
        nexusHorizonBackendDestroy(backend);
        g_state.active = false;
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    nexusInstallBegin(ins, nexusHorizonBackendOps(), backend, target);
    snprintf(g_state.status, sizeof(g_state.status), "downloading");

    SinkCtx ctx = { .ins = ins, .failed = false };
    long status = 0;

    const NexusHttpResult hr = nexusHttpGet(url, net_sink, &ctx,
                                            net_progress, NULL, &status);

    Result rc = 0;

    if (ctx.failed) {
        nexusInstallAbort(ins);
        rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    } else if (g_cancel) {
        LOG_W("netinstall: cancelled by the user");
        snprintf(g_state.status, sizeof(g_state.status), "cancelled");
        nexusInstallAbort(ins);
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    } else if (hr != NexusHttp_Ok) {
        LOG_E("netinstall: download failed -- %s (status %ld)", nexusHttpStr(hr), status);
        snprintf(g_state.status, sizeof(g_state.status), "%s", nexusHttpStr(hr));
        nexusInstallAbort(ins);
        rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    } else {
        snprintf(g_state.status, sizeof(g_state.status), "finalising");

        const NexusInstallResult r = nexusInstallFinish(ins);
        if (r != NexusInstall_Ok) {
            LOG_E("netinstall: %s", nexusInstallStr(r));
            snprintf(g_state.status, sizeof(g_state.status), "failed: %s",
                     nexusInstallStr(r));
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        } else {
            const NexusInstallMeta *meta = nexusInstallGetMeta(ins);
            LOG_I("netinstall: installed %016llx v%u (%u contents)",
                  (unsigned long long)meta->title_id, meta->version, meta->content_count);
            snprintf(g_state.status, sizeof(g_state.status), "installed");
        }
    }

    free(ins);
    nexusHorizonBackendDestroy(backend);

    g_state.active = false;
    return rc;
}

const NexusNetInstallState *nexusNetInstallGetState(void)
{
    return &g_state;
}

void nexusNetInstallCancel(void)
{
    g_cancel = true;
}
