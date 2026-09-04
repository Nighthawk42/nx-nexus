// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- ticket parsing.

#include <string.h>

#include "nexus/ticket.h"

// Ticket data field offsets, relative to the start of the ticket data section.
#define TD_ISSUER              0x000  // 0x40
#define TD_TITLE_KEY_BLOCK     0x040  // 0x100
#define TD_FORMAT_VERSION      0x140
#define TD_KEY_TYPE            0x141
#define TD_TICKET_VERSION      0x142
#define TD_LICENSE_TYPE        0x144
#define TD_MASTER_KEY_REVISION 0x145
#define TD_PROPERTIES          0x146
#define TD_TICKET_ID           0x150
#define TD_DEVICE_ID           0x158
#define TD_RIGHTS_ID           0x160
#define TD_ACCOUNT_ID          0x170

NexusFmtResult ticketGetSigDataSize(u32 sig_type, size_t *out_size)
{
    if (out_size == NULL) return NexusFmt_Truncated;

    // Signature data is 4 bytes of type, the signature itself, then padding to
    // a 0x40 boundary. The padding sizes come from the switchbrew table.
    size_t sig, pad;
    switch (sig_type) {
        case TicketSigType_Rsa4096Sha1:
        case TicketSigType_Rsa4096Sha256: sig = 0x200; pad = 0x3C; break;
        case TicketSigType_Rsa2048Sha1:
        case TicketSigType_Rsa2048Sha256: sig = 0x100; pad = 0x3C; break;
        case TicketSigType_EcdsaSha1:
        case TicketSigType_EcdsaSha256:   sig = 0x3C;  pad = 0x40; break;
        case TicketSigType_HmacSha1160:   sig = 0x14;  pad = 0x28; break;
        default:                          return NexusFmt_Unsupported;
    }

    *out_size = 4 + sig + pad;
    return NexusFmt_Ok;
}

NexusFmtResult ticketInit(TicketContext *ctx, const void *data, size_t len)
{
    if (ctx == NULL || data == NULL) return NexusFmt_Truncated;
    if (len < sizeof(u32))           return NexusFmt_Truncated;

    memset(ctx, 0, sizeof(*ctx));

    const u32 sig_type = nexusRdU32(data, 0);

    size_t sig_data_size = 0;
    NexusFmtResult r = ticketGetSigDataSize(sig_type, &sig_data_size);
    if (r != NexusFmt_Ok) return r;

    const size_t total = sig_data_size + TICKET_DATA_SIZE;
    if (len < total) return NexusFmt_Truncated;

    const u8 *td = (const u8 *)data + sig_data_size;

    ctx->data          = (const u8 *)data;
    ctx->len           = len;
    ctx->sig_type      = sig_type;
    ctx->sig_data_size = sig_data_size;
    ctx->total_size    = total;

    ctx->format_version      = nexusRdU8(td, TD_FORMAT_VERSION);
    ctx->key_type            = nexusRdU8(td, TD_KEY_TYPE);
    ctx->ticket_version      = nexusRdU16(td, TD_TICKET_VERSION);
    ctx->license_type        = nexusRdU8(td, TD_LICENSE_TYPE);
    ctx->master_key_revision = nexusRdU8(td, TD_MASTER_KEY_REVISION);
    ctx->properties          = nexusRdU16(td, TD_PROPERTIES);
    ctx->ticket_id           = nexusRdU64(td, TD_TICKET_ID);
    ctx->device_id           = nexusRdU64(td, TD_DEVICE_ID);
    ctx->account_id          = nexusRdU32(td, TD_ACCOUNT_ID);

    memcpy(ctx->rights_id, td + TD_RIGHTS_ID, TICKET_RIGHTS_ID_SIZE);

    return NexusFmt_Ok;
}

u64 ticketGetTitleId(const TicketContext *ctx)
{
    if (ctx == NULL) return 0;

    // The rights id is stored big-endian, unlike everything else in these
    // formats, so it cannot go through the little-endian readers.
    u64 id = 0;
    for (size_t i = 0; i < 8; i++) id = (id << 8) | ctx->rights_id[i];
    return id;
}

u8 ticketGetKeyGeneration(const TicketContext *ctx)
{
    return (ctx != NULL) ? ctx->rights_id[TICKET_RIGHTS_ID_SIZE - 1] : 0;
}

bool ticketIsCommon(const TicketContext *ctx)
{
    return ctx != NULL && ctx->key_type == TicketKeyType_Common;
}

void ticketFormatRightsId(const TicketContext *ctx, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    if (out == NULL || out_size == 0) return;

    const size_t needed = (TICKET_RIGHTS_ID_SIZE * 2) + 1;
    if (ctx == NULL || out_size < needed) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0; i < TICKET_RIGHTS_ID_SIZE; i++) {
        out[i * 2]       = hex[(ctx->rights_id[i] >> 4) & 0xF];
        out[(i * 2) + 1] = hex[ctx->rights_id[i] & 0xF];
    }
    out[TICKET_RIGHTS_ID_SIZE * 2] = '\0';
}

const char *ticketSigTypeStr(u32 sig_type)
{
    switch (sig_type) {
        case TicketSigType_Rsa4096Sha1:   return "RSA-4096 SHA-1";
        case TicketSigType_Rsa2048Sha1:   return "RSA-2048 SHA-1";
        case TicketSigType_EcdsaSha1:     return "ECDSA SHA-1";
        case TicketSigType_Rsa4096Sha256: return "RSA-4096 SHA-256";
        case TicketSigType_Rsa2048Sha256: return "RSA-2048 SHA-256";
        case TicketSigType_EcdsaSha256:   return "ECDSA SHA-256";
        case TicketSigType_HmacSha1160:   return "HMAC-SHA1-160";
        default:                          return "unknown";
    }
}
