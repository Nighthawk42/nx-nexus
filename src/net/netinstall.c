// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- install a title straight from a URL.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexus/netinstall.h"
#include "nexus/install_horizon.h"
#include "nexus/ncz_decode.h"
#include "nexus/http.h"
#include "nexus/log.h"

static NexusNetInstallState g_state;
static volatile bool        g_cancel = false;

// How many times a dropped connection is picked back up before giving in.
// Switch wifi on a large title drops often enough that one attempt is not a
// realistic policy; more than a handful means something is actually wrong.
#define MAX_RESUMES 8

typedef struct {
    NexusInstaller *ins;
    bool            failed;      // the installer rejected the stream
    u64             fed;         // bytes handed to the installer, across all attempts
    u64             resume_base; // byte offset this request started at
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

    // Counted here rather than from curl's progress, because this is the
    // number a resume has to restart from: bytes the installer has actually
    // consumed, not bytes that merely arrived.
    ctx->fed         += len;
    g_state.received  = ctx->fed;
    return true;
}

static void net_progress(void *user, u64 received, u64 total)
{
    SinkCtx *ctx = (SinkCtx *)user;

    (void)received;

    // total is the length of *this* request. After a resume the server reports
    // only the remainder, so the offset has to be added back to keep the
    // reported total meaningful.
    if (total > 0) g_state.total = ctx->resume_base + total;
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

    // Attached unconditionally: an NSZ looks exactly like an NSP until an
    // .ncz entry turns up, and by then it is too late to go and get a decoder.
    NexusNczDecoder *decoder = nexusNczDecoderCreate();
    if (decoder != NULL) {
        nexusInstallSetDecompressor(ins, nexusNczDecoderOps(), decoder);
    } else {
        LOG_W("netinstall: no memory for the decompressor -- NSZ will be refused");
    }

    snprintf(g_state.status, sizeof(g_state.status), "downloading");

    SinkCtx ctx = { .ins = ins, .failed = false, .fed = 0, .resume_base = 0 };
    long status = 0;

    // Retry loop. A dropped connection is picked up with a Range request at the
    // exact byte the installer last consumed, so the installer itself never
    // learns that anything went wrong -- it just sees a continuous stream.
    //
    // What this does NOT survive is leaving the app: the placeholders and the
    // parse state live in memory only. It covers the failure that actually
    // happens, which is wifi dropping partway through a 15 GB title.
    NexusHttpResult hr = NexusHttp_Ok;

    for (u32 attempt = 0; ; attempt++) {
        ctx.resume_base = ctx.fed;

        hr = nexusHttpGetFrom(url, ctx.fed, net_sink, &ctx,
                              net_progress, &ctx, &status);

        // Only a transport failure is worth retrying. An installer rejection,
        // a cancel, a 404 or a server that cannot do ranges are all final.
        if (hr != NexusHttp_Network) break;
        if (ctx.failed || g_cancel)  break;
        if (attempt + 1 >= MAX_RESUMES) {
            LOG_E("netinstall: giving up after %u resume attempts", attempt + 1);
            break;
        }

        // Nothing arrived at all this time round: the connection is not coming
        // back on its own, and hammering it will not help.
        if (ctx.fed == ctx.resume_base && attempt > 0) {
            LOG_E("netinstall: no progress on retry, giving up");
            break;
        }

        LOG_W("netinstall: connection lost at %llu bytes, resuming (attempt %u)",
              (unsigned long long)ctx.fed, attempt + 2);
        snprintf(g_state.status, sizeof(g_state.status),
                 "reconnecting (%u/%u)", attempt + 2, MAX_RESUMES);

        svcSleepThread(2000000000ull);   // 2s; the radio may still be settling
        if (g_cancel) break;

        snprintf(g_state.status, sizeof(g_state.status), "downloading");
    }

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
    nexusNczDecoderDestroy(decoder);
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
