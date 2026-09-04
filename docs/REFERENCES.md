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

  Atmosphere is deliberately absent from that list: it is GPL-2.0-**only**, so
  its Daybreak firmware installer cannot be ported from into a GPL-3.0-or-later
  work. `src/installer/firmware.c` is clean-room from the wiki instead.

## Compression

- `nicoboss/nsz` — <https://github.com/nicoboss/nsz>
  `docs/formats.md` and `nsz/IndependentNczDecompressorConcise.py` describe the
  NCZ container: the `NCZSECTN` preamble at 0x4000, the 0x38-byte section
  entry, the per-section AES key and counter, and the AES-CTR counter
  construction (`Counter.new(64, prefix=nonce[0:8], initial_value=offset >> 4)`).

  The reference implementation is the authority where the two disagree — the
  prose folds the one-off magic into the section entry, which reads as though
  entries were 0x48 bytes.

  Note for anyone auditing the "no keys" claim: the section keys are *in the
  file*, put there so third-party installers can rebuild an NCA without
  deriving anything. Making an NSZ needs `prod.keys`; installing one does not.

- Zstandard — <https://facebook.github.io/zstd/>
  Linked from devkitPro portlibs (`libzstd.a`). Only the streaming
  decompression API is used.

## Runtime key derivation, and its limits

nxdumptool's README is the practical reference here. It can derive the NCA
header key and decrypt NCA key areas at runtime through the SPL services, so
gamecard operations need no keys file — but NSP dumping and RomFS/ExeFS access
for SD/eMMC titles still require `sdmc:/switch/prod.keys`, specifically
`eticket_rsa_kek` and `titlekek_XX`.

NX-Nexus needs none of that, because it never decrypts an NCA: content ids come
from filenames, sizes and hashes from the CNMT (which Horizon decrypts for us),
and NCZ section keys from the NCZ itself.

<https://github.com/DarkMatterCore/nxdumptool/blob/main/README.md>

## Sourced during implementation

Everything the first two phases needed has now been written, from the wiki
except where [../NOTICE](../NOTICE) records otherwise.

| Needed | Where it came from |
|---|---|
| `es` IPC (`ImportTicket`, `ImportTicketCertificateSet`) | switchbrew wiki |
| PFS0 / HFS0 container layouts | switchbrew wiki |
| XCI gamecard header layout | switchbrew wiki |
| `ncm` content meta record format | switchbrew wiki |
| `nsPushApplicationRecord` / `ListApplicationRecordContentMeta` | switchbrew wiki |
| Raw gamecard storage IPC (absent from libnx) | nxdumptool — see NOTICE |
| Exosphere SPL config items 65000 / 65007 | nxdumptool — see NOTICE |
| NCZ container layout | nsz — see NOTICE |
| `ContentMetaInfo` layout, for firmware installs | switchbrew wiki |

<https://switchbrew.org/wiki/Main_Page>
