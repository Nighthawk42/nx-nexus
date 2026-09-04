// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- HTTP(S) client over libcurl.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "nexus/http.h"
#include "nexus/log.h"

static bool g_initialised   = false;
static bool g_socket_up     = false;
static bool g_curl_up       = false;
static bool g_insecure      = false;
static bool g_have_ca       = false;

const char *nexusHttpStr(NexusHttpResult r)
{
    switch (r) {
        case NexusHttp_Ok:             return "ok";
        case NexusHttp_NotInitialised: return "network not initialised";
        case NexusHttp_BadUrl:         return "bad url";
        case NexusHttp_NoTlsTrust:     return "no CA bundle for HTTPS";
        case NexusHttp_Network:        return "network error";
        case NexusHttp_HttpStatus:     return "server returned an error";
        case NexusHttp_Aborted:        return "aborted";
        case NexusHttp_TooLarge:       return "response too large";
        case NexusHttp_NoRangeSupport: return "server cannot resume";
        default:                       return "unknown";
    }
}

Result nexusHttpInit(void)
{
    if (g_initialised) return 0;

    // A modest socket config: this does a handful of connections, not hundreds.
    const SocketInitConfig cfg = *socketGetDefaultInitConfig();
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        LOG_E("http: socketInitialize failed (0x%x)", rc);
        return rc;
    }
    g_socket_up = true;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        LOG_E("http: curl_global_init failed");
        socketExit();
        g_socket_up = false;
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    g_curl_up = true;

    struct stat sb;
    g_have_ca = (stat(NEXUS_HTTP_CA_BUNDLE, &sb) == 0 && sb.st_size > 0);

    if (g_have_ca) {
        LOG_I("http: ready, TLS verified against %s", NEXUS_HTTP_CA_BUNDLE);
    } else {
        LOG_W("http: ready, but no CA bundle at %s -- HTTPS will be refused",
              NEXUS_HTTP_CA_BUNDLE);
        LOG_W("http: put a cacert.pem there (e.g. from curl.se/ca/cacert.pem)");
    }

    g_initialised = true;
    return 0;
}

void nexusHttpExit(void)
{
    if (!g_initialised) return;

    if (g_curl_up)   { curl_global_cleanup(); g_curl_up = false; }
    if (g_socket_up) { socketExit();          g_socket_up = false; }

    g_initialised = false;
}

bool nexusHttpIsReady(void)      { return g_initialised; }
bool nexusHttpHasCaBundle(void)  { return g_have_ca; }
bool nexusHttpGetInsecure(void)  { return g_insecure; }

void nexusHttpSetInsecure(bool insecure)
{
    g_insecure = insecure;
    if (insecure) {
        LOG_W("http: TLS verification DISABLED by configuration.");
        LOG_W("http: anything on the network path can substitute what you install.");
    }
}

static bool url_is_https(const char *url)
{
    return strncmp(url, "https://", 8) == 0;
}

static bool url_is_supported(const char *url)
{
    return url != NULL
        && (strncmp(url, "http://", 7) == 0 || url_is_https(url));
}

// Applies the shared options: timeouts, redirects and TLS policy.
static NexusHttpResult apply_common(CURL *curl, const char *url)
{
    if (!url_is_supported(url)) return NexusHttp_BadUrl;

    if (url_is_https(url) && !g_have_ca && !g_insecure) {
        LOG_E("http: refusing HTTPS to %.60s -- no CA bundle and insecure mode is off", url);
        return NexusHttp_NoTlsTrust;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NEXUS_HTTP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Connect quickly or give up; a stalled transfer is caught by the low-speed
    // limit rather than a hard total timeout, since a legitimate title download
    // can take a very long time.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    if (g_insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (g_have_ca) curl_easy_setopt(curl, CURLOPT_CAINFO, NEXUS_HTTP_CA_BUNDLE);
    }

    return NexusHttp_Ok;
}

// ---------------------------------------------------------------------------
// Streaming GET
// ---------------------------------------------------------------------------

typedef struct {
    NexusHttpSink sink;
    void         *user;
    u64           received;
    bool          aborted;
} StreamCtx;

static size_t stream_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    StreamCtx *ctx = (StreamCtx *)userdata;
    const size_t total = size * nmemb;

    if (!ctx->sink(ctx->user, ptr, total)) {
        ctx->aborted = true;
        return 0;   // returning short tells curl to abort
    }

    ctx->received += total;
    return total;
}

typedef struct {
    NexusHttpProgress cb;
    void             *user;
} ProgressCtx;

static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;

    ProgressCtx *ctx = (ProgressCtx *)userdata;
    if (ctx->cb != NULL) ctx->cb(ctx->user, (u64)dlnow, (u64)dltotal);
    return 0;
}

NexusHttpResult nexusHttpGetFrom(const char *url, u64 start_offset,
                                 NexusHttpSink sink, void *sink_user,
                                 NexusHttpProgress progress, void *progress_user,
                                 long *status_out)
{
    if (!g_initialised) return NexusHttp_NotInitialised;
    if (sink == NULL)   return NexusHttp_BadUrl;

    CURL *curl = curl_easy_init();
    if (curl == NULL) return NexusHttp_Network;

    NexusHttpResult r = apply_common(curl, url);
    if (r != NexusHttp_Ok) { curl_easy_cleanup(curl); return r; }

    StreamCtx sctx = { .sink = sink, .user = sink_user, .received = 0, .aborted = false };
    ProgressCtx pctx = { .cb = progress, .user = progress_user };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);

    if (start_offset > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)start_offset);
    }

    if (progress != NULL) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);
    }

    const CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status_out != NULL) *status_out = status;

    curl_easy_cleanup(curl);

    if (sctx.aborted) return NexusHttp_Aborted;

    if (code != CURLE_OK) {
        LOG_E("http: transfer failed -- %s", curl_easy_strerror(code));
        return NexusHttp_Network;
    }
    if (status >= 400) {
        LOG_E("http: server returned %ld", status);
        return NexusHttp_HttpStatus;
    }

    // 206 is the only correct answer to a Range request. A server that ignores
    // it and sends 200 has just replayed the whole file from byte zero -- and
    // the sink has already been handed those bytes as though they continued
    // from the offset. Say so instead of pretending the resume worked.
    if (start_offset > 0 && status != 206) {
        LOG_E("http: resume asked for at %llu but the server answered %ld",
              (unsigned long long)start_offset, status);
        return NexusHttp_NoRangeSupport;
    }

    return NexusHttp_Ok;
}

NexusHttpResult nexusHttpGet(const char *url, NexusHttpSink sink, void *sink_user,
                             NexusHttpProgress progress, void *progress_user,
                             long *status_out)
{
    return nexusHttpGetFrom(url, 0, sink, sink_user, progress, progress_user, status_out);
}

// ---------------------------------------------------------------------------
// Buffered GET, for index documents
// ---------------------------------------------------------------------------

typedef struct {
    u8    *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} BufferCtx;

static bool buffer_sink(void *user, const void *data, size_t len)
{
    BufferCtx *ctx = (BufferCtx *)user;

    if (ctx->len + len > ctx->cap) {
        ctx->overflow = true;
        return false;
    }

    memcpy(ctx->buf + ctx->len, data, len);
    ctx->len += len;
    return true;
}

NexusHttpResult nexusHttpGetBuffer(const char *url, void *buffer, size_t capacity,
                                   size_t *out_len, long *status_out)
{
    if (out_len) *out_len = 0;
    if (buffer == NULL || capacity == 0) return NexusHttp_BadUrl;

    BufferCtx ctx = { .buf = (u8 *)buffer, .cap = capacity, .len = 0, .overflow = false };

    const NexusHttpResult r = nexusHttpGet(url, buffer_sink, &ctx, NULL, NULL, status_out);

    // An abort caused by our own overflow should read as "too large", not as a
    // generic abort -- the two need different fixes.
    if (ctx.overflow) return NexusHttp_TooLarge;
    if (r != NexusHttp_Ok) return r;

    if (out_len) *out_len = ctx.len;
    return NexusHttp_Ok;
}

NexusHttpResult nexusHttpHeadSize(const char *url, u64 *out_size)
{
    if (!g_initialised) return NexusHttp_NotInitialised;
    if (out_size) *out_size = 0;

    CURL *curl = curl_easy_init();
    if (curl == NULL) return NexusHttp_Network;

    NexusHttpResult r = apply_common(curl, url);
    if (r != NexusHttp_Ok) { curl_easy_cleanup(curl); return r; }

    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    const CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_off_t len = 0;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) return NexusHttp_Network;
    if (status >= 400)    return NexusHttp_HttpStatus;

    if (out_size && len > 0) *out_size = (u64)len;
    return NexusHttp_Ok;
}
