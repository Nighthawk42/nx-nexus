// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- logger.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nexus/log.h"

#define LOG_DIR  "sdmc:/switch/nx-nexus"
#define LOG_PATH LOG_DIR "/nx-nexus.log"

// Bytes set aside in each line for the timestamp and level prefix. The
// timestamp is padded to 6 digits but is allowed to grow past that after ~11
// days of uptime, so this leaves generous headroom.
#define LOG_PREFIX_RESERVE 32

static FILE          *g_file  = NULL;
static NexusLogLevel  g_level = NexusLogLevel_Info;
static Mutex          g_mutex;

// Ring buffer of the most recent lines, newest at (g_head - 1) mod N.
static char   g_ring[NEXUS_LOG_RING_LINES][NEXUS_LOG_LINE_MAX];
static size_t g_head  = 0;
static size_t g_count = 0;

static const char *level_tag(NexusLogLevel level)
{
    switch (level) {
        case NexusLogLevel_Error: return "E";
        case NexusLogLevel_Warn:  return "W";
        case NexusLogLevel_Info:  return "I";
        case NexusLogLevel_Debug: return "D";
        case NexusLogLevel_Trace: return "T";
        default:                  return "?";
    }
}

void nexusLogInit(NexusLogLevel level)
{
    mutexInit(&g_mutex);
    g_level = level;
    g_head  = 0;
    g_count = 0;

    mkdir(LOG_DIR, 0777);  // fails harmlessly if it already exists
    g_file = fopen(LOG_PATH, "w");
    if (g_file != NULL) {
        // Line buffering keeps the log useful if the app crashes mid-transfer.
        setvbuf(g_file, NULL, _IOLBF, 512);
    }

    nexusLogWrite(NexusLogLevel_Info, "NX-Nexus log opened");
}

void nexusLogExit(void)
{
    mutexLock(&g_mutex);
    if (g_file != NULL) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    mutexUnlock(&g_mutex);
}

void nexusLogSetLevel(NexusLogLevel level)
{
    g_level = level;
}

void nexusLogWrite(NexusLogLevel level, const char *fmt, ...)
{
    if (level > g_level) return;

    // Reserve room for the "[seconds.millis] L " prefix so the composed line
    // never has to be truncated.
    char body[NEXUS_LOG_LINE_MAX - LOG_PREFIX_RESERVE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    // Timestamp is milliseconds since boot -- wall-clock time needs the time
    // service, which is not worth a dependency here.
    const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;

    char line[NEXUS_LOG_LINE_MAX];
    snprintf(line, sizeof(line), "[%6llu.%03llu] %s %s",
             (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000),
             level_tag(level), body);

    mutexLock(&g_mutex);

    memcpy(g_ring[g_head], line, sizeof(g_ring[0]) - 1);
    g_ring[g_head][sizeof(g_ring[0]) - 1] = '\0';
    g_head = (g_head + 1) % NEXUS_LOG_RING_LINES;
    if (g_count < NEXUS_LOG_RING_LINES) g_count++;

    if (g_file != NULL) fprintf(g_file, "%s\n", line);

    mutexUnlock(&g_mutex);
}

bool nexusLogGetLine(size_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return false;

    mutexLock(&g_mutex);
    bool ok = index < g_count;
    if (ok) {
        // index 0 is the newest, which sits one slot behind the head.
        const size_t slot = (g_head + NEXUS_LOG_RING_LINES - 1 - index) % NEXUS_LOG_RING_LINES;
        snprintf(out, out_size, "%s", g_ring[slot]);
    }
    mutexUnlock(&g_mutex);
    return ok;
}

size_t nexusLogGetLineCount(void)
{
    mutexLock(&g_mutex);
    size_t n = g_count;
    mutexUnlock(&g_mutex);
    return n;
}
