// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- streaming NSP installer.
//
// The orchestrator here is deliberately free of any libnx dependency: it drives
// an abstract backend, so the whole install sequence -- including every failure
// and rollback path -- can be unit-tested on the host with a mock. The real
// backend (src/installer/install_horizon.c) is a thin shell over ncm, es and ns.
//
// Install sequence, given an NSP arriving as a byte stream:
//
//   1. Read the PFS0 header and classify every entry by filename.
//   2. As bytes arrive, route each file: NCAs stream straight into ncm
//      placeholders, tickets and certificates are small enough to buffer.
//   3. Commit each NCA as its last byte lands (placeholder -> registered).
//   4. Import the ticket, if the title needs one.
//   5. Read the .cnmt back out of the now-registered Meta NCA -- Horizon
//      decrypts it, which is why no keys are needed here.
//   6. Write the content meta record and push an application record, without
//      which the title installs but never appears on the HOME menu.
//
// Nothing is staged to the SD card: content bytes go from the USB buffer into
// an ncm placeholder directly.
#pragma once

#include "nexus/format.h"
#include "nexus/partition_fs.h"
#include "nexus/cnmt.h"
#include "nexus/ticket.h"

#define NEXUS_CONTENT_ID_SIZE 0x10

// Caps. Real NSPs sit far below all of these; they exist so a malformed
// container cannot drive an unbounded allocation or loop.
//
// These are kept deliberately modest because NexusInstaller embeds them: the
// struct is heap-allocated on the console, and the CNMT buffer in
// nexusInstallFinish is a stack local, which a 64 KiB array would overflow on
// a default thread stack. For scale, a real NSP header is a few hundred bytes,
// a ticket is 0x400, and a CNMT is well under 1 KiB.
#define NEXUS_INSTALL_MAX_CONTENTS    64
#define NEXUS_INSTALL_MAX_HEADER      0x4000    // 16 KiB, ~600 files
#define NEXUS_INSTALL_MAX_TICKET      0x1000    // 4 KiB
#define NEXUS_INSTALL_MAX_CERT        0x2000    // 8 KiB
#define NEXUS_INSTALL_MAX_CNMT        0x4000    // 16 KiB
#define NEXUS_INSTALL_MAX_EXT_HEADER  0x100

typedef enum {
    NexusInstall_Ok = 0,
    NexusInstall_InProgress,      // feed accepted, more bytes expected
    NexusInstall_BadContainer,    // not a usable PFS0
    NexusInstall_HeaderTooLarge,
    NexusInstall_NoMetaNca,       // no *.cnmt.nca entry
    NexusInstall_TooManyContents,
    NexusInstall_BadContentId,    // filename is not 32 hex characters
    NexusInstall_BadCnmt,
    NexusInstall_MissingContent,  // CNMT lists content the NSP did not carry
    NexusInstall_BufferTooSmall,
    NexusInstall_BackendError,
    NexusInstall_NoDecompressor,  // an .ncz arrived with no decoder attached
    NexusInstall_BadNcz,
    NexusInstall_Aborted,
    NexusInstall_NotFinished,     // finish called before the stream ended
} NexusInstallResult;

const char *nexusInstallStr(NexusInstallResult r);

// Target storage, mirroring NcmStorageId without depending on libnx.
typedef enum {
    NexusInstallTarget_SdCard      = 5,
    NexusInstallTarget_BuiltInUser = 4,
} NexusInstallTarget;

/// One content entry destined for the ncm meta record.
typedef struct {
    u8  content_id[NEXUS_CONTENT_ID_SIZE];
    u64 size;
    u8  attr;
    u8  content_type;   // CnmtContentType
    u8  id_offset;
} NexusInstallContent;

/// Everything needed to register the title, assembled from the CNMT plus the
/// Meta NCA's own details (which the CNMT does not list for itself).
typedef struct {
    u64 title_id;
    u32 version;
    u8  meta_type;      // CnmtMetaType
    u8  storage_id;     // NexusInstallTarget

    u16 ext_header_size;
    u8  ext_header[NEXUS_INSTALL_MAX_EXT_HEADER];

    u32                 content_count;
    NexusInstallContent contents[NEXUS_INSTALL_MAX_CONTENTS];

    bool has_ticket;
    u8   rights_id[TICKET_RIGHTS_ID_SIZE];
} NexusInstallMeta;

// ---------------------------------------------------------------------------
// Backend interface
//
// Every hook returns 0 for success and non-zero for failure; the orchestrator
// treats any non-zero as NexusInstall_BackendError and rolls back. Keeping the
// return type a plain int is what lets the mock backend build on the host.
// ---------------------------------------------------------------------------
typedef struct {
    /// Opens a placeholder of exactly `size` bytes for the given content id.
    int (*content_begin)(void *user, const u8 id[NEXUS_CONTENT_ID_SIZE], u64 size);

    /// Appends to the open placeholder. Called with whatever chunk sizes the
    /// USB layer happens to deliver.
    int (*content_write)(void *user, const void *data, size_t len);

    /// Registers the completed placeholder as real content.
    int (*content_commit)(void *user);

    /// Drops the open placeholder without registering it.
    void (*content_discard)(void *user);

    /// Reads the .cnmt out of an already-registered Meta NCA. The backend
    /// mounts it via fsOpenFileSystemWithId(FsFileSystemType_ContentMeta),
    /// letting Horizon do the decryption.
    int (*read_cnmt)(void *user, const u8 meta_id[NEXUS_CONTENT_ID_SIZE],
                     void *out, size_t cap, size_t *out_len);

    /// Hands the raw ticket and certificate chain to es.
    int (*import_ticket)(void *user, const void *tik, size_t tik_len,
                         const void *cert, size_t cert_len);

    /// Writes the ncm content meta record and commits it.
    int (*register_meta)(void *user, const NexusInstallMeta *meta);

    /// Pushes the application record so the title becomes visible.
    int (*push_record)(void *user, const NexusInstallMeta *meta);

    /// Best-effort cleanup after a failure. Must tolerate being called when
    /// only part of the install happened.
    void (*rollback)(void *user, const NexusInstallMeta *meta);
} NexusInstallBackendOps;

// ---------------------------------------------------------------------------
// Orchestrator
// ---------------------------------------------------------------------------

typedef enum {
    NexusInstallStage_Idle = 0,
    NexusInstallStage_Magic,      // buffering the first 16 header bytes
    NexusInstallStage_Header,     // buffering the rest of the PFS0 header
    NexusInstallStage_Streaming,  // routing file data
    NexusInstallStage_Complete,
    NexusInstallStage_Failed,
} NexusInstallStage;

// Per-entry classification, derived from the filename.
typedef enum {
    NexusEntryKind_Ignore = 0,
    NexusEntryKind_Nca,
    NexusEntryKind_MetaNca,
    NexusEntryKind_Ticket,
    NexusEntryKind_Cert,

    // The same two, compressed. An NSZ is an NSP whose NCAs have been replaced
    // by NCZs; everything else about the container is identical.
    NexusEntryKind_Ncz,
    NexusEntryKind_MetaNcz,
} NexusEntryKind;

/// True for any kind that ends up as registered content.
#define NEXUS_KIND_IS_CONTENT(k)                                       \
    ((k) == NexusEntryKind_Nca  || (k) == NexusEntryKind_MetaNca ||    \
     (k) == NexusEntryKind_Ncz  || (k) == NexusEntryKind_MetaNcz)

#define NEXUS_KIND_IS_COMPRESSED(k)                                    \
    ((k) == NexusEntryKind_Ncz  || (k) == NexusEntryKind_MetaNcz)

#define NEXUS_KIND_IS_META(k)                                          \
    ((k) == NexusEntryKind_MetaNca || (k) == NexusEntryKind_MetaNcz)

// ---------------------------------------------------------------------------
// Decompression hook
//
// Kept behind an interface for the same reason the backend is: zstd and AES
// live on the console side, while the sequencing stays here where a host test
// can drive it with a mock. The decoder never emits a byte before it knows the
// reconstructed size, which is what lets the placeholder be opened lazily with
// the *decompressed* length rather than the compressed one.
// ---------------------------------------------------------------------------

/// Receives reconstructed NCA bytes. Returns 0 on success.
typedef int (*NexusNczSink)(void *user, const void *data, size_t len);

typedef struct {
    /// Starts one NCZ. Everything the decoder emits goes to sink.
    int (*begin)(void *user, NexusNczSink sink, void *sink_user);

    /// Feeds compressed bytes from the container.
    int (*feed)(void *user, const void *data, size_t len);

    /// Ends the entry. Non-zero means the NCZ did not decode cleanly.
    int (*end)(void *user);

    /// Size of the NCA being rebuilt, or 0 until the header has been read.
    u64 (*size)(void *user);
} NexusNczOps;

typedef struct {
    u32 pfs_index;
    u8  kind;                                  // NexusEntryKind
    u8  content_id[NEXUS_CONTENT_ID_SIZE];
    u64 abs_offset;                            // from the start of the NSP
    u64 size;
} NexusInstallEntry;

typedef struct {
    const NexusInstallBackendOps *ops;
    void *user;

    NexusInstallStage stage;
    u8  target_storage;

    // PFS0 header staging.
    u8     header[NEXUS_INSTALL_MAX_HEADER];
    size_t header_have;
    u64    header_size;
    PartitionFsContext pfs;

    // Entries ordered by offset, which is the order they arrive in.
    NexusInstallEntry entries[NEXUS_INSTALL_MAX_CONTENTS];
    u32               entry_count;
    u32               entry_cursor;      // entry currently being streamed
    bool              content_open;      // a placeholder is open

    // Small files buffered whole.
    u8     tik[NEXUS_INSTALL_MAX_TICKET];
    size_t tik_len;
    u8     cert[NEXUS_INSTALL_MAX_CERT];
    size_t cert_len;

    u8 meta_content_id[NEXUS_CONTENT_ID_SIZE];
    bool has_meta_nca;
    u64  meta_nca_size;

    // Decompression, when the container is an NSZ.
    const NexusNczOps *ncz;
    void              *ncz_user;
    bool               ncz_active;

    u64 stream_pos;      // bytes consumed from the NSP so far
    u64 bytes_written;   // content bytes handed to the backend

    NexusInstallMeta meta;
} NexusInstaller;

/// Resets the installer and prepares it for a new NSP stream.
NexusInstallResult nexusInstallBegin(NexusInstaller *ins,
                                     const NexusInstallBackendOps *ops, void *user,
                                     u8 target_storage);

/// Attaches a decompressor, enabling .ncz entries. Without one, an NSZ fails
/// with NexusInstall_NoDecompressor rather than installing something broken.
/// Call after nexusInstallBegin.
void nexusInstallSetDecompressor(NexusInstaller *ins, const NexusNczOps *ops, void *user);

/// Feeds the next chunk of the NSP. Chunk boundaries are arbitrary -- the
/// orchestrator reassembles headers and splits file data as needed.
/// Returns NexusInstall_InProgress while more data is expected.
NexusInstallResult nexusInstallFeed(NexusInstaller *ins, const void *data, size_t len);

/// Completes the install: ticket import, CNMT readback, meta registration and
/// the application record push.
NexusInstallResult nexusInstallFinish(NexusInstaller *ins);

/// Abandons an install and asks the backend to clean up.
void nexusInstallAbort(NexusInstaller *ins);

const NexusInstallMeta *nexusInstallGetMeta(const NexusInstaller *ins);
u64 nexusInstallGetBytesWritten(const NexusInstaller *ins);

/// Parses a 32-character hex content id, as found in an NCA filename.
/// Returns false when the name is not exactly 32 hex digits.
bool nexusInstallParseContentId(const char *hex, size_t hex_len,
                                u8 out[NEXUS_CONTENT_ID_SIZE]);

/// Classifies a PFS0 entry name. Exposed for tests.
NexusEntryKind nexusInstallClassify(const char *name, u8 out_id[NEXUS_CONTENT_ID_SIZE]);
