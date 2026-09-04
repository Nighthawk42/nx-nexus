// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- minimal "es" (nn::es::IETicketService) wrapper.
//
// libnx ships no es.h, so the IPC is hand-rolled here. Only the two commands
// the installer needs are implemented; command numbers come from
// https://switchbrew.org/wiki/ETicket_services.
//
// Importing a ticket does not require any key material on our side: the raw
// ticket and certificate blobs from the NSP are handed to the system service
// untouched, and Horizon does the rest.
#pragma once

#include <switch.h>

/// Opens a session with es. Reference counted; safe to nest.
Result nexusEsInitialize(void);

void nexusEsExit(void);

Service *nexusEsGetServiceSession(void);

/// es command 1: ImportTicket.
/// Both blobs come straight out of the NSP: the .tik file and the .cert chain.
Result nexusEsImportTicket(const void *tik, size_t tik_size,
                           const void *cert, size_t cert_size);

/// es command 2: ImportTicketCertificateSet. Imports a certificate chain on its
/// own; rarely needed, since ImportTicket takes the chain alongside the ticket.
Result nexusEsImportTicketCertificateSet(const void *cert, size_t cert_size);

/// es command 22: GetCommonTicketAndCertificateSize [4.0.0+].
/// Reports how much buffer the ticket and its certificate chain need, so an
/// extracted NSP can carry the ticket the title was installed with.
Result nexusEsGetCommonTicketAndCertificateSize(const void *rights_id, size_t rights_id_size,
                                                u64 *out_ticket_size, u64 *out_cert_size);

/// es command 23: GetCommonTicketAndCertificateData [4.0.0+].
Result nexusEsGetCommonTicketAndCertificateData(const void *rights_id, size_t rights_id_size,
                                                void *tik_buf, size_t tik_buf_size,
                                                void *cert_buf, size_t cert_buf_size,
                                                u64 *out_ticket_size, u64 *out_cert_size);

/// es command 3: DeleteTicket. Used to roll back a failed install so a
/// half-installed title does not leave a stray ticket behind.
Result nexusEsDeleteTicket(const void *rights_id, size_t rights_id_size);
