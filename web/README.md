# s-mu2000-web

WebAssembly offline render and live playback for S-MU2000 (audio only, no GUI).

Design and verification notes: [doc/web-assembly.md](../doc/web-assembly.md).

## Prerequisites

- Node.js (see `../stb-vorbis` for the tested range) + npm.
- Emscripten (`emcc`/`em++`). Discovery order: `EMCC`/`EMSDK` env, sibling
  `../emsdk`, `~/emsdk`, `/usr/lib/emscripten`, then `PATH`.
- ROMs in `../roms/` (gitignored, never committed): `mu2000_flash.bin`,
  `dump/xv364a0.ic49`, `dump/xv365a0.ic50`, `dump/xw848a0.ic53`,
  `dump/xw849a0.ic54`, `standin/sin-table.bin` (optional).
  Extract them from your own MU2000 — see
  [Preparing the ROMs](https://github.com/tarboh/S-MU2000/blob/main/doc/manual.en.md#2-preparing-the-roms).

## Commands (run in this directory)

```sh
npm install
npm run build # wasm modules, page bundles, then eslint + tsc
npm run build:wasm # only the Node wasm module (out/smu_render.mjs)
npm run build:standalone # only the browser wasm + base64 (out/smu-standalone.*)
npm run build:page # only the page bundles (dist/)
npm run render -- --roms ../roms ../ONTILMOR.mid out.wav 5
npm run serve # serve dist/ over HTTP for the pages
npm run lint
npm run format
```

## Pages

`public/` holds the page sources (`index.html` landing, `render.html`,
`live.html`, `play.html`, `style.css`); `npm run build:page` emits the
deployable `dist/` (minified bundles + sourcemaps, worklet processor).
The wasm binary is embedded as base64, so `dist/` needs no separate fetch.

- Render: pick your ROM folder and a MIDI file, Render with a progress bar,
  then play or download the WAV.
- Live: pick your ROM folder, Boot synth, Enable MIDI, play from up to 4
  WebMIDI inputs (ports A–D) with activity dots and a Panic button.
- Play: pick your ROM folder, Boot synth, pick a MIDI file, play it
  through the live synth with a playback progress bar. Picking another
  file replaces the song immediately; Play replays, Stop stops.

Needs HTTP (`npm run serve`); `file://` cannot load the worklet. ROMs are
picked once and stored locally in your browser (IndexedDB) — the Forget
button clears them. Nothing is ever uploaded anywhere.

## Debugging live playback

The synth runs in the worklet, synchronously: this needs a fast CPU
(desktop Chromium measures about realtime), but MIDI then applies
within one quantum — no grouping, no lookahead lag. Both bundles log
a build tag first (`src/browser/build-tag.ts`). After pressing Boot
synth the console must show, in order:

```text
[live] live-7 creating AudioContext
[worklet] processor live-7, sampleRate=48000
[worklet] wasm module loaded
[worklet] init with 6 roms
[worklet] reset done, booting across quanta
[live] worklet message: boot        <- heartbeat, then Booting… N s
…
[live] worklet message: live
```

The live quantum path is deliberately log-free: at ~345 quanta/s even
cheap logging and extra buffer passes cost audible glitches. The
remaining worklet logs all fire at most once per session; failures
still surface as `Synth error:` status on the page. The Test tone
button bypasses the synth entirely: audible tone plus silent synth
points at the worklet path; silent tone points downstream (tab mute,
device, policy — browsers mute per origin, so check Unmute site and
Sound permissions for this port).

What the cutoff tells you:

- No `[worklet] processor …` line: stale `dist/` (rebuild + hard reload),
  or the worklet module failed to load — check for load errors above it.
- `init` without progress: page-to-worklet delivery or ROM problem;
  the `init failed:` line names it.
- Heartbeat `boot` missing but reset done: worklet-to-page delivery is
  broken (port wiring), even though the worklet is alive.
- Progress stalls mid-boot: the synth is still grinding (slow machine);
  only an `error` post means real failure.
- Audible dropouts on a slow machine: every quantum misses its ~3 ms
  budget and Chromium discards the audio (Firefox plays late buffers).
  A worker render-ahead design was tried for this and reverted: it
  survives dropouts but delays MIDI by its ~1 s lookahead and groups
  events. Fast CPU is the requirement for this design.

CLI equivalent (same worklet bundle, stubbed AudioWorklet globals with
the real `process(inputs, outputs)` arity, real ROMs, no browser):

```sh
npm run harness
```

It boots across 128-frame quanta exactly like the page, then plays a
note and checks the output is non-silent. `HARNESS OK` with a silent
browser points at the browser/audio path, not the emulation.

Song harness (same bundle, whole MIDI file fed as timed live MIDI —
dense polyphony, CCs, XG SysEx, multi-port routing):

```sh
npm run harness:song -- /path/to/song.mid [--seconds 30] [--fast 0|1] [--roms ../roms]
```

Reports realtime factor, avg/max quantum ms, and overruns over the
`~2.9 ms` quantum budget. Defaults are exact emulation with a 30 s
cap; pass `--fast 1` for the fast synth (the live page default).
The SMF parser covers tempo, `FF 21` port prefix, and the Yamaha
port meta; files stay outside the repo.

## Layout

- `scripts/build.ts` — Node wasm module (`out/smu_render.mjs`).
- `scripts/build-standalone.ts` — browser wasm + base64 (`out/smu-standalone.*`).
- `scripts/build-page.ts` — page bundles (`dist/`).
- `scripts/emu.ts` — shared source list, emcc discovery, compilation.
- `scripts/worklet-harness.mjs` — CLI worklet test (`npm run harness`).
- `scripts/worklet-song-harness.mjs` — whole-song worklet stress (`npm run harness:song`).
- `src/render.ts` — Node CLI: MIDI file to WAV file.
- `src/smu-types.ts` — module interface shared by both loaders.
- `src/browser/smu-standalone.ts` — base64 wasm loader (pages + worklet).
- `src/browser/processor.ts` — synth worklet (`dist/smu-processor.js`).
- `src/browser/protocol.ts` — page/worklet message types.
- `src/browser/smf.ts` — SMF parser for the play page (mirrors `src/smf.cpp`).
- `src/browser/play-app.ts` — MIDI playback page (`dist/play.js`).
- `src/browser/roms.ts` + `idb.ts` — ROM picker card + local persistence.
- `src/wasm/` is intentionally **not** here: the C++ glue
  (`../src/wasm/wasm_render.cpp`) lives with the other C++ frontends —
  see `doc/web-assembly.md`.
- `out/` — build outputs (ignored).
