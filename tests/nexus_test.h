// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- minimal unit test scaffolding.
//
// Deliberately tiny: no dependencies, no build system beyond a single gcc
// invocation, so the container parsers can be tested anywhere a C compiler
// exists. Checks record a failure and continue rather than aborting, so one
// run reports every problem instead of only the first.
#pragma once

#include <stdio.h>
#include <string.h>

#include "nexus/format.h"

extern unsigned g_checks;
extern unsigned g_failures;
extern const char *g_current_test;

void nexusTestRun(const char *name, void (*fn)(void));
int  nexusTestReport(void);

#define CHECK(cond, ...)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            printf("    FAIL %s:%d  ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

#define CHECK_FMT(actual, expected)                                   \
    do {                                                              \
        NexusFmtResult a_ = (actual);                                 \
        NexusFmtResult e_ = (expected);                               \
        CHECK(a_ == e_, "expected '%s', got '%s'",                    \
              nexusFmtStr(e_), nexusFmtStr(a_));                      \
    } while (0)

#define CHECK_U64(actual, expected)                                   \
    do {                                                              \
        unsigned long long a_ = (unsigned long long)(actual);         \
        unsigned long long e_ = (unsigned long long)(expected);       \
        CHECK(a_ == e_, "expected %llu (0x%llx), got %llu (0x%llx)",  \
              e_, e_, a_, a_);                                        \
    } while (0)

#define CHECK_STR(actual, expected)                                   \
    do {                                                              \
        const char *a_ = (actual);                                    \
        const char *e_ = (expected);                                  \
        CHECK(a_ != NULL && strcmp(a_, e_) == 0,                      \
              "expected \"%s\", got \"%s\"", e_, a_ ? a_ : "(null)"); \
    } while (0)

// Test suites, each defined in its own translation unit.
void test_partition_fs(void);
void test_cnmt(void);
void test_ticket(void);
void test_installer(void);
void test_nsp_builder(void);
void test_json(void);
void test_sanitise(void);
