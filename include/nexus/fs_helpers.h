// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- read-only traversal over a mounted FsFileSystem.
//
// The gamecard and save stores both expose a set of lazily-mounted
// filesystems behind one MTP store, so the per-file work is shared here rather
// than written twice. These wrap the raw fs* calls instead of going through a
// devoptab mount, which avoids fighting over devoptab names at runtime.
#pragma once

#include <switch.h>
#include <stdbool.h>

#include "nexus/storage.h"

/// Lists a directory, invoking cb once per entry.
Result nexusFsEnumerate(FsFileSystem *fs, const char *path,
                        NexusEnumCallback cb, void *user);

/// Stats one path. "/" is always reported as a directory.
Result nexusFsStat(FsFileSystem *fs, const char *path, bool *out_is_dir, s64 *out_size);

/// Reads a byte range from a file.
Result nexusFsRead(FsFileSystem *fs, const char *path, u64 offset,
                   void *buffer, size_t size, size_t *out_read);

/// Splits a store-relative path into its first component and the remainder.
/// "/secure/foo.nca" -> head "secure", tail "/foo.nca"
/// "/secure"         -> head "secure", tail "/"
/// "/"               -> head "",       tail "/"
/// Returns false if the path does not start with '/' or a component overflows.
bool nexusFsSplitPath(const char *path, char *head, size_t head_size,
                      char *tail, size_t tail_size);
