// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- HTTP(S) client, over libcurl from devkitPro portlibs.
//
// Downloads stream through a callback rather than into a buffer, because the
// things being fetched are multi-gigabyte titles that go straight into the
// installer.
//
// TLS verification: this code downloads executables that then get installed,
// so an unverified connection is a genuine attack vector -- whoever is between
// you and the server chooses what gets installed. Verification is therefore ON
// by default and needs a CA bundle at sdmc:/switch/nx-nexus/cacert.pem.
// Without one, HTTPS is refused rather than silently downgraded. A user who
// knowingly accepts the risk can set "insecure": true in sources.json.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_HTTP_CA_BUNDLE "sdmc:/switch/nx-nexus/cacert.pem"
#define NEXUS_HTTP_USER_AGENT "NX-Nexus/0.1"

typedef enum {
    NexusHttp_Ok = 0,
    NexusHttp_NotInitialised,
    NexusHttp_BadUrl,
    NexusHttp_NoTlsTrust,     // HTTPS asked for with no CA bundle and no opt-out
    NexusHttp_Network,        // connect/transfer failure
    NexusHttp_HttpStatus,     // server answered with >= 400
    NexusHttp_Aborted,        // the sink asked to stop
    NexusHttp_TooLarge,
    NexusHttp_NoRangeSupport, // resume asked for; server sent the whole body
} NexusHttpResult;

const char *nexusHttpStr(NexusHttpResult r);

/// Receives each chunk as it arrives. Return false to abort the transfer.
typedef bool (*NexusHttpSink)(void *user, const void *data, size_t len);

/// Called periodically during a transfer so the UI can show progress.
/// total is 0 when the server did not report a length.
typedef void (*NexusHttpProgress)(void *user, u64 received, u64 total);

/// Brings up the socket stack and curl. Safe to call more than once.
Result nexusHttpInit(void);

void nexusHttpExit(void);

/// True once networking is up and a connection is plausible.
bool nexusHttpIsReady(void);

/// Allows unverified TLS. Off unless the user opts in, and logged loudly when
/// it is on, because it disables the only thing standing between a download
/// and whoever is on the path.
void nexusHttpSetInsecure(bool insecure);

bool nexusHttpGetInsecure(void);

/// True when a CA bundle is present, so callers can explain *why* HTTPS is
/// unavailable rather than just failing.
bool nexusHttpHasCaBundle(void);

/// Streams a URL into sink. status_out receives the HTTP status when non-NULL.
NexusHttpResult nexusHttpGet(const char *url, NexusHttpSink sink, void *sink_user,
                             NexusHttpProgress progress, void *progress_user,
                             long *status_out);

/// As nexusHttpGet, but asks the server to start at a byte offset.
///
/// The progress callback still reports bytes received *in this request*, so a
/// caller resuming a transfer has to add the offset back on for a total.
///
/// A server that ignores Range answers 200 with the whole body instead of 206.
/// That is detected and reported as NexusHttp_NoRangeSupport rather than
/// silently feeding duplicate bytes into whatever is consuming the stream --
/// which, for an installer, would corrupt the install.
NexusHttpResult nexusHttpGetFrom(const char *url, u64 start_offset,
                                 NexusHttpSink sink, void *sink_user,
                                 NexusHttpProgress progress, void *progress_user,
                                 long *status_out);

/// Fetches a small resource wholly into a buffer -- for index documents, not
/// for titles. Fails with NexusHttp_TooLarge rather than truncating.
NexusHttpResult nexusHttpGetBuffer(const char *url, void *buffer, size_t capacity,
                                   size_t *out_len, long *status_out);

/// Asks for a resource's size without downloading it. Returns 0 when the
/// server declines to say.
NexusHttpResult nexusHttpHeadSize(const char *url, u64 *out_size);
