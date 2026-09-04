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
| **X** | delete (on the title and mod lists; press twice) |
| **Y** | change sort order (title list) |
| **-** | change filter (title list) |
| **+** | quit |

### Menu

**Server**

- **Start / Stop MTP server** — brings the USB interface up or down
- **Stores** — what each store is and whether it is available
- **Transfer status** — throughput, operation and error counts

**Install**

- **Install from SD card or gamecard** — no PC involved
- **Install from a source** — browse a configured server and install

**Manage**

- **Installed titles** — browse, sort, filter, verify or delete
- **Mods and cheats** — enable, disable or remove LayeredFS content
- **Verify installed content** — hash everything against its manifest
- **Maintenance** — reclaim space left by interrupted installs

**System**

- **Install firmware** — emuMMC only
- **Check for updates** — self-update
- **Log** — scrollable; also written to `sdmc:/switch/nx-nexus/nx-nexus.log`

## The stores

### Installing

Copy an `.nsp` or `.nsz` into **MicroSD Install** or **System Install**. The
bytes stream from the USB buffer straight into `ncm` placeholders — nothing is
staged on the SD card, so installing needs no free space beyond the title
itself.

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

Deleting an **update or DLC** on its own removes just that content and trims
the application's record, leaving the base game installed and launchable.
Deleting a base game removes its updates and DLC with it, which is what the
system itself does.

Folders show the game's real box art as a thumbnail in file managers that ask
for one, which is most of them.

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
  "update_url": "https://api.github.com/repos/Nighthawk42/nx-nexus/releases/latest",
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

`update_url` defaults to this project's GitHub releases feed, and the release
API's own JSON is understood directly — there is no separate manifest to keep in
step with a release. Point it somewhere else and it will also accept:

```json
{ "version": "0.2.0", "url": "https://.../NX-Nexus.nro", "notes": "what changed" }
```

**Check for updates** compares versions and downloads on request. Nothing is
checked automatically. The download lands on a scratch name, is verified to
actually be an NRO before replacing the running one, and the previous build is
kept as `NX-Nexus.nro.bak`.

The file replaced is the one you launched, taken from `argv[0]` — so an install
under `switch/NX-Nexus/` from the Homebrew App Store updates itself in place
rather than writing a second copy somewhere you are not running from.

Being HTTPS, this needs the CA bundle described above.

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

## Installing from the SD card or a game card

**Install from SD card or gamecard** needs no PC at all.

Put files in `sdmc:/nsp` (created on first scan):

| | |
|---|---|
| `Game.nsp` | an ordinary NSP |
| `Game.nsz` | a compressed NSP — installs directly, nothing is unpacked first |
| `Game.xci` / `Game.xcz` | a game card image; the `secure` partition is installed |
| `Game.nsp/` | a **split** NSP: a folder of numbered parts `00`, `01`, `02`… |

The split form exists because FAT32 cannot hold a file over 4 GiB. NX-Nexus
reads the parts itself, so it works whether or not the archive bit is set — the
detail that most often makes a split NSP fail elsewhere.

Nothing is copied or unpacked: bytes go from the file straight into `ncm`, the
same as the USB path.

### From the game card in the slot

The first entry installs the inserted card directly. There is no dump step, so
a 30 GB game needs 30 GB of free space rather than 60.

### NSZ and XCZ

Compressed images install everywhere an uncompressed one does — USB, SD card,
game card and network alike.

This does not need keys, and it is worth knowing why. An NCZ carries the AES key
and counter for each of its sections **in its own header**, put there so
third-party installers could rebuild the NCA without deriving anything. Making
an NSZ needs `prod.keys`; installing one does not.

The block-compressed NCZ variant is refused rather than half-decoded — it is not
a single zstd stream, and feeding it to one would produce a corrupt NCA that
only failed much later.

## Verifying installed content

**Verify installed content** reads every installed NCA back out of `ncm`,
hashes it, and compares the result with the SHA-256 the title's own manifest
records.

That hash covers the NCA exactly as stored — encrypted — so no key material is
involved. It is the cheapest way to answer the question that actually matters
when a game will not launch: are the bytes damaged, or is something else wrong?

Verifying everything takes a while; **B** stops it. **A** on a single title in
the title list checks just that one.

Anything reported as corrupt should be reinstalled. Corruption on an SD card
usually means the card itself is failing, and one bad title is rarely the only
one.

Delta fragments are skipped: the manifest lists them, but they are never
installed, so their absence is normal rather than a fault.

## Mods and cheats

**Mods and cheats** lists everything under `sdmc:/atmosphere/contents` that has
an `exefs`, a `romfs` or a `cheats` folder.

- **A** enables or disables an entry
- **X**, twice, deletes it permanently

Disabling renames the folder so Atmosphère stops looking at it — Atmosphère only
reads directories whose names parse as a 16-character title id. Nothing is moved
or rewritten, and enabling puts the name back. That renaming convention is this
tool's, not an Atmosphère feature, but it is reliable and reversible.

Individual cheats within a file cannot be toggled here: that needs `dmnt:cht`,
which only works against a running game, and the running application is
NX-Nexus.

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

**An NSZ fails with "bad or unsupported ncz".** Almost always the
block-compressed variant, which is refused deliberately. The log names the
reason. Recompress without block mode, or use the plain NSP.

**A title installs but will not launch.** Two likely causes, both of which the
log reports at install time: the title needs newer firmware than the console
runs, or signature patches are not installed. Neither stops the install, because
neither makes the content invalid.

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
