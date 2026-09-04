// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- host-side test runner for the container parsers.
//
// These tests build with an ordinary host compiler because the parsers depend
// only on nexus/nx_types.h, never on libnx. Run them with:
//
//   ./scripts/test.sh
//
// or directly:  make -C tests run

#include <stdio.h>

#include "nexus_test.h"

unsigned g_checks       = 0;
unsigned g_failures     = 0;
const char *g_current_test = NULL;

void nexusTestRun(const char *name, void (*fn)(void))
{
    const unsigned before = g_failures;
    g_current_test = name;

    fn();

    const bool passed = (g_failures == before);
    printf("  %s %s\n", passed ? "pass" : "FAIL", name);
}

int nexusTestReport(void)
{
    printf("\n%u checks, %u failure%s\n",
           g_checks, g_failures, (g_failures == 1) ? "" : "s");

    if (g_failures == 0) {
        printf("OK\n");
        return 0;
    }
    return 1;
}

int main(void)
{
    printf("NX-Nexus container parser tests\n\n");

    printf("partition_fs (PFS0 / HFS0):\n");
    test_partition_fs();

    printf("\ncnmt:\n");
    test_cnmt();

    printf("\nticket:\n");
    test_ticket();

    printf("\ninstaller:\n");
    test_installer();

    printf("\nnsp builder:\n");
    test_nsp_builder();

    printf("\njson:\n");
    test_json();

    printf("\nsanitise:\n");
    test_sanitise();

    return nexusTestReport();
}
