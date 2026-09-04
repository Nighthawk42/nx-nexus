# Usage

## Getting started

Copy `NX-Nexus.nro` to `sdmc:/switch/` and launch it from the homebrew menu.

Nothing is exposed over USB until you ask for it. The header line always states
whether the server is running, so you are never accidentally mounted on a
machine you plugged into for charging.

### Controls

| | |
|---|---|
| **Up / Down** | move the selection |
| **A** | select or run |
| **B** | back |
| **L / R** | page (also scrolls the log) |
| **+** | quit |

### Menu

- **Start / Stop MTP server** — brings the USB interface up or down
- **Stores** — what each store is and whether it is available
- **Transfer status** — throughput, operation and error counts
- **Install from a source** — browse a configured server and install
- **Check for updates** — self-update, if `update_url` is configured
- **Maintenance** — reclaim space left by interrupted installs
- **Log** — scrollable; also written to `sdmc:/switch/nx-nexus/nx-nexus.log`

## The stores

### Installing

Copy an `.nsp` into **MicroSD Install** or **System Install**. The bytes stream
from the USB buffer straight into `ncm` placeholders — nothing is staged on the
SD card, so installing needs no free space beyond the title itself.

The stores are intentionally empty when browsed: they are drop targets, not
folders.

### Extracting

**Installed Titles** shows one folder per installed game, update and DLC:

```
Game Name [0100000000010000] v0 Application/
    Game Name [0100000000010000] v0.nsp
    icon.jpg
    info.txt
```

The `.nsp` does not exist anywhere — its header is generated on the fly and its
body is read out of `ncm` as you copy it. Deleting a title folder deletes the
title.

> Deleting an **update or DLC** on its own is refused. Removing one means
> rewriting the application's record rather than deleting the application, and
> getting that wrong leaves the base game unlaunchable.

### Game card

Partitions appear as folders (`secure`, `normal`, `update`, `logo`). The root
also carries `card.xci`, a byte-exact image served from raw sector reads.

### Saves

One folder per save, named after the game. Reads go through a read-only mount,
so browsing and backing up can never touch a save.

Restoring writes each file to a `.nxtmp` scratch name and only renames it over
the real one after the whole transfer has arrived and the filesystem has been
committed. A cable pull leaves the original untouched.

Restores go into a save that already exists — creating one from nothing would
mean choosing a size and owner that this tool has no way to know.

### NAND

Raw BIS partitions of the MMC you booted from.

> **Horizon mounts exactly one NAND.** If you booted an emuMMC then `ncm`, `ns`,
> the save index and these partitions all describe the *emuMMC* — the real
> sysMMC is not reachable from inside the running system, and no IPC exposes it.
> The store name says which one you are looking at. A file-based emuMMC's images
> live at `sdmc:/emuMMC/RAWn/` and are visible through store 1.

> `PRODINFO.bin` holds console-unique certificates. It is exposed because
> dumping it before a NAND repair is the standard reason to want it — treat that
> file like a private key.

## Sources

`sdmc:/switch/nx-nexus/sources.json` is written with a commented template on
first run:

```json
{
  "insecure": false,
  "update_url": "",
  "sources": [
    { "name": "My server", "url": "https://your-server.example/index.json" }
  ]
}
```

An index is either `{"files": [...]}` or a bare array. Entries may be objects
with `url`, optional `size` and optional `name`, or plain URL strings:

```json
{ "files": [ { "url": "https://.../Game.nsp", "size": 1234567890 } ] }
```

### TLS

This downloads executables that then get installed, so **certificate
verification is on by default** and needs a CA bundle at
`sdmc:/switch/nx-nexus/cacert.pem` (for example from
<https://curl.se/ca/cacert.pem>).

Without one, HTTPS is **refused** rather than silently downgraded — otherwise
anyone between you and the server picks what gets installed. `"insecure": true`
opts out, and says so loudly in the log.

### Policy

NX-Nexus ships no sources and has no discovery or search. Every entry is one
you typed. Public "free shops" are not supported and not endorsed.

## Updating

Set `update_url` in `sources.json` to a manifest:

```json
{ "version": "0.2.0", "url": "https://.../NX-Nexus.nro", "notes": "what changed" }
```

**Check for updates** compares versions and downloads on request. Nothing is
checked automatically. The download lands on a scratch name, is verified to
actually be an NRO before replacing the running one, and the previous build is
kept as `NX-Nexus.nro.bak`.

## Installing firmware

**This is only offered when the console booted from an emuMMC.** On sysMMC the
feature is visible but blocked, and says so.

That gate is the whole reason the feature exists in this shape. A firmware
install that goes wrong on sysMMC leaves a console that will not boot and
cannot easily be repaired from the console itself. The same failure on an
emuMMC is an inconvenience: sysMMC still boots, and the emuMMC can be restored.

Horizon mounts exactly one NAND, so "update the emuMMC" just means running this
while booted into it — the content lands on whichever system partition is
active.

### Before you start

**Back up your NAND.** Store 7 dumps the raw partitions, and a failed install is
only recoverable from a backup. This is not boilerplate; it is the actual
recovery path.

Check the firmware is not newer than your Atmosphère supports. Content installs
regardless, and an unsupported version will not boot.

### Doing it

1. Put a firmware folder (loose `.nca` files) at `sdmc:/firmware`
2. **Install firmware** → **Scan**
3. **I have a NAND backup — continue** (arms the install)
4. **INSTALL FIRMWARE NOW**
5. Reboot

Arming is a separate step on purpose: the install is never one careless button
press away.

A sanity check refuses folders with fewer than 16 meta files — that is what
pointing this at a game folder looks like, and installing a game as system
content is not something to do by accident.

Content is registered first and metadata second, so the system never briefly
describes content that is not there. Any failure rolls the whole thing back.

> **Untested on hardware.** This is the highest-risk operation in the tool and
> it has not been run against a real system partition. Do not use it on
> anything you cannot restore.

## Maintenance

Interrupted installs leave placeholder files that were never registered. They
consume space that nothing in the HOME menu accounts for.

- **Scan** — reports what is reclaimable, changes nothing
- **Clear stray placeholders** — deletes them
- **Remove redundant title data** — asks `ns` to drop application entities with
  no record behind them

Nothing here touches content a title still references.

## Troubleshooting

Logs are at `sdmc:/switch/nx-nexus/nx-nexus.log`.

**The console does not appear on the PC.** The MTP spec wants three endpoints,
but libnx's own USB code only ever uses two and whether `usb:ds` accepts the
third is unverified. Rebuild with `./scripts/build.sh --no-intr-ep`; that drops
MTP event notifications, which no file manager needs for browsing or transfer.

**`USB init failed` on start.** Something else holds `usb:ds` — close any other
USB homebrew first.

**A transfer stalls at 100%.** Zero-length-packet termination: a data phase that
is an exact multiple of `wMaxPacketSize` must be followed by a ZLP.
`src/mtp/mtp_server.c` derives the packet size from the negotiated bus speed and
sends it.

**Saves, Titles or NAND are empty.** Almost always FS permissions — a plain
hbmenu launch may not be allowed to enumerate savedata or open BIS. The log
records the exact cause:

```
saves: OpenSaveDataInfoReader failed (0x...) -- this usually means the app
       lacks FS permissions for savedata
```

Running through a forwarder, or with elevated hbloader permissions, resolves it.

**A title installs but does not appear on HOME.** That is the
`nsPushApplicationRecord` step. Known limitation: the record is replaced rather
than merged, so installing a patch for a title already present may drop the base
game's record.

**Firmware older than 5.0.0** is not supported; the pre-5.0.0 `usb:ds`
descriptor API is a different path and is not implemented.
