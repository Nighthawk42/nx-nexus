<p align="center">
  <img src="assets/logo.svg" alt="NX-Nexus" width="520">
</p>

<p align="center">
  <strong>An open-source MTP server and streaming installer for the Nintendo Switch.</strong><br>
  Plug the console into a PC, Mac or Linux box over USB-C and it appears as an
  ordinary media device — no companion app, no drivers, no telemetry.
</p>

<p align="center">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Nintendo%20Switch-informational">
  <img alt="Toolchain" src="https://img.shields.io/badge/built%20with-devkitA64%20%2B%20libnx-lightgrey">
</p>

---

> **Status: early, and only partly proven on hardware.** USB enumeration and
> SD-card browsing are confirmed working on a real console. The installer, game
> card, save, title and NAND features build clean and are unit-tested where that
> is possible, but have not all been exercised end to end. Treat it accordingly.

## What it does

- Appears as a standard **MTP device** — browse and transfer with your normal
  file manager
- **Installs titles without staging.** Drop an NSP into an install store and the
  bytes go from the USB buffer straight into `ncm`; nothing is written to the SD
  card first
- **Extracts installed titles** back out as a synthesised NSP that exists only
  while you copy it
- **Dumps game cards** as their filesystem, or as a byte-exact `card.xci`
- **Backs up and restores saves**, with restores staged so a failed transfer
  cannot corrupt a live save
- **Reads raw NAND partitions** of whichever MMC you booted from
- **Installs from your own server** over HTTPS, and can update itself
- **No keys, anywhere.** NCAs are never decrypted by this tool

## Stores

| ID | Store | Access |
|---|---|---|
| `0x00010001` | **MicroSD** — passthrough to `sdmc:/` | read/write |
| `0x00010002` | **MicroSD Install** — drop an NSP to install to SD | write |
| `0x00010003` | **System Install** — same, to internal storage | write |
| `0x00010004` | **Game Card** — partitions, plus `card.xci` | read |
| `0x00010005` | **Saves** — one folder per save, named after the game | read/restore |
| `0x00010006` | **Installed Titles** — browse, extract as NSP, delete | read/delete |
| `0x00010007` | **NAND** — raw BIS partitions of the active MMC | read |

## Quick start

Copy `NX-Nexus.nro` to `sdmc:/switch/` and launch it from the homebrew menu.

The USB interface is **not** brought up automatically — choose **Start MTP
server** from the menu when you want the console to be visible. Press **+** to
quit.

## Build

Docker is the only requirement; no local devkitPro install needed.

```bash
./scripts/build.sh          # build
./scripts/build.sh --strict # -Werror, same as CI
./scripts/test.sh           # host unit tests, with ASan + UBSan
```

The toolchain image is multi-arch, so x86-64 and arm64 hosts both build
natively.

## Documentation

| | |
|---|---|
| [Usage](docs/USAGE.md) | Menus, stores, sources, updates, troubleshooting |
| [Architecture](docs/ARCHITECTURE.md) | Layout, design decisions, adding a store |
| [Plan review](docs/PLAN-REVIEW.md) | Fact-check of the original design against reality |
| [References](docs/REFERENCES.md) | Primary sources for every format and API used |

## On content sources

NX-Nexus can install from a URL you configure. That is a normal way to move
titles you already own from your own server onto your own console — the same
capability as the USB path, with a different transport.

**It ships no sources, has no discovery or search, and aggregates nothing.**
Public "free shops" that redistribute other people's games are not supported,
not endorsed, and not something this project will help you find. Point it at
your own server.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

GPLv3 was chosen deliberately rather than by default: it is the only licence
under which libnx (ISC), Dear ImGui (MIT), mtp-server-nx (Apache-2.0),
libusbhsfs (GPLv2+) and Goldleaf/nxdumptool (GPL-3.0-or-later) can all be
lawfully combined — Apache-2.0 in particular is compatible with GPLv3 but *not*
GPLv2. The reasoning is in [docs/PLAN-REVIEW.md §1](docs/PLAN-REVIEW.md).

Because this is copyleft, anyone distributing built `.nro` files must also
provide, or offer, the complete corresponding source.

## Legal

A homebrew tool for consoles their owner has legitimate access to. It contains
no keys, no copyrighted Nintendo material, and no circumvention of encryption.
No DRM, no anti-tamper, no telemetry. Installing content requires you to supply
your own files.

Not affiliated with or endorsed by Nintendo. "Nintendo Switch" is a trademark of
Nintendo.
