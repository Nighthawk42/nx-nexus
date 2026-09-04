// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- ticket parsing.
//
// A ticket carries the encrypted title key for content that needs one. The
// install path hands the raw ticket and certificate blobs to esImportTicket
// wholesale, so this parser exists to validate the blob and pull out the
// rights id and key type for display and sanity checks -- not to decrypt
// anything. NX-Nexus holds no keys.
//
// Layout:
//
//   0x0   signature data   size depends on the signature type
//   Y     ticket data      0x2C0 bytes
//
// Verified against https://switchbrew.org/wiki/Ticket.
#pragma once

#include "nexus/format.h"

#define TICKET_DATA_SIZE      0x2C0u
#define TICKET_RIGHTS_ID_SIZE 0x10u
#define TICKET_TITLE_KEY_BLOCK_SIZE 0x100u

typedef enum {
    TicketSigType_Rsa4096Sha1   = 0x010000,
    TicketSigType_Rsa2048Sha1   = 0x010001,
    TicketSigType_EcdsaSha1     = 0x010002,
    TicketSigType_Rsa4096Sha256 = 0x010003,
    TicketSigType_Rsa2048Sha256 = 0x010004,
    TicketSigType_EcdsaSha256   = 0x010005,
    TicketSigType_HmacSha1160   = 0x010006,
} TicketSigType;

// Title key block interpretation. "Common" keys are a plain 16-byte AES key;
// "personalised" keys are an RSA-2048 message that only the original console
// can unwrap, which makes them useless to install elsewhere.
typedef enum {
    TicketKeyType_Common       = 0,
    TicketKeyType_Personalised = 1,
} TicketKeyType;

typedef struct {
    const u8 *data;
    size_t    len;

    u32    sig_type;
    size_t sig_data_size;   // offset at which the ticket data begins
    size_t total_size;      // sig_data_size + TICKET_DATA_SIZE

    u8  format_version;     // always 2 for Switch tickets
    u8  key_type;           // TicketKeyType
    u16 ticket_version;
    u8  license_type;
    u8  master_key_revision;
    u16 properties;
    u64 ticket_id;
    u64 device_id;
    u8  rights_id[TICKET_RIGHTS_ID_SIZE];
    u32 account_id;
} TicketContext;

/// Parses a ticket blob. Borrows the buffer rather than copying it.
NexusFmtResult ticketInit(TicketContext *ctx, const void *data, size_t len);

/// Size of the signature-data section for a signature type, which is also the
/// offset of the ticket data. Returns NexusFmt_Unsupported for unknown types.
NexusFmtResult ticketGetSigDataSize(u32 sig_type, size_t *out_size);

/// The rights id's first 8 bytes are the title id, big-endian.
u64 ticketGetTitleId(const TicketContext *ctx);

/// The rights id's last byte is the master key revision the title needs.
u8 ticketGetKeyGeneration(const TicketContext *ctx);

/// True when the title key is a plain common key. A personalised ticket is
/// bound to the console that bought the content and cannot be installed
/// elsewhere, which is worth telling the user before a long transfer.
bool ticketIsCommon(const TicketContext *ctx);

/// Formats the rights id as 32 lowercase hex characters. out needs 33 bytes.
void ticketFormatRightsId(const TicketContext *ctx, char *out, size_t out_size);

const char *ticketSigTypeStr(u32 sig_type);
