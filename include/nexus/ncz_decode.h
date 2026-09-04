// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- NCZ decompression, on the console.
//
// Rebuilds an NCA from an NCZ as it streams past: copy the first 0x4000 bytes
// through, zstd-decompress the rest, and re-encrypt each section with the AES
// key and counter the NCZ header carries for it.
//
// No key material of our own is involved. The keys are in the file, put there
// by the format so third-party installers could do exactly this.
#pragma once

#include <switch.h>
#include <stdbool.h>

#include "nexus/installer.h"
#include "nexus/ncz.h"

typedef struct NexusNczDecoder NexusNczDecoder;

/// Allocates a decoder. Holds a zstd stream context and a working buffer of a
/// megabyte or so, hence the heap.
NexusNczDecoder *nexusNczDecoderCreate(void);

void nexusNczDecoderDestroy(NexusNczDecoder *d);

/// The ops table to hand to nexusInstallSetDecompressor.
const NexusNczOps *nexusNczDecoderOps(void);

/// Human-readable reason the last entry failed, or "" when it did not.
const char *nexusNczDecoderProblem(const NexusNczDecoder *d);
