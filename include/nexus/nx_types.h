// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- fixed-width types for code that must build both for the console
// and for the host test harness.
//
// The container parsers (PFS0, CNMT, tickets) are deliberately free of any
// libnx dependency so they can be compiled and unit-tested with an ordinary
// host compiler. Including this header rather than <switch.h> is what keeps
// them portable.
#pragma once

#ifdef __SWITCH__

#include <switch.h>

#else  // host build

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#endif
