# Blueprint review — what checks out and what does not

The original NX-Nexus blueprint was checked against the current upstream
sources on 2026-09-03. Most of the architecture holds up. A handful of claims
are wrong in ways that matter, and one is a project-level contradiction that
should be settled before writing Phase 2 code.

Everything below was verified against the actual repositories and headers, not
recalled. Sources are listed in [REFERENCES.md](REFERENCES.md).

---

## 1. Resolved: licence is GPL-3.0-or-later

**Decision (2026-09-03): NX-Nexus is GPL-3.0-or-later.** The blueprint's
"MIT-licensed" goal was dropped in favour of a licence that can actually absorb
the intended component set. The rest of this section records why.

The blueprint called for an **MIT-licensed** project while sourcing code from
**GPL** projects. These cannot both be true. GPL is copyleft: a derivative work
must itself be GPL, so copying code from Goldleaf or nxdumptool into an MIT
repository is not permitted.

| Component | Blueprint claim | Verified reality |
|---|---|---|
| `switchbrew/libnx` | ISC | **ISC** — compatible, link freely |
| `ocornut/imgui` | MIT | **MIT** — compatible |
| `retronx-team/mtp-server-nx` | "MIT / GPL" | **Apache-2.0** — GPLv3-compatible only; dormant since 2020-12-21 |
| `DarkMatterCore/libusbhsfs` | "ISC / GPLv2" | **Dual: ISC or GPLv2+** depending on `BUILD_TYPE` — claim is correct |
| `XorTroll/Goldleaf` | GPLv3 | **GPL-3.0-or-later** — now usable |
| `DarkMatterCore/nxdumptool` | "GPLv2" | **GPL-3.0-or-later**, not v2 — now usable |
| `Huntereb/Tinwoo` | listed as a source | **Does not exist.** Surviving forks (`falcorr/TinWoo`, `daz2048/TinWoo`) carry *no declared licence*, i.e. all rights reserved |

The Tinwoo point deserves emphasis: an unlicensed repository is the *least*
"drama-free" thing you could copy from. No licence means no grant of rights at
all, which is a worse position than GPL.

### Why GPL-3.0 specifically, and not GPLv2

GPLv3 is not interchangeable with GPLv2 here. **Apache-2.0 is compatible with
GPLv3 but not GPLv2** — its patent-retaliation and notice terms count as
additional restrictions under v2. Since `mtp-server-nx` is Apache-2.0, choosing
GPLv2 would exclude the very MTP implementation the blueprint wanted to port
from.

The "or later" election matters too. Goldleaf and nxdumptool both elect
GPL-3.0-**or-later** in their per-file headers (verified from the headers, not
assumed from the SPDX tag), and libusbhsfs's copyleft build is
GPLv2-**or-later**, which the "or later" clause lets us combine under v3.
Everything lines up under GPL-3.0-or-later.

### What this unlocks

Phase 2 and Phase 3 may now port directly from Goldleaf and nxdumptool instead
of clean-rooming from the switchbrew wiki. That removes the bulk of the
projected work: PFS0 and NCA parsing, the `ncm` placeholder pipeline, the
hand-written `es` wrapper, and the raw gamecard IPC all have working
GPL-licensed reference implementations that can now be adapted with
attribution.

The obligations that come with it are all cheap for an already-open project:

- Distributed `.nro` builds must be accompanied by, or offer, the complete
  corresponding source. Keeping the repository public and linking it from
  releases satisfies this.
- Imported files keep their upstream copyright and licence headers. Do not
  replace an upstream author's notice with an NX-Nexus one.
- Modified upstream files need prominent notice of the change and its date
  (GPL §5(a)).
- Apache-2.0 files stay Apache-2.0 inside the combined GPLv3 work rather than
  being relabelled.
- An interactive program should display the copyright, warranty disclaimer and
  licence pointer that GPL "Appropriate Legal Notices" expects; `src/main.c`
  does this on the status screen.

### The exception no licence can fix

Relicensing solves Goldleaf, nxdumptool, libusbhsfs and mtp-server-nx. It does
**not** solve Tinwoo. An unlicensed repository grants no rights at all, so
there is no licence NX-Nexus could adopt that would make copying from it
lawful. Tinwoo is off the source list permanently — and its intended
contribution, PFS0 and NCA handling, is now better served by nxdumptool or
Goldleaf anyway.

### Current state of this tree

Every file is GPL-3.0-or-later. Nothing has yet been copied from any of the
newly-permissible upstreams — the licence allows it, but the code here is still
original. The one borrowed pattern is the usb:ds descriptor and transfer
sequence, which follows libnx's own ISC-licensed `usb_comms.c` and is
attributed in [NOTICE](../NOTICE) and in the source header.

---

## 2. Wrong: `libusbhsfs` has no role here

The blueprint lists libusbhsfs for "high-throughput USB bulk endpoint buffer
handling". That is not what it does. Its own README describes it as a
"USB Mass Storage Class **Host** + Filesystem Mounter" — it lets the Switch read
USB drives plugged *into* the dock. It has nothing to do with the console
presenting *itself* as a USB device to a PC.

For NX-Nexus the relevant API is `usb:ds` from libnx directly, which is what
`src/usb/usb_transport.c` uses. **Drop libusbhsfs from the stack table.** It
would only return if you later add "install from a USB HDD in the dock", which
is a different feature — and note that its NTFS/EXT build is GPLv2+, which
re-opens the licence question above.

---

## 3. Wrong: the `ncm` and `es` function names

None of the installer API names in the blueprint exist as written.

| Blueprint | Actual libnx symbol |
|---|---|
| `ncmCreatePlaceholder()` | `ncmContentStorageCreatePlaceHolder()` |
| `ncmWritePlaceholder()` | `ncmContentStorageWritePlaceHolder()` |
| `ncmRegisterTitle()` | **No such call.** Registration is `ncmContentStorageRegister()` for content, then `ncmContentMetaDatabaseSet()` + `ncmContentMetaDatabaseCommit()`, then `nsPushApplicationRecord()` |
| `esImportTicket()` | **`es` is not in libnx at all** |

Note the capitalisation: libnx spells it `PlaceHolder`, not `Placeholder`.

The `es` finding is the significant one. libnx ships no `es.h` — the service
header list contains `ncm.h`, `ns.h`, `fs.h` and so on, but nothing for `es`.
Goldleaf and Tinwoo hand-roll the `es` IPC themselves. Phase 2 therefore needs
a hand-written `es` service wrapper (`esImportTicket` is command 1,
`esImportTicketCertificateSet` is command 2 in the public IPC documentation),
written from the switchbrew wiki rather than copied from a GPL implementation.

Phase 2's step list should also gain an explicit **`nsPushApplicationRecord`**
step. Without it the title installs but never appears on the HOME menu, which
is a very common first-attempt bug.

---

## 4. Wrong: `fs:pr` is not the gamecard service

The blueprint routes cartridge access through `fs:pr`. That service does exist
in libnx (`fspr.h`) but it is **FilesystemProxy-ProgramRegistry** — it registers
programs and their filesystem access policies (`fsprRegisterProgram`,
`fsprSetEnabledProgramVerification`). It has nothing to do with game cards.

The real gamecard path is through `fsp-srv`:

```c
fsOpenDeviceOperator(&op);
fsDeviceOperatorIsGameCardInserted(&op, &inserted);
fsDeviceOperatorGetGameCardHandle(&op, &handle);
fsOpenGameCardFileSystem(&fs, &handle, partition);
```

There is one further catch for Phase 3: **libnx exposes no raw gamecard storage
API.** There is no `fsOpenGameCardStorage`, only the filesystem-level
`fsOpenGameCardFileSystem`. Dumping a raw XCI (rather than the mounted
partitions) requires hand-written IPC, which is exactly what nxdumptool does.
So Phase 3 needs custom IPC too, and the licence decision in §1 applies again.

---

## 5. Wrong: the savedata API name and the wrong API for the job

The blueprint calls `fsOpenSaveDataFileSystemBySysSaveDataId()`. Two problems:

1. The real name is `fsOpenSaveDataFileSystemBySystemSaveDataId()` —
   `System`, not `Sys`.
2. More importantly, that call is for **system** savedata. Storage
   `0x00010005` is meant to expose *game* saves, which is a different API:
   `fsOpen_SaveData(&fs, application_id, uid)` or the read-only
   `fsOpen_SaveDataReadOnly()`. Enumerate them with `fsOpenSaveDataInfoReader()`.

Read-only is the right choice for a dump-only store, and it avoids the
save-corruption risk of mounting user saves writable.

---

## 6. Correct, and confirmed

These parts of the blueprint stand up:

- **`usb:ds` is the right transport.** `usbds.h` exists with the full API:
  `usbDsRegisterInterface`, `usbDsInterface_RegisterEndpoint`,
  `usbDsInterface_AppendConfigurationData`, `usbDsEndpoint_PostBufferAsync`.
- **`devkitpro/devkita64:latest` is a real, current image** — last pushed
  2026-02-19, and multi-arch (`linux/amd64` + `linux/arm64`), so either an
  x86-64 or an arm64 build host works natively.
- **The virtual-storage architecture is sound.** Splitting install targets into
  separate MTP stores is a clean way to distinguish "copy a file" from
  "install a title" without a companion app. `src/storage/storage.h` implements
  exactly this as a backend vtable.
- **The phase ordering is right.** Getting a plain SD-card MTP store working
  first isolates USB and protocol bugs from installer bugs.

---

## 7. Uncertain — needs hardware to settle

Two things in this scaffold are best-effort and cannot be confirmed without a
console:

- **Three-endpoint MTP interface.** The MTP spec wants bulk-IN, bulk-OUT and
  interrupt-IN. libnx's `usb_comms` only ever registers two endpoints, and it
  is unclear whether `usb:ds` accepts the third address
  (`USB_ENDPOINT_IN + interface_index + 2`) for one interface. If enumeration
  fails, rebuild with `-DNEXUS_USB_NO_INTERRUPT_EP=1`, which drops to two
  endpoints and gives up MTP events. Host file managers do not need events to
  browse or transfer.
- **Zero-length packet termination.** A data phase whose total length is an
  exact multiple of `wMaxPacketSize` must be terminated with a ZLP.
  `mtp_server.c` derives the packet size from the negotiated bus speed and
  emits the ZLP itself rather than relying on `usbDsEndpoint_SetZlt`, because
  ZLT is per-URB and would inject spurious packets mid-stream. This logic is
  the most likely source of a "transfer hangs at 100%" bug.

What *has* been verified: the project compiles clean under devkitA64 (GCC 15.2)
with `-Werror`, in both the three-endpoint and two-endpoint configurations, and
produces a valid `NRO0` binary. The container parsers and the install
orchestrator pass 333 assertions on the host under AddressSanitizer and
UndefinedBehaviorSanitizer, including every install failure and rollback path
and chunk boundaries from one byte to 64 KiB. None of that says the USB layer or
the `ncm` pipeline behaves correctly against real hardware — only a console can
settle that.

Two further things are written but never executed: the hand-rolled `es` and
`ns` IPC. The command numbers and buffer attributes come from the public
documentation, but an IPC mistake there will surface as a `0x...` result code
at install time, not at compile time.

---

## 8. Correction: NCA parsing is neither necessary nor possible

The blueprint's Phase 2 calls for "on-the-fly NCA container analysis" to extract
the title id and content types. That cannot work, and does not need to.

Per switchbrew: *"The entire raw NCAs are encrypted."* The first 0xC00 bytes are
AES-XTS with a non-standard tweak; only the logo section, when present, is
plaintext. Parsing an NCA header therefore requires the console's key material.
A tool that ships or derives keys is exactly the kind of thing this project set
out to avoid.

It is also unnecessary, because everything the installer needs is available
without ever decrypting an NCA:

| Needed | Where it actually comes from |
|---|---|
| Content id | The NCA's filename inside the NSP (`<32 hex chars>.nca`) |
| Content type | The CNMT's `PackagedContentInfo.ContentType` |
| Content size | The CNMT's 40-bit `Size` field |
| Title id, version | The CNMT header |
| Title key | The ticket, handed to `es` untouched |

The CNMT itself lives inside the Meta NCA, which is encrypted — but Horizon will
decrypt it on request. The sequence is: write the meta NCA to a placeholder,
register it, then `ncmContentStorageGetPath` followed by
`fsOpenFileSystemWithId(..., FsFileSystemType_ContentMeta, path)` yields the
plaintext `.cnmt`. The OS does the decryption with keys it already holds.

**Consequence: NX-Nexus needs no keys at all**, and `nca.cpp` drops out of the
Phase 2 work list entirely. The parsers that do matter — PFS0/HFS0, CNMT and
tickets — are all plaintext, which is why they could be written and unit-tested
on the host before any console was involved.
