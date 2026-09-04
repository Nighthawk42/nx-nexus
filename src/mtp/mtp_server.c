// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- MTP transport loop and data-phase primitives.
//
// One MTP transaction is: command block in, optional data block in either
// direction, response block out. This file owns that sequencing and the USB
// framing rules; the operations themselves live in mtp_ops.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "mtp_internal.h"
#include "nexus/usb_transport.h"
#include "nexus/log.h"

MtpServerState g_mtp;
u8            *g_mtp_payload = NULL;

// Command blocks are tiny (at most 32 bytes) but still need a page-aligned
// landing buffer for usb:ds.
static u8 *g_cmd_buf = NULL;
#define CMD_BUF_SIZE 0x1000u

// Tracks an in-flight streamed data block so mtpSendDataStreamEnd() knows
// whether a terminating zero-length packet is required.
static u64 g_stream_sent = 0;

static bool g_initialized = false;

// wMaxPacketSize for the negotiated bus speed. MTP requires a zero-length
// packet to terminate a data phase whose length is an exact multiple of this.
static size_t mtp_max_packet_size(void)
{
    switch (usbTransportGetSpeed()) {
        case UsbDeviceSpeed_Super: return 0x400;
        case UsbDeviceSpeed_High:  return 0x200;
        case UsbDeviceSpeed_Full:  return 0x40;
        default:                   return 0x200;  // assume high speed
    }
}

Result mtpServerInit(void)
{
    if (g_initialized) return 0;

    g_cmd_buf = (u8 *)memalign(USB_XFER_ALIGN, CMD_BUF_SIZE);
    if (g_cmd_buf == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    g_mtp_payload = (u8 *)memalign(USB_XFER_ALIGN, MTP_PAYLOAD_BUF_SIZE);
    if (g_mtp_payload == NULL) {
        free(g_cmd_buf);
        g_cmd_buf = NULL;
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    memset(&g_mtp, 0, sizeof(g_mtp));
    g_initialized = true;
    LOG_I("mtp: server ready (%u KiB payload buffer)", MTP_PAYLOAD_BUF_SIZE / 1024u);
    return 0;
}

void mtpServerExit(void)
{
    if (!g_initialized) return;

    free(g_cmd_buf);
    free(g_mtp_payload);
    g_cmd_buf     = NULL;
    g_mtp_payload = NULL;
    g_initialized = false;
}

const MtpServerState *mtpServerGetState(void)
{
    return &g_mtp;
}

void mtpServerResetSession(void)
{
    // Abandon any half-finished upload so the backend does not keep a stale
    // file handle open across a reconnect.
    if (g_mtp.pending_send) {
        NexusStorage *s = nexusStorageById(g_mtp.pending_storage_id);
        if (s != NULL && s->ops->write_end != NULL) s->ops->write_end(s, false);
    }

    const MtpServerStats stats = g_mtp.stats;  // statistics survive a reconnect
    memset(&g_mtp, 0, sizeof(g_mtp));
    g_mtp.stats = stats;

    nexusObjectDbClear();
    LOG_I("mtp: session reset");
}

// ---------------------------------------------------------------------------
// Data-phase primitives
// ---------------------------------------------------------------------------

// Writes a header into the first 12 bytes of g_mtp_payload. length is the
// on-the-wire total; MTP uses 0xFFFFFFFF when it exceeds 32 bits.
static void payload_write_header(u16 type, u16 code, u32 txn_id, u64 total)
{
    const u32 length = (total > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)total;
    memcpy(g_mtp_payload + 0, &length, 4);
    memcpy(g_mtp_payload + 4, &type,   2);
    memcpy(g_mtp_payload + 6, &code,   2);
    memcpy(g_mtp_payload + 8, &txn_id, 4);
}

Result mtpSendDataFromPayload(u16 code, u32 txn_id, size_t payload_len)
{
    const u64 total = (u64)MTP_CONTAINER_HEADER_SIZE + payload_len;
    payload_write_header(MtpContainerType_Data, code, txn_id, total);

    size_t sent = 0;
    Result rc = usbTransportWrite(g_mtp_payload, (size_t)total, &sent, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) {
        LOG_E("mtp: data send failed (0x%x)", rc);
        return rc;
    }
    g_mtp.stats.bytes_out += sent;

    // A data phase that lands exactly on a packet boundary needs a ZLP so the
    // host knows it is complete.
    const size_t mps = mtp_max_packet_size();
    if (total != 0 && (total % mps) == 0) {
        size_t zero = 0;
        usbTransportWrite(g_mtp_payload, 0, &zero, MTP_DATA_TIMEOUT_NS);
    }
    return 0;
}

Result mtpSendDataStreamBegin(u16 code, u32 txn_id, u64 total_payload_len,
                              size_t first_chunk_len)
{
    const u64 total = (u64)MTP_CONTAINER_HEADER_SIZE + total_payload_len;
    payload_write_header(MtpContainerType_Data, code, txn_id, total);

    const size_t bytes = MTP_CONTAINER_HEADER_SIZE + first_chunk_len;
    size_t sent = 0;
    Result rc = usbTransportWrite(g_mtp_payload, bytes, &sent, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;

    g_mtp.stats.bytes_out += sent;
    g_stream_sent = sent;
    return 0;
}

Result mtpSendDataStreamChunk(size_t len)
{
    if (len == 0) return 0;

    size_t sent = 0;
    Result rc = usbTransportWrite(g_mtp_payload, len, &sent, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;

    g_mtp.stats.bytes_out += sent;
    g_stream_sent += sent;
    return 0;
}

Result mtpSendDataStreamEnd(void)
{
    const size_t mps = mtp_max_packet_size();
    if (g_stream_sent != 0 && (g_stream_sent % mps) == 0) {
        size_t zero = 0;
        usbTransportWrite(g_mtp_payload, 0, &zero, MTP_DATA_TIMEOUT_NS);
    }
    g_stream_sent = 0;
    return 0;
}

Result mtpRecvDataToPayload(size_t *out_len)
{
    if (out_len) *out_len = 0;

    size_t got = 0;
    Result rc = usbTransportRead(g_mtp_payload, MTP_PAYLOAD_BUF_SIZE, &got, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;
    if (got < MTP_CONTAINER_HEADER_SIZE) {
        LOG_E("mtp: short data block (%zu bytes)", got);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    g_mtp.stats.bytes_in += got;

    u32 length = 0;
    u16 type   = 0;
    memcpy(&length, g_mtp_payload + 0, 4);
    memcpy(&type,   g_mtp_payload + 4, 2);
    if (type != MtpContainerType_Data) {
        LOG_E("mtp: expected data block, got type %u", type);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // Everything after the header in this first transfer is payload. Datasets
    // handled this way are small enough to arrive in one transfer; anything
    // larger belongs on the streamed path.
    size_t payload = got - MTP_CONTAINER_HEADER_SIZE;

    if (length != 0xFFFFFFFFu && length > got) {
        // The host declared more than it has sent so far; pull the remainder.
        u64 remaining = (u64)length - got;
        if ((u64)got + remaining > MTP_PAYLOAD_BUF_SIZE) {
            LOG_E("mtp: dataset of %u bytes exceeds payload buffer", length);
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        while (remaining > 0) {
            size_t chunk = 0;
            rc = usbTransportRead(g_mtp_payload + MTP_CONTAINER_HEADER_SIZE + payload,
                                  (size_t)remaining, &chunk, MTP_DATA_TIMEOUT_NS);
            if (R_FAILED(rc)) return rc;
            if (chunk == 0) break;  // host stopped early
            g_mtp.stats.bytes_in += chunk;
            payload   += chunk;
            remaining -= chunk;
        }
    }

    // Hand back only the payload; callers should not see the header.
    memmove(g_mtp_payload, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE, payload);
    if (out_len) *out_len = payload;
    return 0;
}

Result mtpRecvDataStreamed(MtpDataSink sink, void *user, u64 *out_total)
{
    if (out_total) *out_total = 0;

    size_t got = 0;
    Result rc = usbTransportRead(g_mtp_payload, MTP_PAYLOAD_BUF_SIZE, &got, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;
    if (got < MTP_CONTAINER_HEADER_SIZE) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    g_mtp.stats.bytes_in += got;

    u32 length = 0;
    u16 type   = 0;
    memcpy(&length, g_mtp_payload + 0, 4);
    memcpy(&type,   g_mtp_payload + 4, 2);
    if (type != MtpContainerType_Data) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // Payload bytes that shared the first transfer with the header.
    const size_t first = got - MTP_CONTAINER_HEADER_SIZE;
    u64 delivered = 0;

    if (first > 0) {
        rc = sink(user, g_mtp_payload + MTP_CONTAINER_HEADER_SIZE, first);
        if (R_FAILED(rc)) return rc;
        delivered = first;
    }

    // Total payload the host promised, if it told us.
    const bool known_length = (length != 0xFFFFFFFFu && length >= MTP_CONTAINER_HEADER_SIZE);
    const u64  expected     = known_length ? ((u64)length - MTP_CONTAINER_HEADER_SIZE) : UINT64_MAX;

    const u64    start_tick = armGetSystemTick();
    const size_t mps        = mtp_max_packet_size();

    while (delivered < expected) {
        u64 want = expected - delivered;
        if (want > MTP_PAYLOAD_BUF_SIZE) want = MTP_PAYLOAD_BUF_SIZE;

        size_t chunk = 0;
        rc = usbTransportRead(g_mtp_payload, (size_t)want, &chunk, MTP_DATA_TIMEOUT_NS);
        if (R_FAILED(rc)) return rc;
        if (chunk == 0) break;  // zero-length packet: end of data phase

        g_mtp.stats.bytes_in += chunk;
        rc = sink(user, g_mtp_payload, chunk);
        if (R_FAILED(rc)) return rc;
        delivered += chunk;

        // With an unknown total length, a short packet marks the end.
        if (!known_length && (chunk % mps) != 0) break;
    }

    const u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
    if (elapsed_ns > 0) {
        g_mtp.stats.last_rate_bps = (delivered * 1000000000ull) / elapsed_ns;
    }

    if (out_total) *out_total = delivered;
    return 0;
}

Result mtpSendResponse(u16 code, u32 txn_id, const u32 *params, u32 param_count)
{
    MtpContainer resp = {
        .type           = MtpContainerType_Response,
        .code           = code,
        .transaction_id = txn_id,
        .param_count    = param_count,
    };
    for (u32 i = 0; i < param_count && i < MTP_MAX_PARAMS; i++) resp.params[i] = params[i];

    // Serialise into the aligned command buffer -- responses are tiny.
    const size_t len = mtpContainerSerialize(&resp, g_cmd_buf, CMD_BUF_SIZE);
    if (len == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    size_t sent = 0;
    Result rc = usbTransportWrite(g_cmd_buf, len, &sent, MTP_DATA_TIMEOUT_NS);
    if (R_FAILED(rc)) {
        LOG_E("mtp: response send failed (0x%x)", rc);
        return rc;
    }
    g_mtp.stats.bytes_out += sent;

    if (code != MtpRes_OK) g_mtp.stats.errors++;
    return 0;
}

// ---------------------------------------------------------------------------
// Transaction loop
// ---------------------------------------------------------------------------

Result mtpServerRunOnce(u64 timeout_ns)
{
    if (!g_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    size_t got = 0;
    Result rc = usbTransportRead(g_cmd_buf, CMD_BUF_SIZE, &got, timeout_ns);
    if (R_FAILED(rc)) return rc;          // includes the idle timeout case
    if (got == 0) return 0;               // spurious zero-length packet

    MtpContainer cmd;
    if (!mtpContainerParse(g_cmd_buf, got, &cmd)) {
        LOG_W("mtp: unparseable command block (%zu bytes)", got);
        return 0;
    }

    if (cmd.type != MtpContainerType_Command) {
        // Most likely a stray data block from a cancelled transaction. Drop it
        // rather than trying to interpret it as a command.
        LOG_W("mtp: ignoring container type %u outside a transaction", cmd.type);
        return 0;
    }

    g_mtp.transaction_id = cmd.transaction_id;

    rc = mtpHandleCommand(&cmd);
    if (R_SUCCEEDED(rc)) g_mtp.stats.operations++;
    return rc;
}
