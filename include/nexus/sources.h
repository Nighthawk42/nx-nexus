// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- user-configured content sources.
//
// POLICY, and it is deliberate: NX-Nexus ships **no** source URLs, has no
// discovery or search, and aggregates nothing. The config file starts empty
// and every source in it is one the user typed themselves.
//
// Fetching your own dumps from your own server is a normal way to move titles
// you already own onto your own console -- it is the same capability as the
// USB install path with a different transport. Public "free shops" that
// distribute other people's games are a different thing entirely: they are not
// supported, not endorsed, and not something this project will help you find.
// Point this at your own server.
#pragma once

#include "nexus/format.h"
#include <stdbool.h>

#define NEXUS_SOURCES_PATH   "sdmc:/switch/nx-nexus/sources.json"

// Where self-update looks by default. The GitHub releases API is understood
// directly, so cutting a release is the whole publishing process -- there is no
// separate manifest to keep in step with it. Users can point update_url at
// their own JSON instead; both shapes are accepted.
#define NEXUS_DEFAULT_UPDATE_URL \
    "https://api.github.com/repos/Nighthawk42/nx-nexus/releases/latest"

#define NEXUS_SOURCE_MAX          8
#define NEXUS_SOURCE_NAME_LEN     48
#define NEXUS_SOURCE_URL_LEN      512
#define NEXUS_SOURCE_ITEMS_MAX    2048
#define NEXUS_SOURCE_ITEM_NAME_LEN 128

// The largest index document accepted, so a hostile or broken server cannot
// make the console chew through memory.
#define NEXUS_SOURCE_INDEX_MAX  (2 * 1024 * 1024)

typedef struct {
    char name[NEXUS_SOURCE_NAME_LEN];
    char url[NEXUS_SOURCE_URL_LEN];
} NexusSource;

typedef struct {
    char name[NEXUS_SOURCE_ITEM_NAME_LEN];   // display/file name
    char url[NEXUS_SOURCE_URL_LEN];
    u64  size;                               // 0 when the index does not say
} NexusSourceItem;

typedef struct {
    NexusSource sources[NEXUS_SOURCE_MAX];
    u32         count;

    bool insecure;                       // user opted out of TLS verification
    char update_url[NEXUS_SOURCE_URL_LEN];  // for self-update; defaults below
} NexusSourcesConfig;

/// Loads sources.json. A missing file is not an error: it writes a commented
/// template (with the policy above in it) and reports zero sources.
Result nexusSourcesLoad(NexusSourcesConfig *cfg);

const NexusSourcesConfig *nexusSourcesGet(void);

/// Parses an index document into items. Kept separate from the fetch so it can
/// be unit-tested on the host against untrusted-looking input.
///
/// Accepts the common shapes: a top-level {"files":[...]} array, or a bare
/// [...] array. Each entry may be an object with "url"/"size"/"name", or a
/// plain URL string.
NexusFmtResult nexusSourcesParseIndex(const char *json, size_t len,
                                      NexusSourceItem *items, u32 max_items,
                                      u32 *out_count);

/// Derives a display name from a URL when the index gives none: the last path
/// component, percent-decoded and sanitised.
void nexusSourcesNameFromUrl(const char *url, char *out, size_t out_size);
