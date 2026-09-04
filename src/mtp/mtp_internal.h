// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- internals shared between the MTP transport loop (mtp_server.c)
// and the operation handlers (mtp_ops.c). Not a public interface.
#pragma once

#include "nexus/mtp_server.h"
#include "nexus/mtp_container.h"
#include "nexus/storage.h"

// How long to wait for the data phase of a transaction once a command has been
// accepted. Generous, because a host writing a multi-gigabyte title can stall
// briefly on its own IO.
#define MTP_DATA_TIMEOUT_NS (30ull * 1000000000ull)

// Mutable server state, owned by mtp_server.c.
extern MtpServerState g_mtp;

// Staging buffer for datasets and file streaming, page aligned for usb:ds.
// Owned by mtp_server.c.
extern u8 *g_mtp_payload;

// ---------------------------------------------------------------------------
// Data-phase primitives, implemented in mtp_server.c
// ---------------------------------------------------------------------------

/// Sends a complete data block (12-byte header plus payload) in one shot.
/// The payload must already sit at g_mtp_payload + MTP_CONTAINER_HEADER_SIZE.
Result mtpSendDataFromPayload(u16 code, u32 txn_id, size_t payload_len);

/// Begins a streamed data block whose total payload length is known up front.
/// first_chunk_len bytes of payload must already sit at
/// g_mtp_payload + MTP_CONTAINER_HEADER_SIZE. Sends the header and that chunk.
Result mtpSendDataStreamBegin(u16 code, u32 txn_id, u64 total_payload_len,
                              size_t first_chunk_len);

/// Sends a further chunk of a streamed data block from g_mtp_payload.
Result mtpSendDataStreamChunk(size_t len);

/// Finishes a streamed data block, emitting a terminating zero-length packet
/// if the total transfer landed on a wMaxPacketSize boundary.
Result mtpSendDataStreamEnd(void);

/// Receives a data block into g_mtp_payload. On success *out_len holds the
/// payload length (header excluded). Fails if the payload exceeds the buffer.
Result mtpRecvDataToPayload(size_t *out_len);

/// Receives a data block, handing each payload chunk to sink as it arrives.
/// Used by SendObject so a multi-gigabyte upload never has to be buffered.
typedef Result (*MtpDataSink)(void *user, const void *data, size_t size);
Result mtpRecvDataStreamed(MtpDataSink sink, void *user, u64 *out_total);

/// Sends a response block with 0..MTP_MAX_PARAMS parameters.
Result mtpSendResponse(u16 code, u32 txn_id, const u32 *params, u32 param_count);

// ---------------------------------------------------------------------------
// Handler entry point, implemented in mtp_ops.c
// ---------------------------------------------------------------------------

/// Handles one parsed command block, including its data and response phases.
/// Returns an error Result only for transport-level failures; protocol-level
/// rejections are reported to the host as a response code.
Result mtpHandleCommand(const MtpContainer *cmd);

// ---------------------------------------------------------------------------
// Path helpers, implemented in mtp_ops.c
// ---------------------------------------------------------------------------

/// Joins a directory path and a child name into out ("/" + "a" -> "/a").
/// Returns false if the result would not fit or name is unsafe.
bool mtpPathJoin(const char *dir, const char *name, char *out, size_t out_size);

/// Rejects names that could escape the store root or break path handling.
bool mtpNameIsSafe(const char *name);
