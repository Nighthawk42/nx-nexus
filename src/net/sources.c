// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- user-configured content sources.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nexus/sources.h"
#include "nexus/json.h"
#include "nexus/http.h"
#include "nexus/log.h"

static NexusSourcesConfig g_cfg;

// Written when no config exists. The policy lives in the file the user edits,
// not only in documentation they may never read.
static const char *k_template =
"{\n"
"  \"_readme\": [\n"
"    \"NX-Nexus content sources. This file starts empty on purpose.\",\n"
"    \"\",\n"
"    \"Add YOUR OWN server here -- somewhere you host your own dumps. That is\",\n"
"    \"a normal way to move titles you already own onto your own console.\",\n"
"    \"\",\n"
"    \"Public 'free shops' that redistribute other people's games are NOT\",\n"
"    \"supported and NOT endorsed. Do not ask for help with them.\",\n"
"    \"\",\n"
"    \"'insecure' disables TLS certificate checking. Leave it false. With it\",\n"
"    \"on, anyone between you and the server chooses what you install.\",\n"
"    \"For HTTPS put a CA bundle at sdmc:/switch/nx-nexus/cacert.pem\",\n"
"    \"(for example from https://curl.se/ca/cacert.pem).\"\n"
"  ],\n"
"\n"
"  \"insecure\": false,\n"
"  \"update_url\": \"" NEXUS_DEFAULT_UPDATE_URL "\",\n"
"\n"
"  \"sources\": [\n"
"    { \"name\": \"Example (edit me)\", \"url\": \"https://your-server.example/index.json\" }\n"
"  ]\n"
"}\n";

static void write_template(void)
{
    mkdir("sdmc:/switch/nx-nexus", 0777);

    FILE *f = fopen(NEXUS_SOURCES_PATH, "w");
    if (f == NULL) {
        LOG_W("sources: could not create %s", NEXUS_SOURCES_PATH);
        return;
    }

    fputs(k_template, f);
    fclose(f);
    LOG_I("sources: wrote a template to %s", NEXUS_SOURCES_PATH);
}

void nexusSourcesNameFromUrl(const char *url, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (url == NULL) return;

    // Take the last path component, stopping before any query string.
    const char *end = strpbrk(url, "?#");
    const char *stop = (end != NULL) ? end : url + strlen(url);

    const char *start = stop;
    while (start > url && start[-1] != '/') start--;

    // Percent-decode into a scratch buffer before sanitising, so an encoded
    // space or accent comes out readable.
    char decoded[NEXUS_SOURCE_ITEM_NAME_LEN * 2];
    size_t w = 0;

    for (const char *p = start; p < stop && w + 1 < sizeof(decoded); p++) {
        if (*p == '%' && p + 2 < stop) {
            char hex[3] = { p[1], p[2], '\0' };
            char *endp = NULL;
            const long v = strtol(hex, &endp, 16);
            if (endp == hex + 2 && v > 0 && v < 256) {
                decoded[w++] = (char)v;
                p += 2;
                continue;
            }
        }
        decoded[w++] = *p;
    }
    decoded[w] = '\0';

    nexusSanitiseUtf8(decoded, out, out_size);
    if (out[0] == '\0') snprintf(out, out_size, "item");
}

// Reads one index entry, which may be an object or a bare URL string.
static bool read_item(const JsonDoc *doc, u32 node, NexusSourceItem *out)
{
    memset(out, 0, sizeof(*out));

    if (jsonTypeOf(doc, node) == JsonType_String) {
        if (!jsonGetString(doc, node, out->url, sizeof(out->url))) return false;
        nexusSourcesNameFromUrl(out->url, out->name, sizeof(out->name));
        return out->url[0] != '\0';
    }

    if (jsonTypeOf(doc, node) != JsonType_Object) return false;

    const u32 url = jsonObjectGet(doc, node, "url");
    if (!jsonGetString(doc, url, out->url, sizeof(out->url))) return false;
    if (out->url[0] == '\0') return false;

    // "name" is optional; fall back to the URL's last component.
    const u32 name = jsonObjectGet(doc, node, "name");
    if (!jsonGetString(doc, name, out->name, sizeof(out->name)) || out->name[0] == '\0') {
        nexusSourcesNameFromUrl(out->url, out->name, sizeof(out->name));
    } else {
        char raw[NEXUS_SOURCE_ITEM_NAME_LEN];
        snprintf(raw, sizeof(raw), "%s", out->name);
        nexusSanitiseUtf8(raw, out->name, sizeof(out->name));
        if (out->name[0] == '\0') nexusSourcesNameFromUrl(out->url, out->name, sizeof(out->name));
    }

    jsonGetU64(doc, jsonObjectGet(doc, node, "size"), &out->size);
    return true;
}

NexusFmtResult nexusSourcesParseIndex(const char *json, size_t len,
                                      NexusSourceItem *items, u32 max_items,
                                      u32 *out_count)
{
    if (out_count) *out_count = 0;
    if (json == NULL || items == NULL || max_items == 0) return NexusFmt_Truncated;

    // JsonDoc is a few hundred KiB of nodes, so it cannot live on the stack.
    JsonDoc *doc = (JsonDoc *)malloc(sizeof(JsonDoc));
    if (doc == NULL) return NexusFmt_TooLarge;

    NexusFmtResult r = jsonParse(doc, json, len);
    if (r != NexusFmt_Ok) { free(doc); return r; }

    const u32 root = jsonRoot(doc);

    // Either {"files":[...]} or a bare array.
    u32 array = 0;
    if (jsonTypeOf(doc, root) == JsonType_Array) {
        array = root;
    } else {
        array = jsonObjectGet(doc, root, "files");
        if (jsonTypeOf(doc, array) != JsonType_Array) {
            free(doc);
            return NexusFmt_Unsupported;
        }
    }

    const u32 total = jsonCount(doc, array);
    u32 kept = 0;

    for (u32 i = 0; i < total && kept < max_items; i++) {
        if (read_item(doc, jsonAt(doc, array, i), &items[kept])) kept++;
    }

    free(doc);

    if (out_count) *out_count = kept;
    return NexusFmt_Ok;
}

Result nexusSourcesLoad(NexusSourcesConfig *cfg)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    struct stat sb;
    if (stat(NEXUS_SOURCES_PATH, &sb) != 0) {
        write_template();
        if (cfg != NULL) *cfg = g_cfg;
        return 0;
    }

    if (sb.st_size <= 0 || sb.st_size > 256 * 1024) {
        LOG_W("sources: %s is an implausible size, ignoring", NEXUS_SOURCES_PATH);
        if (cfg != NULL) *cfg = g_cfg;
        return 0;
    }

    char *text = (char *)malloc((size_t)sb.st_size + 1);
    if (text == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    FILE *f = fopen(NEXUS_SOURCES_PATH, "rb");
    if (f == NULL) { free(text); return MAKERESULT(Module_Libnx, LibnxError_IoError); }

    const size_t got = fread(text, 1, (size_t)sb.st_size, f);
    fclose(f);
    text[got] = '\0';

    JsonDoc *doc = (JsonDoc *)malloc(sizeof(JsonDoc));
    if (doc == NULL) { free(text); return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); }

    if (jsonParse(doc, text, got) != NexusFmt_Ok) {
        LOG_E("sources: %s is not valid JSON -- ignoring it", NEXUS_SOURCES_PATH);
        free(doc);
        free(text);
        if (cfg != NULL) *cfg = g_cfg;
        return 0;
    }

    const u32 root = jsonRoot(doc);

    bool insecure = false;
    if (jsonGetBool(doc, jsonObjectGet(doc, root, "insecure"), &insecure)) {
        g_cfg.insecure = insecure;
    }
    jsonGetString(doc, jsonObjectGet(doc, root, "update_url"),
                  g_cfg.update_url, sizeof(g_cfg.update_url));

    // An older sources.json predates the default, and an empty string here
    // would otherwise disable updates silently.
    if (g_cfg.update_url[0] == '\0') {
        snprintf(g_cfg.update_url, sizeof(g_cfg.update_url), "%s",
                 NEXUS_DEFAULT_UPDATE_URL);
    }

    const u32 arr = jsonObjectGet(doc, root, "sources");
    const u32 n   = jsonCount(doc, arr);

    for (u32 i = 0; i < n && g_cfg.count < NEXUS_SOURCE_MAX; i++) {
        const u32 entry = jsonAt(doc, arr, i);

        NexusSource s;
        memset(&s, 0, sizeof(s));

        if (!jsonGetString(doc, jsonObjectGet(doc, entry, "url"), s.url, sizeof(s.url))) {
            continue;
        }
        if (s.url[0] == '\0') continue;

        char raw[NEXUS_SOURCE_NAME_LEN];
        if (jsonGetString(doc, jsonObjectGet(doc, entry, "name"), raw, sizeof(raw))
            && raw[0] != '\0') {
            nexusSanitiseUtf8(raw, s.name, sizeof(s.name));
        }
        if (s.name[0] == '\0') snprintf(s.name, sizeof(s.name), "Source %u", i + 1);

        g_cfg.sources[g_cfg.count++] = s;
    }

    free(doc);
    free(text);

    if (g_cfg.insecure) nexusHttpSetInsecure(true);

    LOG_I("sources: %u configured%s", g_cfg.count,
          g_cfg.insecure ? " (TLS verification OFF)" : "");

    if (cfg != NULL) *cfg = g_cfg;
    return 0;
}

const NexusSourcesConfig *nexusSourcesGet(void)
{
    return &g_cfg;
}
