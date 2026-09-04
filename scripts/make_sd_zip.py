#!/usr/bin/env python3
"""Builds the SD-card zip an end user extracts to the root of their card.

Run from the repository root, after a build:

    ./scripts/build.sh --strict
    python scripts/make_sd_zip.py

The sources.json template is lifted out of src/net/sources.c rather than
retyped, so the file shipped in the zip can never drift from the one the app
writes for itself on first run.
"""
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NRO = ROOT / "NX-Nexus.nro"

# Matches the layout in packaging/manifest.install, so an App Store install and
# a manual one end up in the same place. Self-update follows argv[0], so either
# works -- but having one canonical answer avoids two copies on one card.
APP_DIR = "switch/NX-Nexus"
CFG_DIR = "switch/nx-nexus"


def app_version() -> str:
    text = (ROOT / "include" / "nexus" / "update.h").read_text(encoding="utf-8")
    m = re.search(r'#define\s+NEXUS_VERSION\s+"([^"]+)"', text)
    if not m:
        sys.exit("could not find NEXUS_VERSION in include/nexus/update.h")
    return m.group(1)


def default_update_url() -> str:
    text = (ROOT / "include" / "nexus" / "sources.h").read_text(encoding="utf-8")
    marker = "#define NEXUS_DEFAULT_UPDATE_URL"
    if marker not in text:
        sys.exit("could not find NEXUS_DEFAULT_UPDATE_URL")

    # The macro wraps onto a continuation line, so take the first quoted string
    # after it rather than trying to match the whole definition.
    tail = text.split(marker, 1)[1]
    start = tail.index('"') + 1
    return tail[start:tail.index('"', start)]


def sources_template() -> str:
    """Reassembles the C string literal that sources.c writes on first run.

    A plain line scan, deliberately: the obvious multi-line regex for a run of
    adjacent string literals backtracks catastrophically on this file.
    """
    lines = (ROOT / "src" / "net" / "sources.c").read_text(encoding="utf-8").splitlines()

    # The template is the run of quoted lines between the assignment and its
    # terminating semicolon.
    try:
        first = next(i for i, l in enumerate(lines) if l.strip() == '"{\\n"')
    except StopIteration:
        sys.exit("could not find the sources.json template in src/net/sources.c")

    pieces = []
    for line in lines[first:]:
        stripped = line.strip()
        pieces.extend(re.findall(r'"((?:[^"\\]|\\.)*)"', stripped))
        if stripped.endswith(";"):
            break
    else:
        sys.exit("the sources.json template is not terminated")

    body = "".join(pieces).replace("NEXUS_DEFAULT_UPDATE_URL", "")

    return (body.replace("\\n", "\n")
                .replace('\\"', '"')
                .replace("\\\\", "\\")
                .replace('"update_url": ""',
                         f'"update_url": "{default_update_url()}"'))


README = """NX-Nexus {version}
==================

An MTP server and streaming installer for the Nintendo Switch.


INSTALLING
----------

Extract this zip to the ROOT of your SD card, keeping the folder structure.
You should end up with:

    /switch/NX-Nexus/NX-Nexus.nro     the app
    /switch/nx-nexus/sources.json     its configuration
    /nsp/                             drop installable files here

Then launch NX-Nexus from the homebrew menu.

Nothing is exposed over USB until you ask for it -- pick "Start MTP server"
from the menu when you want the console to appear on a computer. The header
line always says whether the server is running.


INSTALLING GAMES
----------------

Over USB: start the server, then copy an .nsp or .nsz into the "MicroSD
Install" or "System Install" store from your file manager. Nothing is staged
on the SD card; the bytes go straight into the system's content storage.

Without a computer: put .nsp, .nsz, .xci or .xcz files in /nsp on the card and
use "Install from SD card or gamecard". Split NSPs -- a folder of numbered
parts 00, 01, 02 -- work whether or not the archive bit is set.

From the game card in the slot: same menu, first entry. There is no dump step,
so a 30 GB game needs 30 GB of free space rather than 60.


HTTPS (NETWORK INSTALLS AND AUTO-UPDATE)
----------------------------------------

Both need a CA certificate bundle, which is deliberately NOT included here --
a bundled copy goes stale and keeps trusting authorities that have since been
withdrawn. Download a current one and put it at:

    /switch/nx-nexus/cacert.pem

    https://curl.se/ca/cacert.pem

Without it, HTTPS is refused rather than quietly downgraded. That is on
purpose: this tool installs executables, so an unverified connection would let
anyone on the network path choose what gets installed.


CONFIGURATION
-------------

/switch/nx-nexus/sources.json holds your own content servers and the
auto-update feed. It ships with no sources -- add your own.

Public "free shops" that redistribute other people's games are not supported
and not endorsed.


LOGS
----

/switch/nx-nexus/nx-nexus.log, and the Log screen in the app.


A WORD OF WARNING
-----------------

This is early software. USB browsing is confirmed working on real hardware;
most of the rest builds clean and is unit-tested where that is possible off
the console, but has not been exercised end to end.

The firmware installer is the highest-risk part by a distance. It is refused
outright unless you booted from an emuMMC, and it has never been run against a
real system partition. Do not use it on anything you cannot restore from a
NAND backup.


LICENCE
-------

GPL-3.0-or-later, with ABSOLUTELY NO WARRANTY. Source, including the complete
corresponding source for this build:

    https://github.com/Nighthawk42/nx-nexus
"""

NSP_README = """Put installable files in this folder.

    Game.nsp        an ordinary NSP
    Game.nsz        a compressed NSP -- installs directly, nothing is unpacked
    Game.xci        a game card image
    Game.xcz        a compressed game card image
    Game.nsp/       a split NSP: a folder of numbered parts 00, 01, 02, ...

Then pick "Install from SD card or gamecard" in NX-Nexus.

This file is ignored by the scanner and can be deleted.
"""


def main() -> None:
    if not NRO.exists():
        sys.exit("NX-Nexus.nro not found -- run ./scripts/build.sh first")

    version = app_version()
    out = ROOT / f"NX-Nexus-{version}-sdcard.zip"
    if out.exists():
        # A previously sent zip can still be held open by the client that is
        # displaying it; writing a fresh name beats failing outright.
        import time
        try:
            out.unlink()
        except OSError:
            out = ROOT / f"NX-Nexus-{version}-sdcard-{int(time.time()) % 100000}.zip"

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("README.txt", README.format(version=version))
        z.write(NRO, f"{APP_DIR}/NX-Nexus.nro")
        z.writestr(f"{CFG_DIR}/sources.json", sources_template())
        z.writestr("nsp/README.txt", NSP_README)

    print(f"wrote {out.name} ({out.stat().st_size} bytes)")
    with zipfile.ZipFile(out) as z:
        for info in z.infolist():
            print(f"  {info.file_size:>9}  {info.filename}")


if __name__ == "__main__":
    main()
