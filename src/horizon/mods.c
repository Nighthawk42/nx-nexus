// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- LayeredFS mods and cheat files.

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nexus/mods.h"
#include "nexus/format.h"
#include "nexus/log.h"

// Directory names are exactly 16 hex characters, which is also how Atmosphere
// decides whether to look inside one.
static bool parse_title_id(const char *name, u64 *out)
{
    u64 value = 0;

    for (int i = 0; i < 16; i++) {
        const char c = name[i];
        u64 digit;

        if      (c >= '0' && c <= '9') digit = (u64)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (u64)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (u64)(c - 'A' + 10);
        else return false;

        value = (value << 4) | digit;
    }

    // Anything after the 16 digits, other than our own disabled marker, means
    // this is not a title directory.
    if (name[16] != '\0' && strcmp(name + 16, NEXUS_MODS_SUFFIX) != 0) return false;

    *out = value;
    return true;
}

static bool dir_exists(const char *base, const char *child)
{
    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path), "%.*s/%.16s", (int)(sizeof(path) - 24), base, child);

    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static u32 count_cheat_files(const char *base)
{
    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path), "%.*s/cheats", (int)(sizeof(path) - 16), base);

    DIR *d = opendir(path);
    if (d == NULL) return 0;

    u32 count = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        count++;
    }

    closedir(d);
    return count;
}

static void resolve_name(u64 title_id, char *out, size_t out_size)
{
    NsApplicationControlData *data =
        (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));

    if (data != NULL) {
        u64 actual = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                    title_id, data, sizeof(*data), &actual))
            && actual >= sizeof(data->nacp)) {
            NacpLanguageEntry *entry = NULL;
            if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry))
                && entry != NULL && entry->name[0] != '\0') {
                nexusSanitiseUtf8(entry->name, out, out_size);
                free(data);
                if (out[0] != '\0') return;
                out[0] = '\0';
            }
        }
        free(data);
    }

    // A mod folder for a title that is not installed is perfectly normal --
    // people prepare them ahead of time. The id is still useful.
    snprintf(out, out_size, "%016llx", (unsigned long long)title_id);
}

Result nexusModsScan(NexusModList *out)
{
    if (out == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(out, 0, sizeof(*out));

    DIR *d = opendir(NEXUS_MODS_DIR);
    if (d == NULL) {
        LOG_I("mods: %s does not exist -- nothing installed", NEXUS_MODS_DIR);
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        if (out->count >= NEXUS_MODS_MAX) { out->truncated = true; break; }

        u64 title_id = 0;
        if (!parse_title_id(ent->d_name, &title_id)) continue;

        char full[FS_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%.32s", NEXUS_MODS_DIR, ent->d_name);

        struct stat sb;
        if (stat(full, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;

        NexusModEntry *e = &out->entries[out->count];
        memset(e, 0, sizeof(*e));

        e->title_id = title_id;
        snprintf(e->dir, sizeof(e->dir), "%.60s", ent->d_name);
        e->enabled     = (strlen(ent->d_name) == 16);
        e->has_exefs   = dir_exists(full, "exefs");
        e->has_romfs   = dir_exists(full, "romfs");
        e->cheat_files = count_cheat_files(full);
        e->has_cheats  = e->cheat_files > 0;

        // A directory with none of the three is not a mod; it is more likely a
        // stray folder, and listing it would only invite deleting the wrong one.
        if (!e->has_exefs && !e->has_romfs && !e->has_cheats) continue;

        resolve_name(title_id, e->name, sizeof(e->name));
        out->count++;
    }

    closedir(d);

    LOG_I("mods: %u title(s) with content in %s", out->count, NEXUS_MODS_DIR);
    return 0;
}

Result nexusModsSetEnabled(NexusModEntry *entry, bool enabled)
{
    if (entry == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (entry->enabled == enabled) return 0;

    char from[FS_MAX_PATH], to[FS_MAX_PATH];
    snprintf(from, sizeof(from), "%s/%.60s", NEXUS_MODS_DIR, entry->dir);

    char target[80];
    if (enabled) {
        snprintf(target, sizeof(target), "%016llx", (unsigned long long)entry->title_id);
    } else {
        snprintf(target, sizeof(target), "%016llx%s",
                 (unsigned long long)entry->title_id, NEXUS_MODS_SUFFIX);
    }
    snprintf(to, sizeof(to), "%s/%.70s", NEXUS_MODS_DIR, target);

    if (rename(from, to) != 0) {
        LOG_E("mods: could not rename %s to %s", from, to);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    snprintf(entry->dir, sizeof(entry->dir), "%.60s", target);
    entry->enabled = enabled;

    LOG_I("mods: %016llx %s", (unsigned long long)entry->title_id,
          enabled ? "enabled" : "disabled");
    return 0;
}

// Depth-limited recursive delete. The bound exists because this walks
// user-supplied directory trees and a symlink loop or a pathological layout
// should stop rather than recurse until the stack gives out.
#define MAX_DEPTH 8

static bool remove_tree(const char *path, int depth)
{
    if (depth > MAX_DEPTH) {
        LOG_E("mods: refusing to recurse past depth %d at %s", MAX_DEPTH, path);
        return false;
    }

    DIR *d = opendir(path);
    if (d == NULL) return remove(path) == 0;

    bool ok = true;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char child[FS_MAX_PATH];
        snprintf(child, sizeof(child), "%.*s/%.128s",
                 (int)(sizeof(child) - 136), path, ent->d_name);

        struct stat sb;
        if (stat(child, &sb) != 0) { ok = false; continue; }

        if (S_ISDIR(sb.st_mode)) {
            if (!remove_tree(child, depth + 1)) ok = false;
        } else if (remove(child) != 0) {
            ok = false;
        }
    }

    closedir(d);

    if (rmdir(path) != 0) ok = false;
    return ok;
}

Result nexusModsDelete(const NexusModEntry *entry)
{
    if (entry == NULL) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%.60s", NEXUS_MODS_DIR, entry->dir);

    LOG_W("mods: deleting %s", path);

    if (!remove_tree(path, 0)) {
        LOG_E("mods: could not fully delete %s", path);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    LOG_I("mods: deleted %016llx", (unsigned long long)entry->title_id);
    return 0;
}
