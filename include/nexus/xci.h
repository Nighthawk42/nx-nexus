// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- XCI (gamecard image) header parsing.
//
// An XCI is a signed 0x200-byte header followed by a root HFS0 that holds the
// card's partitions:
//
//   update/   system update the card shipped with
//   logo/     [9.0.0+] boot logo
//   normal/   plaintext area
//   secure/   the NCAs that make up the game
//
// Everything needed to install lives in "secure", and both the XCI header and
// the HFS0 partitions are plaintext, so this needs no keys -- same as the NSP
// path. Only the NCAs inside are encrypted, and those are never parsed.
//
// Gamecard titles are ticketless: their NCAs use standard crypto rather than a
// title key, so an XCI install imports no ticket at all.
//
// Layout verified against https://switchbrew.org/wiki/XCI.
#pragma once

#include "nexus/format.h"
#include "nexus/partition_fs.h"

#define XCI_HEADER_SIZE     0x200u
#define XCI_MAGIC           0x44414548u   // "HEAD" little-endian
#define XCI_MAGIC_OFFSET    0x100u

// Addresses in the header are counted in media units, not bytes.
#define XCI_MEDIA_UNIT      0x200u

typedef struct {
    u64 root_offset;        // absolute byte offset of the root HFS0
    u64 root_header_size;   // its full header length
    u64 valid_data_end;     // absolute byte offset; a trimmed XCI ends here
    u64 cart_size;          // nominal capacity of the cartridge, in bytes
    u8  header_version;
    u8  flags;
} XciHeader;

/// Parses the 0x200-byte gamecard header. buf must hold at least
/// XCI_HEADER_SIZE bytes.
NexusFmtResult xciParseHeader(const void *buf, size_t len, XciHeader *out);

/// Human-readable cartridge capacity, e.g. "32 GB". Never NULL.
const char *xciCartSizeStr(u64 cart_size);

/// Absolute byte offset of a partition named in the root HFS0.
/// root must have been initialised over the root HFS0's header.
NexusFmtResult xciFindPartition(const XciHeader *hdr, const PartitionFsContext *root,
                                const char *name, u64 *out_offset, u64 *out_size);
