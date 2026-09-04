# Homebrew App Store packaging

The App Store is how most people actually find Switch homebrew, so it matters
more for reach than any feature.

## What CI produces

Every tagged build attaches two files to the GitHub release:

| File | For |
|---|---|
| `NX-Nexus.nro` | manual install — copy anywhere under `sdmc:/switch/` |
| `NX-Nexus.zip` | the App Store package |

The zip is laid out as a fragment of the SD card, with `manifest.install` at
its root listing what goes where:

```
manifest.install
switch/NX-Nexus/NX-Nexus.nro
```

`U:` marks a file the store may overwrite on update. Nothing else is shipped —
`sources.json` and `cacert.pem` are created or supplied by the user, and an
update must never overwrite either.

## The self-update interaction

The store installs to `switch/NX-Nexus/NX-Nexus.nro`, while a manual install
usually lands at `switch/NX-Nexus.nro`. NX-Nexus reads `argv[0]` at startup and
updates *that* file, so both installs update themselves in place and the two
never fight over one path.

## Submitting

Listing is a pull request against
[fortheusers/hb-appstore-packages](https://github.com/fortheusers/hb-appstore-packages),
not something the build can do. A submission needs:

- `pkgbuild.json` pointing at this repository's releases
- `icon.png` — 256×256, in this directory
- `screen.png` — a screenshot of the running app, **not yet captured**

Take the screenshot from real hardware on the main menu with the server
running, so the store listing shows the app doing its job rather than an idle
splash.
