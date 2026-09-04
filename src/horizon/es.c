// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- "es" service wrapper.

#include <string.h>

#include "nexus/es.h"
#include "nexus/log.h"

static Service g_es_srv;
static u64     g_es_refcnt = 0;
static Mutex   g_es_lock;   // zero-initialised, which is an unlocked Mutex

Result nexusEsInitialize(void)
{
    mutexLock(&g_es_lock);

    Result rc = 0;
    if (g_es_refcnt == 0) {
        rc = smGetService(&g_es_srv, "es");
        if (R_FAILED(rc)) LOG_E("es: smGetService failed (0x%x)", rc);
    }
    if (R_SUCCEEDED(rc)) g_es_refcnt++;

    mutexUnlock(&g_es_lock);
    return rc;
}

void nexusEsExit(void)
{
    mutexLock(&g_es_lock);

    if (g_es_refcnt > 0 && --g_es_refcnt == 0) {
        serviceClose(&g_es_srv);
    }

    mutexUnlock(&g_es_lock);
}

Service *nexusEsGetServiceSession(void)
{
    return &g_es_srv;
}

Result nexusEsImportTicket(const void *tik, size_t tik_size,
                           const void *cert, size_t cert_size)
{
    if (tik == NULL || tik_size == 0 || cert == NULL || cert_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // Command 1 takes two input buffers: the ticket and the certificate chain.
    return serviceDispatch(&g_es_srv, 1,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tik,  tik_size  },
            { cert, cert_size },
        },
    );
}

Result nexusEsImportTicketCertificateSet(const void *cert, size_t cert_size)
{
    if (cert == NULL || cert_size == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    return serviceDispatch(&g_es_srv, 2,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers      = { { cert, cert_size } },
    );
}

Result nexusEsDeleteTicket(const void *rights_id, size_t rights_id_size)
{
    if (rights_id == NULL || rights_id_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    return serviceDispatch(&g_es_srv, 3,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers      = { { rights_id, rights_id_size } },
    );
}

// Commands 22 and 23 share a request shape: the rights id goes in as a
// map-alias buffer, and two u64 sizes come back. The data variant additionally
// takes two output buffers.
typedef struct {
    u64 ticket_size;
    u64 cert_size;
} EsTicketSizes;

Result nexusEsGetCommonTicketAndCertificateSize(const void *rights_id, size_t rights_id_size,
                                                u64 *out_ticket_size, u64 *out_cert_size)
{
    if (rights_id == NULL || rights_id_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (!hosversionAtLeast(4, 0, 0)) {
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }

    EsTicketSizes out = {0};
    Result rc = serviceDispatchOut(&g_es_srv, 22, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers      = { { rights_id, rights_id_size } },
    );

    if (R_SUCCEEDED(rc)) {
        if (out_ticket_size) *out_ticket_size = out.ticket_size;
        if (out_cert_size)   *out_cert_size   = out.cert_size;
    }
    return rc;
}

Result nexusEsGetCommonTicketAndCertificateData(const void *rights_id, size_t rights_id_size,
                                                void *tik_buf, size_t tik_buf_size,
                                                void *cert_buf, size_t cert_buf_size,
                                                u64 *out_ticket_size, u64 *out_cert_size)
{
    if (rights_id == NULL || tik_buf == NULL || cert_buf == NULL) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (!hosversionAtLeast(4, 0, 0)) {
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }

    EsTicketSizes out = {0};
    Result rc = serviceDispatchOut(&g_es_srv, 23, out,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tik_buf,   tik_buf_size  },
            { cert_buf,  cert_buf_size },
            { rights_id, rights_id_size },
        },
    );

    if (R_SUCCEEDED(rc)) {
        if (out_ticket_size) *out_ticket_size = out.ticket_size;
        if (out_cert_size)   *out_cert_size   = out.cert_size;
    }
    return rc;
}
