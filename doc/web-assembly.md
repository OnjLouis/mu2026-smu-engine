# WebAssembly port — audio only (no GUI)

Compiling the S-MU2000 audio core to WebAssembly.
Out of scope: panel/GUI, VST3/CLAP/AU, DAW plugins in browser.

## Status

- [x] **Step 1 — Node render.** `emcc` build of the core + thin glue that renders MIDI to WAV under Node. ROMs and MIDI passed in as memory buffers (done 2026-09-29, see "Step 1" below).
- [x] **Step 2 — Web render.** Browser UI: upload ROMs + MIDI, render offline, download/play WAV. Reuses the Step 1 module (done 2026-09-29, see "Step 2" below).
- [x] **Step 3 — Web live.** WebMIDI inputs (up to 4 → ports A–D) +
  synchronous worklet synth, direct MIDI response (done 2026-09-30;
  fast CPU required, dropouts on slower hardware).

Test data: ROMs in `roms/` (gitignored, never committed). Example MIDI: `ONTILMOR.mid` (repo root).

## Tool status (checked 2026-09-29)

| Need                         | Status                                                           |
| ---------------------------- | ---------------------------------------------------------------- |
| `emcc 6.0.9`                 | present (`C:/.../emsdk/upstream/emscripten/`)                    |
| `node v26.7.0`               | present                                                          |
| `cmake 4.4.3`, `python 3.13` | present                                                          |
| `g++`                        | missing on this machine — not needed, wasm path uses `emcc` only |
| ROMs                         | in `roms/` (gitignored, correct)                                 |
| Test MIDI                    | present (`ONTILMOR.mid`)                                         |

Nothing to download.

## Why the core should compile almost as-is

- **JIT off on wasm32** (confirmed by building). `src/mame/cpu/sh2_jit.cpp:36` enables the SH2 JIT only on `__x86_64__`/`__aarch64__`/`_M_X64`; `src/mame/sound/swp30_jit.cpp:31-36` follows the same pattern. `emcc` targets wasm32 (`__wasm32__`/`__EMSCRIPTEN__`), so both fall back to `SMU2000_*_JIT 0` = interpreter path (`sh.cpp`, `swp30.cpp`). `compat/exec_mem.h` (`mmap`/`VirtualAlloc`/`MAP_JIT`) is then uncompiled — no source changes were needed here.
- **Threads avoidable.** `mu2000::set_threaded(false)` keeps everything single-threaded (`render.cpp:354` already does this with `--single`). The `std::atomic::wait/notify` slave path (`mu2000.cpp:341-373`) is then never entered. So **no `-pthread`, no SharedArrayBuffer/COOP/COEP** for steps 1–2.
- **Platform shims are no-ops.** `compat/platform.h:76` (`denormals_off`) is guarded by `__SSE2__`/`__x86_64__` — inactive under wasm. `perf_ticks()` falls back to `steady_clock`. `getenv` returns null, fine.
- **Two gaps to close with small new code (no core edits):**
  1. ROM/MIDI loading is `fopen`-path based (`mu2000::load_program/load_wave`, `smf::load(path)`). Wasm receives `ArrayBuffer`s from JS, so add: feed `set_program_rom/set_wave_rom/set_sintab_rom` (`mu2000.h:52-56`) from vectors + a wave-interleave helper (4×8 MB → 32 MB, logic copied from `mu2000.cpp:414-443`) + `smf::load_from_memory(const u8*, size_t)` (split file read from parse in `smf.cpp:61-182`).

## Step 1 — Node render (done 2026-09-29)

Files: `src/wasm/wasm_render.cpp`, `web/src/render.ts`, `web/scripts/build.ts`
(`npm run build:wasm` in `web/`), TS + eslint + prettier suite copied from
`../stb-vorbis`.

Run from `web/`: `npm run build:wasm`, then
`npm run render -- --roms ../roms ../ONTILMOR.mid out.wav 5`.

Verified: 5 s and 20 s of `ONTILMOR.mid` render **byte-identical** to native
`build/render.exe --single` (interpreter == JIT). Speed ~0.7× realtime at
`-O3` (link) / `-O2` (objects). The build was also validated on Linux (WSL,
system emscripten 6.0.9), which caught and fixed an `em++` discovery bug
(Debian layout uses `em++`, not `emcc++`).

New files:

- `src/wasm/wasm_render.cpp` — C API with `EMSCRIPTEN_KEEPALIVE`, no embind dependency:
  - `smu_init()` — create one `mu2000`, `set_threaded(false)`, `set_usb_host(...)`.
  - `smu_set_rom(kind, ptr, len)` — kinds: program (4 MB), wave0–3 (8 MB each, interleaved on finalize), sintab (64 KB). Returns error code, `smu_error()` string for details.
  - `smu_reset()` — `mu.reset()` after all ROMs set.
  - `smu_boot(max)` / query `smu_midi_ready()` — boot-wait loop mirror of `render.cpp:376-393`. `smu_run_blank(n)` runs n samples discarding output (chunked boot for pages/worklet).
  - `smu_load_midi(ptr, len)` — parse SMF from memory, store event list.
  - `smu_midi_in(port, bytes...)` — live path (also used by step 3).
  - `smu_render_frames(out_ptr, nframes, ...)` — feed due MIDI events by sample clock, `run_sample` loop (`render.cpp:571-577`), write interleaved int16 stereo.
- `web/scripts/build.ts` (`tsx scripts/build.ts`) — incremental `emcc` objects to `web/out/obj/`, `em++` link to `web/out/smu_render.mjs` (+ generated `smu_render.d.mts`): `-O3 -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_ES6=1`, core sources + `wasm_render.cpp` + `smf.cpp`, no `-pthread`, no SDL. `emcc`/`em++` discovery mirrors stb-vorbis (`EMCC`/`EMSDK` env, sibling `../emsdk`, `~/emsdk`, `PATH`).
- `web/src/render.ts` — Node driver: read ROM/MIDI files, call the module, write WAV. Types for the generated module in `web/src/smu-types.ts`.
- `smf::load_from_memory` — refactor: keep `load(path)` as read-file + new `load_from_memory` parse entry.

Test: `npm run render -- --roms ../roms ../ONTILMOR.mid out.wav`, byte-compare vs native `build/render.exe` output (interpreter vs JIT must be bit-exact). `blocktime` gives the expected ~5–8× slowdown baseline.

## Step 2 — Web render (done 2026-09-29)

Files: `web/public/render.html` (page source), `web/src/browser/render-app.ts`
(bundled minified by `web/scripts/build-page.ts` into `web/dist/render.js`),
`web/src/common/wav.ts` (WAV encoder shared with the CLI),
`web/src/browser/roms.ts` + `idb.ts` (ROM picker card + local persistence).

Run from `web/`: `npm run build:page`, then serve `dist/` over HTTP
(`npm run serve`, or `python3 -m http.server -d dist`) and open the page.
The page: one ROM folder picker (`webkitdirectory`, files matched by basename
so `roms/` drops in as-is; sine table optional), one MIDI picker, Render
button, progress bar, `<audio>` preview + WAV download. Picked ROMs persist
in IndexedDB (Forget button clears them). Render is chunked on the main
thread (2 s audio per chunk, `setTimeout` yields); boot uses 1-sample
`smu_run_blank` steps so it ends on the exact ready sample. The synth
boots on USB ports like the desktop default, so song ports 1-4 reach
A-D discretely.

Notes:

- The browser uses a standalone wasm (`build-standalone.ts`: same sources,
  `-sSTANDALONE_WASM=1`, no JS glue), embedded as base64 and loaded by
  `src/browser/smu-standalone.ts` — no `.wasm` fetch, so AudioWorklets work
  and `file://` is only blocked by worklet loading. The Node CLI keeps the
  glue module (`out/smu_render.mjs`).
- Manual instantiation must call the `_initialize` export (static
  constructors). Miss it and `attotime::never` reads as zero, so
  never-expiring timers fire immediately: the synth boots but stays silent.
  Found by comparing SWP register streams (identical) against render output
  (silent), then bisecting link flags to exonerate the optimizer.
- Boot exactness matters: chunked blank-boot overshoots the ready sample by
  up to one chunk (1827 samples observed), which audibly changes the render
  (108k of 441k samples differed, max 2942). 1-sample steps cost ~7 s for the
  ~351k-sample boot and render bit-identical to the CLI.

## Step 3 — Web live (2026-09-29, direct worklet design, needs fast CPU)

Files: `web/public/live.html`, `web/src/browser/live-app.ts` (page),
`web/src/browser/processor.ts` (synth worklet, bundled to
`dist/smu-processor.js`), `web/src/browser/protocol.ts` (page/worklet
message types), `web/src/browser/build-tag.ts` (shared build tag logged
by both bundles), `web/scripts/worklet-harness.mjs` (`npm run harness`).

The synth instance lives in the worklet and renders synchronously, one
128-frame quantum at a time: the page posts ROMs (transferred), then MIDI
bytes per port, and MIDI applies within the same quantum — no grouping,
no lookahead lag. Boot runs chunked across quanta (512-sample slices,
silence out, progress posted back roughly once per emulated second).
The synth boots on USB ports like the desktop default, so WebMIDI
inputs on ports C-D sound instead of going quiet.
The wasm module loads via static `import` of the loader (dynamic
`import()` is disallowed on `WorkletGlobalScope`); the AudioContext
opens at the device native rate with a linear-resampling FIFO for
non-44100 rates. This design needs a fast CPU (desktop Chromium measures
about realtime); slower machines miss every quantum and Chromium
discards the audio while Firefox plays late buffers.

WebMIDI inputs map to ports A–D via per-device dropdowns (first four
default to A–D, several devices may share a port); per-port activity dots,
message counters and a last-message readout visualize traffic; Panic sends
all-notes-off on every channel of every port. A Test tone button (plain
oscillator, bypasses everything) tells downstream silence (device/mute/
policy — browsers mute per origin) apart from synth silence.

Validated with the worklet harness in Node (real ROMs, real
`process(inputs, outputs)` arity): boot → live → note-on (audible,
rms ~0.02) → `HARNESS OK`. Played from real browsers and a MIDI file
player: Firefox fine; Chromium fine on a Ryzen 5800X3D, dropouts on
slower hardware.

Worker render-ahead detour (tried 2026-09-30, reverted same day): synth
in a Worker rendering ~1 s ahead, thin worklet player with ack pacing.
Survived Chromium dropouts on slow hardware, but MIDI lagged up to the
lookahead and grouped into bursts (queue dumped per chunk), which is
worse for live playing than occasional dropouts on fast hardware.
Reverted to the direct design; nothing of it remains in the tree.

First design (AudioWorklet owns the synth) and what it taught:

- Boot ran a full second of emulation per `process()` call
  (`_smu_run_blank(44100)` ≈ 470 ms wall on a fast desktop, 160× the
  ~2.9 ms quantum budget). Both browsers stopped calling the processor:
  stall with no error and not even one progress message. Fix was 512
  samples per quantum — but that only cured boot, not live rendering.
- `process(outputs)` signature bug: the browser calls
  `process(inputs, outputs, parameters)`, so the single parameter
  received `inputs` (always `[]` with `numberOfInputs: 0`) and boot
  never ran anywhere. The Node harness masked it by passing buffers as
  the first argument. Always drive test doubles with the real arity.
- `performance.now()` is absent from the AudioWorklet scope in some
  browsers (`ReferenceError`); slice timing used `Date.now()`.
- Measured live cost 13–36 ms per 128-frame quantum (4–12× over
  budget) on Chromium: synth rendered (peaks to 0.23) yet zero sound.
  Firefox plays late buffers; Chromium discards them — total dropout
  from deficit that ~1× emulation can never repay. Hence the worker
  redesign; no JS-gluing survives a 5× deficit.
- The SH2/MEG JITs cannot run under WebAssembly: they emit native
  machine code into executable memory, which the sandbox forbids
  (both already compile to 0 on wasm32). Codegen is maxed (`-O3`);
  speed must come from architecture (render-ahead), not flags.

## Step 4 — Web MIDI playback (mix of live and render)

Files: `web/public/play.html`, `web/src/browser/play-app.ts` (page),
`web/src/browser/smf.ts` (SMF parser mirroring `src/smf.cpp`),
`web/src/browser/protocol.ts` (`play`/`stop`/`song` messages).

The page boots the worklet synth like the live page (ROM picker, fast
synth on by default, chunked boot with progress, USB ports so all four
parts sound discretely) but takes a MIDI file like the render page —
no WebMIDI. The file is parsed on the page (`smf.ts`: tempo, `FF 21`,
Yamaha port meta, track/device names, `F5` overrides folded USB-style)
and posted to the worklet, which feeds due events into its MIDI queue
each quantum on the sample clock and posts playback position back
(~10 Hz) plus a done message after a 2 s tail. Picking another file
replaces the song immediately; Stop clears it with all-notes-off and
Play replays it. No wasm change was needed: scheduling rides the
existing live `midi_in` path.

## Improving performance — preface, problems, options

Preface: offline render only needs throughput (`~0.7x` realtime is
usable, just slow). Live needs every 128-frame quantum finished
inside `~2.9 ms` with margin — average is not enough, the worst
quantum sets the dropout rate. All options below keep the default
bit-exact emulation; faster-but-different paths (`native-fx`,
`native-engine`) are opt-in and measured separately.

Problems (measured, not guessed):

- Interpreter-only. `src/mame/cpu/sh2_jit.cpp:36`,
  `src/mame/sound/swp30_jit.cpp:31-36` compile to 0 on wasm32, so
  `emcc` runs the `sh.cpp`/`swp30.cpp` interpreter. Native baseline
  is `~5-8x` slower without JIT (`doc/benchmarks.md`,
  `doc/todo.md` §6); Node render logs `~0.7x` realtime.
- Firmware dominates. Per-sample breakdown on Ryzen 7 9700X
  (`doc/native-dsp.md`, `doc/native-engine.md` §1): SH-2 firmware
  `~2.3 us (55%)`, SWP30 voices `~1.4 us (33%)`, MEG effects
  `~0.5 us (12%)`. Killing MEG alone saves `~10-14%`.
- Quantum deficit. Live measures `13-36 ms` per 128-frame quantum
  (`4-12x` over budget) on Chromium; boot slice
  `_smu_run_blank(44100) ~= 470 ms` wall (`160x` budget). Chromium
  discards late quanta, Firefox plays them late — neither repays a
  `5x` deficit.
- Single thread. `wasm_render.cpp` calls `set_threaded(false)`, no
  `-pthread`, no `SharedArrayBuffer`/COOP/COEP by design. Slave
  SWP30 runs inline.
- JS/wasm glue per quantum (`web/src/browser/processor.ts`):
  `_malloc/_free` per MIDI message, fresh `HEAPU8`/`Int16Array`
  views per call, `fifoLeft/Right: number[]` with `push/splice` plus
  per-sample linear resample for non-44100 devices, `1/32768`
  scaling per sample.
- C++ glue per sample (`src/wasm/wasm_render.cpp:260-291`):
  `double(g_rendered+f)/kRate` time check, branch, and
  `l*32768/DAC_FULL_SCALE` + `clamp` for every frame.
- Build flags. Objects compile `-O2` (`web/scripts/build.ts`),
  standalone links `-O3` (`web/scripts/build-standalone.ts`), no
  `-flto`, no `-msimd128`, `ALLOW_MEMORY_GROWTH=1` (bounds checks).
- Boot cost. `~351k`-sample boot; page render uses 1-sample
  `smu_run_blank` steps for bit-exactness (`~7 s`), worklet uses
  512-sample slices. Every boot starts from scratch — no
  `bootcache.h`/`nvram.h`/`voicecache` persistence yet.

Potential solutions, highest ROI first:

1. Expose `native-engine` (+ `native-fx-full`) to wasm. Native
   runs SH-2 at `~1/6.5` (`2145 ns -> 329 ns`) and renders real
   songs `2.3-3.6x` faster, `~4.2x` with lightweight FX and
   bootcache (`doc/native-engine.md` §6.20, §6.39). Needs new
   `smu_set_native_engine/fx` exports in `wasm_render.cpp`, updated
   `STANDALONE_WASM` export list, `smu-types.ts`, and a page/worklet
   toggle; keep the `100 ms / 5 ms` firmware keepalive for LCD/panel
   and persist `voicecache` in IndexedDB next to ROMs. This is the
   only option that removes the `2.3 us` SH-2 term instead of
   shrinking it.
2. Build flags. Unify objects at `-O3 -flto -msimd128 -fno-rtti`,
   try fixed `INITIAL_MEMORY` without growth for the live module,
   keep `-fno-exceptions` for standalone. Re-measure with the Node
   render `realtime` line; expect percent-level, not multiples.
3. Glue cleanup (no emulation change). Batch MIDI into one
   `_malloc` per quantum, preallocate persistent `outPtr`/views,
   move resampling into C++ (or request `44100 Hz` `AudioContext`
   where the device allows it), precompute MIDI event sample
   indices in `smu_load_midi` instead of per-sample doubles.
4. Small-lookahead worker (revisit). The reverted `~1 s` Worker
   survived dropouts but lagged MIDI up to the lookahead and burst.
   A `2-3` quantum (`~6-9 ms`) FIFO with sample-stamped MIDI would
   keep latency near one video frame while absorbing worst quanta.
   Full `-pthread` + slave thread is the heavier variant (needs
   COOP/COEP); measure after (1).
5. Boot persistence. Cache NVRAM/boot state locally so repeat boots
   skip the `~8 s` firmware bring-up. Deferred so far because saved
   RAM changes every boot and rarely hits.

What likely does not pay:

- Hand-rewriting 100+ MEG effects in C++ (HLE). Saves only the
  `0.5 us` MEG term and loses bit-exactness (see `doc/todo.md` §6
  external review, `doc/native-dsp.md`).

### Update 2026-09-30 — implemented on Xeon E3-1220 v3

Done, default path stays bit-exact (5 s `ONTILMOR.mid` hashes
identical before/after):

- `wasm_render.cpp`: integer fast-reject frame per MIDI event
  (`ceil(time*rate)`), exact double check only when due; new
  opt-in `smu_set_native_engine/fx`, `smu_native_firmware_share`
  exports (default off).
- `web/scripts/build.ts`: objects `-O2` -> `-O3` (matches
  standalone link).
- `processor.ts`: single MIDI staging malloc per quantum, panic
  reuses `outPtr` scratch, `Float32Array` FIFO ring instead of
  `number[]` push/splice, `* (1/32768)` scaling.
- `web/src/render.ts`: `--native-engine`, `--native-fx-full` flags
  for measurement.
- `worklet-harness.mjs`: `pathToFileURL` fix (was broken on
  Windows absolute paths).

Measured (`../ONTILMOR.mid`, Node wasm):

| Mode | 5 s render | 20 s render |
|---|---|---|
| Default (exact) | `0.89-0.95x` | `1.04x` |
| `--native-engine` | `1.32x` (fw share `31.6%`) | `1.70x` (fw share `18.6%`) |
| `--native-engine --native-fx-full` | `2.58-2.64x` | — |

Native rms diff `-0.03 dB` on this song (within the documented
`±0.46 dB`). Worklet harness on the same Xeon (`FAST=0/1`,
single note, 345x128-frame quanta):

| Worklet mode | 44160 frames wall | Avg/quantum | Max quantum |
|---|---|---|---|
| Exact | `1.1 s` (`0.91x`) | `3.2 ms` | `38 ms` |
| Fast synth (native+fx) | `0.2 s` (`~5x`) | `0.58 ms` | `7 ms` |

Budget is `~2.9 ms`. Exact averages over budget, so Firefox
stutters every other quantum and Chromium discards everything
(silence) — exactly the reported symptom. Fast synth averages
`0.58 ms`; the `7 ms` max is the one-time per-voice learn spike,
then it settles. The live page offers an opt-in Fast synth checkbox
(default off, faithful emulation first per project rule), wired
through `protocol.ts` init -> `processor.ts` (FX after reset, engine
on going live, mirroring `render.cpp`).

### Upstream merge 2026-09-30 — native FX fidelity fix

Merged `origin/main` (`5137689`, `c952159`, `d17f14b`) into this
branch. Directly related to the "variation seems gone" report:

- `c952159`: reverb 18 types rewritten with the same structure as
  the MEG program (`src/dsp/meg_reverb.h`), coefficients/addresses
  read from MEG. Band diff `4-25 dB` -> `0.05-0.25 dB`.
- `d17f14b`: chorus/variation/insertion-1 all types generated from
  the 34 MEG program shapes (`src/dsp/meg_fx_*.h` via
  `tools/meg_fx`, coefficients/addresses from MEG). Median band
  diff `0.01 dB`. Also faster: dense `0.84 -> 0.76 ms`.
- `5137689`: AWM2 skips idle voices (bit-exact, piano/dry `-25%`).

Measured on Xeon with `ONTILMOR.mid` after a clean wasm rebuild
(new headers don't trigger incremental rebuilds, so `obj`/`obj_sa`
were removed): exact base output byte-identical pre/post merge;
`--native-engine --native-fx-full` rms went from `+0.54 dB` (old
approximate FX) to `-0.03 dB` vs exact. The missing variation was
the old approximation, now replaced.

## Risks / notes

- Interpreter is ~5–8× slower than JIT; fine for offline render. For live
  it measures ~1× realtime on desktop Chromium (boot pace) with 13–36 ms
  worst quanta — survivable only via worker render-ahead (step 3).
- ROMs are never bundled, committed, or baked into the wasm — always user-supplied at runtime, persisted locally in IndexedDB after the first pick.
- `bootcache.h`/`nvram.h`/`voicecache` disk persistence deferred; every boot starts from scratch like `render.cpp` default.
