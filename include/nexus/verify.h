// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- integrity checking for installed content.
//
// The CNMT records a SHA-256 for every NCA a title is made of, and that hash is
// over the NCA exactly as it sits on disk -- encrypted. So the whole of an
// install can be verified byte for byte without any key material: read each
// registered NCA back out of ncm, hash it, compare.
//
// This is the cheapest way to answer the question that actually matters when a
// game will not launch: is the content damaged, or is something else wrong?
// Failing SD cards are common and they corrupt silently.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define NEXUS_VERIFY_MAX_ISSUES 64

typedef enum {
    NexusVerifyIssue_Corrupt = 0,   // hash mismatch: the bytes on disk are wrong
    NexusVerifyIssue_Missing,       // the CNMT lists content that is not registered
    NexusVerifyIssue_Unreadable,    // ncm refused to read it back
    NexusVerifyIssue_NoManifest,    // the meta NCA could not be read
} NexusVerifyIssueKind;

const char *nexusVerifyIssueStr(u8 kind);

typedef struct {
    char title[128];
    char content[36];   // content id as hex, or "(meta)"
    u8   kind;          // NexusVerifyIssueKind
} NexusVerifyIssue;

typedef struct {
    u32 titles_total;
    u32 titles_done;
    u32 contents_ok;
    u32 contents_bad;

    u64 bytes_total;    // of the title being checked, summed as we go
    u64 bytes_done;

    char current[128];  // title being hashed right now

    NexusVerifyIssue issues[NEXUS_VERIFY_MAX_ISSUES];
    u32              issue_count;
    bool             issues_truncated;

    bool cancelled;
} NexusVerifyReport;

/// Called periodically while hashing. Return false to cancel.
/// This is how the UI stays responsive during a check that can run for minutes.
typedef bool (*NexusVerifyTick)(void *user, const NexusVerifyReport *report);

/// Verifies one installed content meta.
Result nexusVerifyContentMeta(const NcmContentMetaKey *key, u8 storage_id,
                              const char *title_name, NexusVerifyReport *report,
                              NexusVerifyTick tick, void *tick_user);

/// Verifies everything installed on the SD card and on internal user storage.
/// Resets the report before starting.
Result nexusVerifyEverything(NexusVerifyReport *report,
                             NexusVerifyTick tick, void *tick_user);
