// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- NCZ (compressed NCA) header parsing.
//
// An NSZ is an ordinary NSP whose big NCAs have been replaced by .ncz files.
// Each NCZ is:
//
//   0x0000  the original NCA's first 0x4000 bytes, byte for byte and still
//           encrypted -- copied through untouched
//   0x4000  "NCZSECTN"               (8 bytes, once)
//   0x4008  section count            (u64)
//   0x4010  section[]                (0x38 bytes each)
//   ...     optional "NCZBLOCK" header, for the seekable variant
//   ...     a zstd stream of the *decrypted* remainder
//
// The part that matters for this project: **each section entry carries its own
// AES key and counter**. The format's author put them there deliberately so
// third-party installers can rebuild the NCA without deriving anything, and it
// is why NX-Nexus can install an NSZ while still holding no key material of
// its own. Compressing an NSZ needs prod.keys; decompressing one does not.
//
// Layout taken from nsz's own IndependentNczDecompressorConcise.py, which is
// the normative description -- the prose in docs/formats.md folds the one-off
// magic into the section entry and reads as though entries were 0x48 bytes.
#pragma once

#include "nexus/format.h"

#define NCZ_HEADER_OFFSET     0x4000u   // where the section table begins
#define NCZ_SECTION_SIZE      0x38u
#define NCZ_MAX_SECTIONS      64u
#define NCZ_KEY_SIZE          0x10u
#define NCZ_MAGIC_SIZE        8u

// Magic + count, which is all that is needed to size the table.
#define NCZ_PREAMBLE_SIZE     16u

// nn::ncm NcaEncryptionType, as the NCZ header records it.
typedef enum {
    NczCrypto_Auto  = 0,
    NczCrypto_None  = 1,   // section is stored plain; copy it through
    NczCrypto_Xts   = 2,   // header crypto; never appears on a body section
    NczCrypto_Ctr   = 3,
    NczCrypto_Bktr  = 4,   // patch RomFS; still AES-CTR at this level
} NczCryptoType;

typedef struct {
    u64 offset;    // absolute within the reconstructed NCA; the first is 0x4000
    u64 size;
    u64 crypto_type;
    u8  key[NCZ_KEY_SIZE];
    u8  counter[NCZ_KEY_SIZE];
} NczSection;

typedef struct {
    u32        section_count;
    NczSection sections[NCZ_MAX_SECTIONS];

    size_t header_bytes;      // from NCZ_HEADER_OFFSET to the zstd stream
    u64    decompressed_size; // the size of the NCA this rebuilds to

    bool block_compressed;    // an NCZBLOCK header was present
} NczContext;

/// Parses the section table. `buf` must start at NCZ_HEADER_OFFSET of the NCZ.
///
/// Returns NexusFmt_Truncated when more bytes are needed -- the caller is
/// streaming and should read more; use nczHeaderSizeFor to size that read.
NexusFmtResult nczParseHeader(NczContext *ctx, const void *buf, size_t len);

/// Total bytes of magic + count + section table, given at least the first
/// NCZ_PREAMBLE_SIZE bytes. Returns 0 when this is not an NCZ header.
size_t nczHeaderSizeFor(const void *preamble, size_t len);

/// Finds the section covering an offset in the reconstructed NCA.
/// Returns NULL past the end.
const NczSection *nczSectionAt(const NczContext *ctx, u64 offset);

/// True when the section's bytes must be re-encrypted rather than copied.
bool nczSectionIsEncrypted(const NczSection *section);

/// Builds the AES-CTR counter for an absolute offset: the section's own
/// counter in the top eight bytes, the AES block index in the bottom eight,
/// big-endian. This is the standard NCA construction, and getting the
/// endianness wrong produces an NCA that decrypts to noise.
void nczBuildCounter(const NczSection *section, u64 offset, u8 out[NCZ_KEY_SIZE]);
