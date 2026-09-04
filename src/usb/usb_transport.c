// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- usb:ds bulk transport for the MTP responder.
//
// The descriptor and transfer sequences here follow libnx's own usb_comms
// reference implementation (ISC licensed, switchbrew/libnx), retargeted from
// the vendor-specific class to the USB Still Image class so that host operating
// systems recognise the console as an MTP device.
//
// Known limitation: only the [5.0.0+] usb:ds descriptor API is implemented.
// Pre-5.0.0 firmware uses a different path (usbDsGetDsInterface) and is
// rejected with a clear error rather than silently misbehaving.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "nexus/usb_transport.h"
#include "nexus/log.h"

// USB Still Image class -- PIMA 15740 / PTP, which is what MTP rides on.
#define USB_CLASS_STILL_IMAGE   0x06
#define USB_SUBCLASS_STILL_IMAGE 0x01
#define USB_PROTOCOL_PIMA_15740  0x01

// Reported to the host. 0x057E is Nintendo; the product id is arbitrary and
// deliberately not one of Nintendo's own, so host drivers do not mistake this
// for a retail device.
#define NEXUS_USB_VID 0x057E
#define NEXUS_USB_PID 0x3000

// Set to 1 to build without the interrupt endpoint. MTP events (ObjectAdded
// and friends) then become unavailable, but enumeration only needs the two
// bulk endpoints. See docs/PLAN-REVIEW.md -- the three-endpoint layout still
// needs verification on hardware.
#ifndef NEXUS_USB_NO_INTERRUPT_EP
#define NEXUS_USB_NO_INTERRUPT_EP 0
#endif

#if NEXUS_USB_NO_INTERRUPT_EP
#define NEXUS_USB_NUM_ENDPOINTS 2
#else
#define NEXUS_USB_NUM_ENDPOINTS 3
#endif

static bool             g_initialized = false;
static UsbDsInterface  *g_interface   = NULL;
static UsbDsEndpoint   *g_ep_in       = NULL;  // bulk IN  (console -> host)
static UsbDsEndpoint   *g_ep_out      = NULL;  // bulk OUT (host -> console)
static UsbDsEndpoint   *g_ep_intr     = NULL;  // interrupt IN (events)

// Bounce buffer used when a caller hands us memory that is not page aligned.
static u8 *g_bounce = NULL;
#define BOUNCE_SIZE 0x1000u

static Result nexus_setup_device_descriptors(void)
{
    u8 iManufacturer = 0, iProduct = 0, iSerialNumber = 0;
    static const u16 supported_langs[1] = { 0x0409 };  // en-US

    Result rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs,
                    sizeof(supported_langs) / sizeof(u16));
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iManufacturer, "NX-Nexus");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProduct, "NX-Nexus MTP");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSerialNumber, "0000000000000001");
    if (R_FAILED(rc)) return rc;

    struct usb_device_descriptor device_descriptor = {
        .bLength            = USB_DT_DEVICE_SIZE,
        .bDescriptorType    = USB_DT_DEVICE,
        .bcdUSB             = 0x0110,
        .bDeviceClass       = 0x00,   // class is declared per-interface
        .bDeviceSubClass    = 0x00,
        .bDeviceProtocol    = 0x00,
        .bMaxPacketSize0    = 0x40,
        .idVendor           = NEXUS_USB_VID,
        .idProduct          = NEXUS_USB_PID,
        .bcdDevice          = 0x0100,
        .iManufacturer      = iManufacturer,
        .iProduct           = iProduct,
        .iSerialNumber      = iSerialNumber,
        .bNumConfigurations = 0x01,
    };

    // Full Speed is USB 1.1
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);

    // High Speed is USB 2.0
    device_descriptor.bcdUSB = 0x0200;
    if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);

    // Super Speed is USB 3.0, with a 512-byte control endpoint (log2 encoded)
    device_descriptor.bcdUSB = 0x0300;
    device_descriptor.bMaxPacketSize0 = 0x09;
    if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);
    if (R_FAILED(rc)) return rc;

    // Binary Object Store advertising USB 2.0 LPM and USB 3.0 support.
    u8 bos[0x16] = {
        0x05,                       // bLength
        USB_DT_BOS,                 // bDescriptorType
        0x16, 0x00,                 // wTotalLength
        0x02,                       // bNumDeviceCaps

        // USB 2.0 extension
        0x07,
        USB_DT_DEVICE_CAPABILITY,
        0x02,
        0x02, 0x00, 0x00, 0x00,

        // SuperSpeed USB device capability
        0x0A,
        USB_DT_DEVICE_CAPABILITY,
        0x03,
        0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    return usbDsSetBinaryObjectStore(bos, sizeof(bos));
}

static Result nexus_setup_interface(void)
{
    struct usb_interface_descriptor interface_descriptor = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = NEXUS_USB_NUM_ENDPOINTS,
        .bInterfaceClass    = USB_CLASS_STILL_IMAGE,
        .bInterfaceSubClass = USB_SUBCLASS_STILL_IMAGE,
        .bInterfaceProtocol = USB_PROTOCOL_PIMA_15740,
    };

    struct usb_endpoint_descriptor ep_in = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x40,
    };

    struct usb_endpoint_descriptor ep_out = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x40,
    };

#if !NEXUS_USB_NO_INTERRUPT_EP
    struct usb_endpoint_descriptor ep_intr = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes     = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize   = 0x1C,   // MTP event blocks are at most 24 bytes
        .bInterval        = 0x06,
    };
#endif

    struct usb_ss_endpoint_companion_descriptor ep_companion = {
        .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst         = 0x0F,
        .bmAttributes      = 0x00,
        .wBytesPerInterval = 0x00,
    };

    Result rc = usbDsRegisterInterface(&g_interface);
    if (R_FAILED(rc)) return rc;

    // usb:ds assigns the interface index; endpoint addresses are derived from
    // it exactly as libnx's usb_comms does.
    const u8 intf_num = g_interface->interface_index;
    interface_descriptor.bInterfaceNumber = intf_num;
    ep_in.bEndpointAddress  += intf_num + 1;
    ep_out.bEndpointAddress += intf_num + 1;
#if !NEXUS_USB_NO_INTERRUPT_EP
    ep_intr.bEndpointAddress += intf_num + 2;
#endif

    // --- Full Speed ---
    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Full,
            &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Full, &ep_in, USB_DT_ENDPOINT_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Full, &ep_out, USB_DT_ENDPOINT_SIZE);
#if !NEXUS_USB_NO_INTERRUPT_EP
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Full, &ep_intr, USB_DT_ENDPOINT_SIZE);
#endif
    if (R_FAILED(rc)) return rc;

    // --- High Speed ---
    ep_in.wMaxPacketSize  = 0x200;
    ep_out.wMaxPacketSize = 0x200;
    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High,
            &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_High, &ep_in, USB_DT_ENDPOINT_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_High, &ep_out, USB_DT_ENDPOINT_SIZE);
#if !NEXUS_USB_NO_INTERRUPT_EP
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_High, &ep_intr, USB_DT_ENDPOINT_SIZE);
#endif
    if (R_FAILED(rc)) return rc;

    // --- Super Speed --- every endpoint needs a companion descriptor
    ep_in.wMaxPacketSize  = 0x400;
    ep_out.wMaxPacketSize = 0x400;
    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
            &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_in, USB_DT_ENDPOINT_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_out, USB_DT_ENDPOINT_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
#if !NEXUS_USB_NO_INTERRUPT_EP
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_intr, USB_DT_ENDPOINT_SIZE);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(g_interface,
            UsbDeviceSpeed_Super, &ep_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
#endif
    if (R_FAILED(rc)) return rc;

    // --- Endpoints ---
    rc = usbDsInterface_RegisterEndpoint(g_interface, &g_ep_in, ep_in.bEndpointAddress);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(g_interface, &g_ep_out,
            ep_out.bEndpointAddress);
#if !NEXUS_USB_NO_INTERRUPT_EP
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(g_interface, &g_ep_intr,
            ep_intr.bEndpointAddress);
#endif
    if (R_FAILED(rc)) return rc;

    return usbDsInterface_EnableInterface(g_interface);
}

Result usbTransportInit(void)
{
    if (g_initialized) return 0;

    if (!hosversionAtLeast(5, 0, 0)) {
        LOG_E("usb: firmware < 5.0.0 is not supported by this build");
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }

    g_bounce = (u8 *)memalign(USB_XFER_ALIGN, BOUNCE_SIZE);
    if (g_bounce == NULL) {
        LOG_E("usb: bounce buffer allocation failed");
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    memset(g_bounce, 0, BOUNCE_SIZE);

    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) {
        LOG_E("usb: usbDsInitialize failed (0x%x)", rc);
        goto fail;
    }

    rc = nexus_setup_device_descriptors();
    if (R_FAILED(rc)) {
        LOG_E("usb: device descriptor setup failed (0x%x)", rc);
        goto fail_ds;
    }

    rc = nexus_setup_interface();
    if (R_FAILED(rc)) {
        LOG_E("usb: interface setup failed (0x%x)", rc);
        goto fail_ds;
    }

    rc = usbDsEnable();
    if (R_FAILED(rc)) {
        LOG_E("usb: usbDsEnable failed (0x%x)", rc);
        goto fail_ds;
    }

    g_initialized = true;
    LOG_I("usb: MTP interface up (%d endpoints)", NEXUS_USB_NUM_ENDPOINTS);
    return 0;

fail_ds:
    usbDsExit();
fail:
    free(g_bounce);
    g_bounce = NULL;
    g_interface = NULL;
    g_ep_in = g_ep_out = g_ep_intr = NULL;
    return rc;
}

void usbTransportExit(void)
{
    if (!g_initialized) return;

    usbTransportCancel();
    // usbDsExit closes any interfaces and endpoints still open, which is
    // required for usb-sysmodule to reset usb:ds back to defaults.
    usbDsExit();

    free(g_bounce);
    g_bounce = NULL;
    g_interface = NULL;
    g_ep_in = g_ep_out = g_ep_intr = NULL;
    g_initialized = false;
    LOG_I("usb: MTP interface down");
}

UsbTransportState usbTransportGetState(void)
{
    if (!g_initialized) return UsbTransportState_Detached;

    UsbState state = UsbState_Detached;
    if (R_FAILED(usbDsGetState(&state))) return UsbTransportState_Detached;

    return (state == UsbState_Configured) ? UsbTransportState_Ready
                                          : UsbTransportState_Detached;
}

bool usbTransportIsReady(void)
{
    return usbTransportGetState() == UsbTransportState_Ready;
}

UsbDeviceSpeed usbTransportGetSpeed(void)
{
    if (!g_initialized) return UsbDeviceSpeed_None;

    UsbDeviceSpeed speed = UsbDeviceSpeed_None;
    if (R_FAILED(usbDsGetSpeed(&speed))) return UsbDeviceSpeed_None;
    return speed;
}

// Posts one transfer and waits for it to complete. buffer must be page
// aligned. On timeout the URB is cancelled so the endpoint is left usable.
static Result nexus_xfer_once(UsbDsEndpoint *ep, void *buffer, size_t size,
                              u32 *out_transferred, u64 timeout_ns)
{
    u32 urb_id = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buffer, size, &urb_id);
    if (R_FAILED(rc)) return rc;

    rc = eventWait(&ep->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        // Timed out or interrupted: drop the URB, then drain the event so a
        // late completion does not leak into the next transfer.
        usbDsEndpoint_Cancel(ep);
        eventWait(&ep->CompletionEvent, UINT64_MAX);
        eventClear(&ep->CompletionEvent);
        return rc;
    }
    eventClear(&ep->CompletionEvent);

    UsbDsReportData report;
    rc = usbDsEndpoint_GetReportData(ep, &report);
    if (R_FAILED(rc)) return rc;

    u32 transferred = 0;
    rc = usbDsParseReportData(&report, urb_id, NULL, &transferred);
    if (R_FAILED(rc)) return rc;

    if (transferred > size) transferred = (u32)size;
    *out_transferred = transferred;
    return 0;
}

// Shared read/write body. usb:ds requires page-aligned DMA buffers, so
// unaligned callers are served through the bounce buffer one page at a time.
static Result nexus_xfer(UsbDsEndpoint *ep, void *buffer, size_t size,
                         size_t *out_transferred, u64 timeout_ns, bool is_write)
{
    if (ep == NULL) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (out_transferred) *out_transferred = 0;
    if (size == 0) return 0;

    u8 *ptr = (u8 *)buffer;
    size_t total = 0;

    while (size > 0) {
        void  *xfer_buf;
        size_t chunk;
        bool   bounced;

        if (((uintptr_t)ptr & (USB_XFER_ALIGN - 1)) != 0) {
            // Move just enough to bring ptr back to a page boundary.
            chunk = USB_XFER_ALIGN - ((uintptr_t)ptr & (USB_XFER_ALIGN - 1));
            if (chunk > size) chunk = size;
            xfer_buf = g_bounce;
            bounced  = true;
            if (is_write) memcpy(g_bounce, ptr, chunk);
            else          memset(g_bounce, 0, chunk);
        } else {
            chunk = size;
            if (chunk > USB_XFER_MAX_SIZE) chunk = USB_XFER_MAX_SIZE;
            xfer_buf = ptr;
            bounced  = false;
        }

        u32 moved = 0;
        Result rc = nexus_xfer_once(ep, xfer_buf, chunk, &moved, timeout_ns);
        if (R_FAILED(rc)) {
            if (out_transferred) *out_transferred = total;
            return rc;
        }

        if (bounced && !is_write) memcpy(ptr, g_bounce, moved);

        ptr   += moved;
        size  -= moved;
        total += moved;

        // A short transfer terminates the request: for reads this is the
        // host's end-of-data marker, for writes it means the host stopped
        // accepting. Either way there is nothing more to do.
        if (moved < chunk) break;
    }

    if (out_transferred) *out_transferred = total;
    return 0;
}

Result usbTransportRead(void *buffer, size_t size, size_t *out_transferred, u64 timeout_ns)
{
    return nexus_xfer(g_ep_out, buffer, size, out_transferred, timeout_ns, false);
}

Result usbTransportWrite(const void *buffer, size_t size, size_t *out_transferred, u64 timeout_ns)
{
    // nexus_xfer only writes into the buffer on the read path, so discarding
    // const here is safe.
    return nexus_xfer(g_ep_in, (void *)buffer, size, out_transferred, timeout_ns, true);
}

Result usbTransportWriteInterrupt(const void *buffer, size_t size, u64 timeout_ns)
{
    if (g_ep_intr == NULL) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (size > BOUNCE_SIZE) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // Events are small and rare; always stage them through the aligned buffer.
    memcpy(g_bounce, buffer, size);
    u32 moved = 0;
    return nexus_xfer_once(g_ep_intr, g_bounce, size, &moved, timeout_ns);
}

void usbTransportCancel(void)
{
    if (g_ep_in)   usbDsEndpoint_Cancel(g_ep_in);
    if (g_ep_out)  usbDsEndpoint_Cancel(g_ep_out);
    if (g_ep_intr) usbDsEndpoint_Cancel(g_ep_intr);
}

void usbTransportStall(void)
{
    if (g_ep_in)  usbDsEndpoint_Stall(g_ep_in);
    if (g_ep_out) usbDsEndpoint_Stall(g_ep_out);
}
