// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP responder state machine.
#pragma once

#include <switch.h>
#include <stdbool.h>
#include "nexus/mtp_container.h"
#include "nexus/storage.h"

// Payload staging buffer. Must be a multiple of USB_XFER_ALIGN. Datasets
// (device info, object handle lists) are built here before being sent, and
// SendObjectInfo datasets are received here.
#define MTP_PAYLOAD_BUF_SIZE (1u * 1024u * 1024u)

typedef struct {
    u64  bytes_in;         // host -> console, this session
    u64  bytes_out;        // console -> host
    u64  last_rate_bps;    // most recent measured transfer rate
    u32  operations;       // completed operations
    u32  errors;
} MtpServerStats;

typedef struct {
    bool session_open;
    u32  session_id;
    u32  transaction_id;

    // Pending SendObjectInfo -> SendObject handoff. MTP splits an upload into
    // two operations: the host describes the object, then sends the bytes.
    bool pending_send;
    u32  pending_storage_id;
    u32  pending_parent;
    u64  pending_size;
    u32  pending_handle;
    char pending_path[NEXUS_PATH_MAX];

    MtpServerStats stats;
} MtpServerState;

/// Allocates buffers and resets state. Call after usbTransportInit() and
/// nexusStorageRegistryInit().
Result mtpServerInit(void);

void mtpServerExit(void);

/// Processes exactly one MTP transaction: waits for a command block, dispatches
/// it, and sends the data and response blocks. Returns after one transaction,
/// or on timeout with no command pending.
///
/// timeout_ns bounds only the initial wait for a command; once a transaction is
/// under way the data phase uses its own longer timeout.
Result mtpServerRunOnce(u64 timeout_ns);

/// Read-only view of server state, for the UI.
const MtpServerState *mtpServerGetState(void);

/// Resets session state. Called on USB detach so a reconnecting host starts
/// from a clean slate.
void mtpServerResetSession(void);
