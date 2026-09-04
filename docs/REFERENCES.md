# References

Primary sources used while building this scaffold and verifying the original
blueprint. Everything asserted in [PLAN-REVIEW.md](PLAN-REVIEW.md) traces back
to something here, checked on 2026-09-03.

## Toolchain and SDK

- **libnx** — <https://github.com/switchbrew/libnx> (ISC)
  - `nx/include/switch/services/usbds.h` — the `usb:ds` API surface
  - `nx/include/switch/services/usb.h` — descriptor structs, class and
    endpoint constants, `UsbState`
  - `nx/include/switch/services/ncm.h` — content storage and meta database;
    the source for the corrected `PlaceHolder` function names
  - `nx/include/switch/services/fs.h` — gamecard (`fsOpenDeviceOperator`,
    `fsDeviceOperatorGetGameCardHandle`, `fsOpenGameCardFileSystem`) and
    savedata (`fsOpen_SaveData`, `fsOpenSaveDataInfoReader`) entry points
  - `nx/include/switch/services/fspr.h` — confirms `fs:pr` is the program
    registry, not a gamecard service
  - `nx/include/switch/runtime/util/utf.h` — `utf8_to_utf16` / `utf16_to_utf8`
  - `nx/include/switch/result.h` — `LibnxError_*` values
  - `nx/source/runtime/devices/usb_comms.c` — the reference usb:ds
    initialisation and transfer sequence that `src/usb/usb_transport.c` follows
  - The service header listing contains **no `es.h`**, which is how §3 of the
    plan review establishes that `esImportTicket` must be hand-written

- **switch-examples** — <https://github.com/switchbrew/switch-examples>
  - `templates/application/Makefile` — the base this project's Makefile is
    adapted from

- **devkitPro Docker images** — <https://hub.docker.com/r/devkitpro/devkita64>
  - Confirmed multi-arch (`linux/amd64`, `linux/arm64`); `latest` last pushed
    2026-02-19

## Protocol

- **PIMA 15740:2000** — Picture Transfer Protocol. Source of the container
  layout, operation codes, response codes and the ObjectInfo / DeviceInfo /
  StorageInfo dataset shapes in `include/nexus/mtp_types.h`.
- **Microsoft MTP extension** — the `0x9xxx` operation codes, `0xDCxx` object
  property codes, `0xD4xx` device property codes, and the
  `"microsoft.com: 1.0;"` vendor extension string that Windows keys MTP
  support off.
- **USB 2.0 / 3.0 specifications** — Still Image class (`bInterfaceClass 0x06`,
  SubClass `0x01`, Protocol `0x01`), endpoint descriptors, BOS and SuperSpeed
  companion descriptors, and the zero-length-packet termination rule for
  transfers that are an exact multiple of `wMaxPacketSize`.

## Homebrew ecosystem

Licences and repository state were checked via the GitHub API; the GPL
"or later" elections were confirmed from the projects' own per-file headers
rather than inferred from the SPDX tag. Since NX-Nexus is GPL-3.0-or-later,
all of these except Tinwoo are available to port from — see
[../NOTICE](../NOTICE) for the attribution rules.

- `retronx-team/mtp-server-nx` — Apache-2.0, last pushed 2020-12-21.
  GPLv3-compatible, GPLv2-incompatible. Usable with notices retained.
- `DarkMatterCore/libusbhsfs` — dual ISC / GPLv2+; README establishes it is a
  USB mass storage **host** library, so it has no role here regardless
- `XorTroll/Goldleaf` — GPL-3.0-or-later (per `Goldleaf/source/main.cpp`
  header). Usable.
- `DarkMatterCore/nxdumptool` — GPL-3.0-or-later (per
  `include/core/nxdt_utils.h` on the `rewrite` branch). Usable.
- `ocornut/imgui` — MIT. Usable.
- `Huntereb/Tinwoo` — **does not exist**; surviving forks declare no licence, so
  they grant no rights and must not be used under any circumstances

## Still to be sourced (Phase 2 / 3)

Now that the project is GPL-3.0-or-later, these may come from either the
switchbrew wiki or the GPL reference implementations above. The wiki is still
preferable where it documents the IPC directly, because it avoids inheriting
another project's structure and version assumptions.

| Needed | Wiki | GPL reference |
|---|---|---|
| `es` IPC (`ImportTicket`, `ImportTicketCertificateSet`) | yes | Goldleaf, nxdumptool |
| PFS0 / NCA container layouts | yes | nxdumptool (most thorough) |
| `ncm` content meta record format for `ncmContentMetaDatabaseSet` | partial | Goldleaf |
| `nsPushApplicationRecord` record layout | partial | Goldleaf |
| Raw gamecard storage IPC (absent from libnx) | partial | nxdumptool |

<https://switchbrew.org/wiki/Main_Page>
