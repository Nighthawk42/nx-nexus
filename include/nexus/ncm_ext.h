// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- shared ncm helpers.
//
// The trick that keeps this project key-free lives here: a Meta NCA is
// encrypted like any other, but once it is registered Horizon will mount it as
// FsFileSystemType_ContentMeta and hand back the plaintext .cnmt inside. The
// installer uses this during an install; verification uses it afterwards.
#pragma once

#include <switch.h>

/// Reads the .cnmt out of a registered Meta NCA.
/// cap should be at least 64 KiB; a CNMT for a title with a large DLC
/// catalogue can be tens of kilobytes.
Result nexusNcmReadCnmt(NcmContentStorage *cs, const NcmContentId *meta_id,
                        void *out, size_t cap, size_t *out_len);
