# Architecture

## Layout

```
include/nexus/       public headers
src/
  main.c             entry point, console UI, screen state machine
  nexus_log.c        file + ring-buffer logger
  ui/menu.c          scrolling list widget
  usb/               usb:ds descriptors, endpoints, transfers
  mtp/               container wire format, transaction loop, operations
  storage/           one file per virtual store, plus the registry
  format/            container parsers -- no libnx, host-testable
  installer/         install orchestration + its ncm/es/ns backend
  horizon/           services libnx does not provide
  net/               HTTP client, sources, network install, self-update
tests/               host unit tests
```

## The idea that shapes everything

**Anything that can be tested off-console is kept free of libnx.**

`src/format/` and `src/installer/installer.c` depend only on
`nexus/nx_types.h`, so they compile with an ordinary host compiler and run
under AddressSanitizer and UndefinedBehaviorSanitizer. That covers the code
most likely to be wrong in ways that are painful to debug on hardware: parsers
eating untrusted bytes, and the install sequence's failure and rollback paths.

The parts that genuinely cannot be tested off-console — IPC — are deliberately
kept as thin and boring as possible. `install_horizon.c` is a translation layer
and nothing more; every decision it might have made lives in `installer.c`
instead, where a mock backend can drive it.

Current coverage: **813 assertions**, zero failures.

## Storage backends

Every store implements `NexusStorageOps` (`include/nexus/storage.h`) and is
registered in `src/storage/storage_registry.c`. The MTP layer never changes.

```c
static const NexusStorageOps g_my_ops = {
    .description = my_description,
    .get_info    = my_get_info,
    .enumerate   = my_enumerate,
    .stat        = my_stat,
    .read        = my_read,
    // NULL write hooks make the store read-only; the MTP layer reports
    // StoreReadOnly on its own.
};
```

Read-only stores leave the write hooks NULL. `move` and `copy` back MTP's
`MoveObject`/`CopyObject` and the rename hosts perform through
`SetObjectPropValue(ObjectFileName)`.

## Decisions worth knowing

**No NCA parsing, and therefore no keys.** The entire NCA is encrypted, so a
parser would need the console's key material. It is also unnecessary: content
IDs come from NSP filenames and everything else from the CNMT — which Horizon
decrypts for us once the meta NCA is registered. See
[PLAN-REVIEW.md §8](PLAN-REVIEW.md).

**The virtual NSP.** Extracting a title synthesises a PFS0 header
(`format/nsp_builder.c`) and serves the body from `ncm`. The layout maths is
unit-tested by round-tripping through the real PFS0 parser, because a bad
offset produces a container that only fails much later, at install time.

**Install rollback.** Any failure drops the open placeholder, deletes
registered content, removes the meta record and the application record, and
deletes an imported ticket. Every path is exercised by the mock backend.

**MTP names are UTF-16.** libnx's `utf8_to_utf16` rejects a string containing a
truncated multi-byte sequence, and the caller then emits an *empty* name — which
shows up as an unnamed folder. `nexusSanitiseUtf8` therefore never splits a
character, and `mtpWriteString` falls back to ASCII transliteration rather than
emitting nothing. There is a regression test that truncates non-ASCII strings at
every buffer size from 1 to 40.

**Listing APIs that look paginated but are not.**
`ncmContentMetaDatabaseList` has no offset parameter — it always returns from
the beginning and reports the total separately. Calling it in a loop returns the
same entries repeatedly.

**The USB layer follows libnx's own `usb_comms.c`**, retargeted from the
vendor-specific class to USB Still Image (PIMA 15740), which is what hosts probe
for when mounting MTP.

## Threading

The MTP responder runs on its own thread with blocking USB reads. The main
thread only draws and reads input. Stopping the server flips a flag and calls
`usbTransportCancel()` to unblock the parked read.

Network installs run on the main thread and block the UI on purpose: the screen
says what is happening, and there is no useful work to do meanwhile.

## Adding a store

1. Write `src/storage/storage_yours.c` implementing `NexusStorageOps`.
2. Add a constructor to `include/nexus/storage.h` and an ID constant.
3. Register it in `nexusStorageRegistryInit()`.
4. Add a label in `store_label()` in `main.c` so the UI names it.

Filenames must be unique across every directory in `SOURCES` — the devkitPro
Makefile uses `$(notdir)` with VPATH.

## Building

`scripts/build.sh` runs the devkitPro container. `scripts/test.sh` needs only a
host compiler. CI runs the host tests first on a plain runner so a parser
regression fails in seconds rather than after a full cross-compile.

Test object files go in `tests/obj/`, never beside their sources: the devkitPro
Makefile finds sources through VPATH and would otherwise pick up a host `.o` for
the ARM link.
