// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- usb:ds bulk transport for the MTP responder.
//
// Exposes the console to a host PC as a USB Still Image class device
// (bInterfaceClass 0x06 / SubClass 0x01 / Protocol 0x01 == PIMA 15740), which
// is what Windows, macOS and Linux probe for when mounting an MTP device.
#pragma once

#include <switch.h>
#include <stddef.h>

// usb:ds requires every DMA buffer to be page aligned. Transfers are posted
// directly out of caller-supplied memory, so callers must honour this.
#define USB_XFER_ALIGN 0x1000u

// Largest single bulk transfer we will post. 1 MiB keeps the URB count low on
// large title transfers without reserving an unreasonable amount of memory.
#define USB_XFER_MAX_SIZE (1u * 1024u * 1024u)

// Convenience for declaring a correctly aligned static/stack transfer buffer.
#define USB_ALIGNED __attribute__((aligned(USB_XFER_ALIGN)))

typedef enum {
    UsbTransportState_Detached = 0,  // no host, or cable unplugged
    UsbTransportState_Ready    = 1,  // enumerated and configured by the host
} UsbTransportState;

/// Brings up usb:ds, publishes the MTP descriptors and enables the interface.
/// Safe to call once; returns an error Result on failure and leaves nothing
/// initialised.
Result usbTransportInit(void);

/// Tears down endpoints, interface and the usb:ds session. Idempotent.
void usbTransportExit(void);

/// Current USB state, derived from usbDsGetState().
UsbTransportState usbTransportGetState(void);

/// True once the host has configured the device (UsbState_Configured).
bool usbTransportIsReady(void);

/// Negotiated bus speed, for reporting in the UI. UsbDeviceSpeed_None if
/// unknown or detached.
UsbDeviceSpeed usbTransportGetSpeed(void);

/// Reads up to size bytes from the bulk-OUT endpoint (host -> console).
/// buffer must be USB_XFER_ALIGN aligned and size <= USB_XFER_MAX_SIZE.
/// out_transferred receives the byte count actually moved (may be short, and
/// may legitimately be 0 for a zero-length packet).
/// timeout_ns == UINT64_MAX blocks indefinitely.
Result usbTransportRead(void *buffer, size_t size, size_t *out_transferred, u64 timeout_ns);

/// Writes size bytes to the bulk-IN endpoint (console -> host). Same alignment
/// and size constraints as usbTransportRead.
Result usbTransportWrite(const void *buffer, size_t size, size_t *out_transferred, u64 timeout_ns);

/// Writes an MTP event to the interrupt-IN endpoint. Events are advisory; the
/// caller may ignore failures.
Result usbTransportWriteInterrupt(const void *buffer, size_t size, u64 timeout_ns);

/// Cancels any in-flight transfer on both bulk endpoints. Used to unblock the
/// server thread during shutdown.
void usbTransportCancel(void);

/// Stalls the bulk endpoints, which is how MTP signals a protocol-level fault
/// to the host.
void usbTransportStall(void);
