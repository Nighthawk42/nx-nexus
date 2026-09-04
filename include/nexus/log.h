// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- tiny logger. Writes to sdmc:/switch/nx-nexus/nx-nexus.log and
// keeps the last few lines in a ring buffer so the UI can display them.
#pragma once

#include <switch.h>
#include <stdarg.h>

#define NEXUS_LOG_RING_LINES 64
#define NEXUS_LOG_LINE_MAX   192

typedef enum {
    NexusLogLevel_Error = 0,
    NexusLogLevel_Warn  = 1,
    NexusLogLevel_Info  = 2,
    NexusLogLevel_Debug = 3,
    NexusLogLevel_Trace = 4,
} NexusLogLevel;

/// Opens the log file and clears the ring buffer. Never fails hard -- if the
/// file cannot be opened, logging degrades to the ring buffer only.
void nexusLogInit(NexusLogLevel level);

void nexusLogExit(void);

void nexusLogSetLevel(NexusLogLevel level);

/// Writes one line. Prefer the macros below.
void nexusLogWrite(NexusLogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/// Copies the most recent line into out (NUL terminated). index 0 is the newest.
/// Returns false when index is beyond the number of buffered lines.
bool nexusLogGetLine(size_t index, char *out, size_t out_size);

/// Number of lines currently buffered (up to NEXUS_LOG_RING_LINES).
size_t nexusLogGetLineCount(void);

#define LOG_E(...) nexusLogWrite(NexusLogLevel_Error, __VA_ARGS__)
#define LOG_W(...) nexusLogWrite(NexusLogLevel_Warn,  __VA_ARGS__)
#define LOG_I(...) nexusLogWrite(NexusLogLevel_Info,  __VA_ARGS__)
#define LOG_D(...) nexusLogWrite(NexusLogLevel_Debug, __VA_ARGS__)
#define LOG_T(...) nexusLogWrite(NexusLogLevel_Trace, __VA_ARGS__)
