# Release automation (continuous build)

Rolling binaries for users who don't build from source.
One `continuous` prerelease, overwritten on a schedule. Versioned releases stay manual (`v*` tags).

## Decisions

- **One `continuous` release, not per-push.** The repo does ~10-100 commits/day. Per-push
  releases would spam tags, confuse users ("which of 13 today's builds?"), and burn Actions
  minutes. `build.yml` still validates every push; only publishing is scheduled.
- **Schedule: every 10h + manual.** `cron: 0 */10 * * *` (~2-3 builds/day, always <1 day stale)
  plus `workflow_dispatch`. No `on: push` publishing. A `freshness` job compares
  `refs/tags/continuous` with the commit being built and skips scheduled runs when
  main hasn't moved (manual dispatches always build).
- **Tests gate publishing.** Every packaging job runs `make test` first (Linux under
  `SDL_VIDEODRIVER=dummy`, like `build.yml`); `publish` needs all three jobs, so a
  broken main never overwrites the downloads.
- **The `continuous` tag is force-moved to the built commit each publish.**
  `softprops/action-gh-release` only replaces assets/release metadata in place — its
  update path (`src/github.ts`, `repos.updateRelease`) never moves `refs/tags/continuous`
  itself, which would leave the tag and the auto-generated "Source code" archives stuck
  on the first build. So `publish` does `git tag -f continuous <sha> && git push -f`
  before uploading, and also passes `target_commitish`.
- **Pinned toolchain.** Third-party actions are pinned to commit SHAs and appimagetool
  to fixed release `1.9.1` (not its `continuous` channel, which has shipped breakage —
  it even carries a `broken-zsyncmake` tag). Pinning the *packager* never freezes the
  *app*: the app is still built fresh from main every run.
- **Three assets:** `S-MU2000-windows-x64.zip`, `S-MU2000-macos-universal.zip`,
  `S-MU2000-linux-x64.tar.gz` + `S-MU2000-x86_64.AppImage` (gui). All from `make all`
  (+ `make clap` on macOS). No 32-bit, no AUv3.
- **AUv3 dropped from releases.** It only sings with ROMs baked in (`make auv3 AUV3_ROMS=roms`,
  `Makefile` + `doc/auv3.md`), which we can never redistribute, and it needs a signed
  `.appex` inside an app. VST3/AU/CLAP cover all hosts; AUv3 stays local-build only.
- **Linux = AppImage + tarball.** Raw `build-linux/` binaries need
  `libasound/cairo/fontconfig/sdl3` on the host. The AppImage bundles them so `gui` runs
  anywhere; the tarball covers `render`/`live`/plug-ins for DAW/CLI users.
  Note: CI's `build.yml` SDL3 is dummy-only (`X11/WAYLAND=OFF`). The release build compiles
  SDL3 with video backends ON, otherwise the shipped `gui`/AppImage shows nothing.

## Layout (all OSes)

```
S-MU2000-<os>-<arch>/
  LICENSE.txt          # copy of LICENSE (BSD-3 requires it in binary dists)
  NOTICE.txt            # copy of NOTICE.txt (vendored code attributions)
  README-release.txt    # generated: WIP/unsigned warning, version, ROM link, platform notes
  roms.txt.example      # one-line path template, never real ROMs
  bin/                  # runnable as-is, no build tree needed
  plugins/              # DAW formats
```

| OS | `bin/` | `plugins/` |
|---|---|---|
| Windows x64 (MSYS2 mingw64, `-static`) | `gui.exe render.exe live.exe panel.exe verify.exe boot.exe statetest.exe blocktime.exe midisend.exe rec.exe vst3probe.exe clapprobe.exe vstiprobe.exe` | `S-MU2000.vst3/` `S-MU2000.clap` `S-MU2000.dll` (VST2) |
| macOS universal (`UNIVERSAL=1`, `MACOSX_DEPLOYMENT_TARGET=11.0`) | `gui live render panel verify boot statetest blocktime vst3probe aubprobe clapprobe` | `S-MU2000.vst3/` `S-MU2000.clap/` `S-MU2000.component/` |
| Linux x64 | same as macOS minus `aubprobe`, plus AppImage (below) | `S-MU2000.vst3/` `S-MU2000.clap` |

`art/real/panel.txt`+PNGs and `doc/vst3-readme.txt` are already copied inside the VST3/AU
bundles by the Makefile; don't duplicate them at top level. `xgtest/fx_probe/fxsweep/`
and other dev tools are not in `all` and not shipped. ROMs, `bootcache/`, `nvram/`,
`log.txt` are never shipped (see `README.md`: hardware-derived data is not distributed).

`README-release.txt` points at `doc/manual.en.md` (ROM extraction, `roms.txt` placement,
`S_MU2000_ROMS`) plus per-OS notes:

- Windows: runs from Explorer/PowerShell, no MSYS2 needed.
- macOS: unsigned/ad-hoc — `xattr -dr com.apple.quarantine <bundle>` + right-click Open
  on first run; VST3 → `~/Library/Audio/Plug-Ins/VST3`, AU → `.../Components`.
- Linux: `chmod +x AppImage`; tarball needs `libasound2` on very minimal hosts;
  `gui` creates one ALSA sequencer port (`S-MU2000`).

## Workflow

`.github/workflows/release.yml`:

- `on: schedule: [cron: 0 */10 * * *], workflow_dispatch:`
- `concurrency: group: continuous, cancel-in-progress: false` (don't overlap publishes).
- Jobs `pkg-windows` (msys2 mingw64), `pkg-linux` (ubuntu + full SDL3 + appimagetool),
  `pkg-macos` (`UNIVERSAL=1`). Each: build → `scripts/package-<os>.sh <build-dir> <dist-dir>`
  → `actions/upload-artifact`.
- Job `publish` (needs all three, `ubuntu-latest`): download artifacts, stamp
  `README-release.txt` with SHA/date, zip/tar, push to tag `continuous` with
  `softprops/action-gh-release` (`prerelease: true, make_latest: true, files: ...`).
  Versioned releases: same packaging scripts, triggered by `push: tags: v*` (future work).

## Linux AppImage detail

`scripts/package-linux.sh` builds an `AppDir`:

```
S-MU2000.AppDir/
  AppRun                # execs $APPDIR/usr/bin/gui "$@"
  S-MU2000.desktop
  S-MU2000.png          # from art/ or doc screenshot
  usr/bin/gui live render panel verify ...   # + bundled *.so* under usr/lib/
```

Deps collected with `ldd` on `gui` (+ `libSDL3/cairo/asound/fontconfig` chain) and copied
in; loader/`libc`/`libm`/NSS and the whole display stack (`libX*`, `xcb`, GL/EGL/Vulkan,
Wayland, `xkbcommon`) are excluded and come from the host — bundling those breaks other
distros. `appimagetool` produces `S-MU2000-x86_64.AppImage`. Only `gui` is the AppImage entry;
CLI/plug-in users take the tarball. AppImage is `chmod +x` and runs without `apt install`.

## Testing

- `act workflow_dispatch -j pkg-linux` (docker) for the Linux path; Windows/macOS jobs
  can't run under `act`/docker — test their packaging scripts directly:
  `bash scripts/package-windows.sh <fake-build> <dist>` etc. in WSL/MSYS2.
- `shellcheck scripts/package-*.sh`, `actionlint`/YAML parse of the workflow.
- Smoke: `verify` (no ROMs), `gui --selftest` (Linux, dummy driver), `vst3probe/clapprobe`
  load checks where ROMs exist locally (never in CI).
- Verify zips contain `LICENSE.txt/NOTICE.txt`, `bin/` runs (`--help`/`--list`),
  bundle `Info.plist` (mac) present, AppImage `chmod +x` + `--appimage-extract` works.
