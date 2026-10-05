# Linux GUI porting plan (SDL3 + Cairo)

> **Refactored onto the shared base (2026-09-21).** The contributed monolith
> was reworked into the same three-file layout the Windows and macOS front
> ends use — `gui_linux.cpp` (main only), `ui/app_linux.h/.cpp`
> (`linux_app : public ui::app`), `ui/window_sdl.cpp` (the SDL3 pump, usable
> by any front end answering `ui::app`) — and `--audio-in` joined the shared
> parser (`ui/options.h`). Menus and status are Japanese like the other
> platforms; the contributed English texts (`ui/texts.h`) and the ImGui help
> language default (`ui::xgui::set_help_lang`) are available but no longer
> installed at startup. See [Architecture](#architecture) and
> [English on Linux](#english-on-linux).
>
> **Not maintained by the repository owner.** The Linux GUI and plug-ins were contributed by
> spessasus in PR #33. The maintainer does not use Linux and **cannot test, support, or take
> responsibility for them**. Use at your own risk; reports and fixes from Linux users are welcome.
> （作者は Linux を使っておらず、この部分の動作確認・保守・責任は負えない。自己責任で。）

Goal: `build/gui` on Linux — the same front-panel window, port menus, card
slot, MIDI file drop/play, PC editor windows (F2/F3), `--shot`, and then real
X11 editor views in the VST3/CLAP plug-ins. The engine half is already shared
(`ui/engine.h`) and the ALSA backends are done; this plan is only the
presentation half.

## Upstream note (2026-09-19)

Upstream implemented issue #25 in parallel (ALSA `live`, headless tools,
[doc/linux.md](linux.md)) with Claude Opus 5. This port now builds on that:
upstream's `audio_out_linux.cpp`/`midi_in_linux.cpp` are used as-is, and only
the two backends they left out (`midi_out`, `audio_in`) are added here. The
Makefile recipes below slot into upstream's Linux branch (`build-linux/`).
The VST3/CLAP **editor** port attempted here was reverted at the author's
request — plug-ins stay headless (generic UI) until a later phase; the shared
`ui/sdl_popup` component remains as gui's menu/dialog implementation.

## Toolkit decision: SDL3 + Cairo, no OpenGL

* **Window, events, timers, clipboard, drag-drop: SDL3** (3.4.16 present).
  SDL also answers the display-server question: the same binary runs on X11
  and Wayland, which matters because DAW hosts on Linux are mixed.
* **Panel drawing: Cairo image surface** (1.18 + cairo-ft + fontconfig
  present). The panel is GDI-style software drawing; rendering to a memory
  buffer and uploading it to an `SDL_Texture` each frame needs no GL context
  at all, at 30 fps for a ~1400×360 window. Text goes through `cairo-ft` +
  fontconfig (DejaVu/Noto fallback for "Segoe UI", same caveat as macOS).
* **PC editor windows: Dear ImGui views (shared) + SDL3 shell.**
  `third_party/imgui` is 1.92.9b, which ships `imgui_impl_sdl3` and, as the
  renderer, `imgui_impl_sdlgpu3`; vendor those unmodified, like the
  win32/dx11/metal backends already there. Upstream prefers SDL_gpu over
  SDL_Renderer where both exist (its `docs/BACKENDS.md` says so), so the
  renderer here is the GPU one rather than `imgui_impl_sdlrenderer3`.

  One `SDL_GPUDevice` for the whole process with every `SDL_Window` claimed
  onto it (`SDL_ClaimWindowForGPUDevice`), each window getting its own
  swapchain. That preserves the existing per-window `ImGuiContext` design:
  SDL_gpu wants a device per process with many windows on it, not a device per
  window. On Linux that means the Vulkan driver, so `SDL_GPU_SHADERFORMAT_SPIRV`
  and the backend's SPIR-V shader blobs; the swapchain is set to SDR, since the
  panel's colours are authored as sRGB triples.

  SDL_gpu has no `SDL_RenderReadPixels`, and a swapchain texture is
  write-only, so **`--shot` cannot read the window's swapchain**. It reads an
  offscreen texture instead: `imshell::sdl_read_pixels()` draws the frame into
  a `COLOR_TARGET` texture it owns, copies that into a download transfer
  buffer with `SDL_DownloadFromGPUTexture`, and maps it. That is SDL_gpu's
  own readback path -- no compute shader needed -- so `--shot` works on Linux
  like it does on the other two.

* **Dialogs/modals: SDL3 native.** `SDL_ShowOpenFileDialog` /
  `SDL_ShowSaveFileDialog` (card open, MIDI file, new card) and
  `SDL_ShowMessageBox` (factory-reset confirm, card alerts) cover everything
  `gui.cpp` does with `GetOpenFileNameW`/`MessageBoxW` and `gui_mac.cpp` does
  with `NSOpenPanel`/alerts. **Menus are the one gap**: SDL has no menus, so
  context popups are custom-drawn in-window (hit-testing and menu contents
  already live in the app code).

Nothing to install on this machine. For anyone following along:
`apt install libasound2-dev libsdl3-dev libcairo2-dev` (plus existing g++).

Rejected: raw X11+Xlib (reimplements timers/DnD/clipboard/Wayland for
nothing), GTK/Qt (native menus/dialogs, but a heavy dep with its own event
loop and C++ ABI to reconcile with `-fPIC` plug-ins), OpenGL renderers
(unneeded bandwidth/complexity for software-drawn panels).

## Architecture

Since the 2026-09-21 refactor the Linux front end has the same shape as the
Windows and macOS ones — a window-system file, an app class, and a main —
with everything the events mean decided once in `ui::app`:

| role | Windows | macOS | Linux |
|---|---|---|---|
| main | `src/gui.cpp` | `src/gui_mac.cpp` | `src/gui_linux.cpp` |
| app class | `src/ui/app_win.h/.cpp` (`win_app`) | `src/ui/app_mac.h/.cpp` (`gui_app`) | `src/ui/app_linux.h/.cpp` (`linux_app`) |
| window system | `src/ui/window_win.h/.cpp` (Win32) | `src/ui/window_mac.mm` (AppKit) | `src/ui/window_sdl.h/.cpp` (SDL3 + Cairo) |

The pump (`ui::window_sdl.cpp`) drives the shared event verbs directly
(`mouse_down` → `ui::mouse_out`, `key` in the shared key space of
`ui/menu.h`, `context_menu`, `resized`, `file_dropped`, …), exactly like
`wnd_proc` and `window_mac.mm`. The SDL window state sits behind `run_window
(ui::app &, …)`, so a future front end on another platform can reuse the pump
by answering `ui::app` — it is not Linux's.

Linux-specifics that remain: `linux_app` keeps the SDL handles the pump
feeds, the `sdl_popup` menu renderer (`ui/menu.h`'s `menu_group` description,
shared with the other platforms' menus), SDL3 file dialogs and message boxes
in named functions (so headless tests can stub them), and `--selftest` /
`--seconds` as pre-scan Linux-only flags.

## Phases

### 0. Vendor ImGui SDL3 backends
Copy `imgui_impl_sdl3.{h,cpp}` + `imgui_impl_sdlrenderer3.{h,cpp}` from
ImGui 1.92.9b into `third_party/imgui/backends/`, unmodified. No build yet.

Accept: tree builds exactly as before (`make`, `make test` green).

### 1. Real `gdi_linux.cpp`, DIB path first → `--shot` milestone
Implement the `compat/gdi.h` subset over Cairo image surfaces: brushes,
pens (incl. dash/dot, widths), `FillRect`/`RoundRect`/`Ellipse`/`Polygon`/
`PolyPolygon` (even-odd), `MoveToEx`/`LineTo`/`Polyline`, the 1-px
half-pixel nudge, `[left,right)` rects, `SetTextColor`/`SetBkMode`/
`DrawTextW` (UTF-16 → cairo-ft, flags `DT_CENTER/RIGHT/VCENTER/BOTTOM/
WORDBREAK/SINGLELINE/END_ELLIPSIS`), `CreateFontA` (height→px, weight,
fontconfig fallback chain), stock objects, `CreateCompatibleDC` /
`CreateDIBSection` (top-down 32-bit, like the Windows `--shot`) /
`GdiFlush` / `DeleteDC`. Port the half-pixel and rect-convention logic from
`gdi_mac.cpp` rather than reinventing it. Then wire `gui --shot` (same
`shot()` shape as `gui.cpp`, minus `GetDC`) into the Linux Makefile target.

Accept: `--shot` PNGs of the built-in panel, `--grid`, `--boot` with ROMs,
and both `art/*/panel.txt` layouts; art-only colour (`#404040`) present,
face `#c4bdaa` (same checks as doc/porting-macos.md). Pixel-compare Linux
`--shot` against macOS/Windows `--shot` where fonts allow; layout diffs from
font metrics get recorded, not hand-fixed.

### 2. Main window + panel interaction
`src/gui_linux.cpp`: SDL3 window, 33 ms timer, Cairo→texture upload +
`SDL_RenderPresent`, mouse press/drag/release/wheel, F2/F3/F5 +
`key_to_button` (shared), focus-lost release-all, XDND MIDI drop, resize,
`--list`, status line (use ALSA-side counters: `cpu_percent`,
`worst_ms`, `starved` — same line as the mac front end, not the WASAPI-only
`output_ms`/`late`). Reuse `ui/engine.h`, `nvram.h`, `gui.ini` keys
verbatim so settings files stay interchangeable.

Accept: DIB-vs-window comparison at 0 differing bytes (the check that makes
`--shot` a valid stand-in); `make test` still green; smoke under
`SDL_VIDEODRIVER=dummy` (window creates, synthetic events pump, no display
needed — Xvfb not required).

### 3. Menus, dialogs, card, player
Custom-drawn context popups from the shared `menu_group` description (port
picker A–D/THRU/OUT, A/D input, audio out, card new/open/eject, MIDI play/
stop, ports-3/4 fold/drop, factory reset, PC-editor entries); SDL3 file
dialogs and message boxes; SmartMedia 2 s flush timer; MIDI-drop watchdog
log; `--factory`.

Accept: every `WM_COMMAND` id in `gui.cpp` reachable through the Linux menus
(reviewed by id, like the mac port's menu-id audit); `gui.ini` round-trips
`ports34`, audio/MIDI names, volume.

### 4. PC editor windows (F2/F3)
`src/ui/pc_window_sdl.cpp`: per-view `SDL_Window` + `SDL_Renderer` +
`ImGuiContext`, driving the shared `imgui_view`s (`pc_editor`, `overview`,
`fx_editor`, `part_shapes`, `master_editor`) via the vendored SDL3
renderer backend; `open_window` semantics match `gui.cpp`.

Accept: all five views open, edit round-trips into automation params
(covered by existing probe/automation checks once Phase 5 lands).

### 5. Plug-in editor views
`plug_window_linux.cpp`: answer X11 and embed via `SDL_CreateWindowFrom`
in `attached()`; un-guard the CLAP `gui_create`/`is_api_supported`
(headless stub until here). `--view` in `vst3probe` becomes a real check.

Accept: `vst3probe --view` attaches; `aubprobe`-equivalent CLAP GUI round
trip; hosts fall back gracefully if embedding fails.

## Progress

### Phase 0 — done
`imgui_impl_sdl3.{h,cpp}` + `imgui_impl_sdlrenderer3.{h,cpp}` vendored from
upstream tag `v1.92.9b` (matches `IMGUI_VERSION_NUM 19291`), unmodified.
Nothing includes them yet; `make` + `make test` unchanged.

### Phase 1 — done
`src/compat/gdi_linux.cpp` rewritten over Cairo + fontconfig/FreeType, and
`src/gui_linux.cpp` added (`--shot` / `--list` / `--dump-layout`; a windowed
run refuses with a pointer to this doc). `build/gui` is in the Linux `all`
and in `CMakeLists.txt`.

| Check | Result |
|---|---|
| `--shot` empty panel / `--grid` | render; face `#c4bdaa`, keys `#d8cda5` (same palette as doc/porting-macos.md) |
| `roms --boot --shot` | LCD lit, boot voice screen; `96cd2d` LCD pixels present only here |
| `--mid piano.mid --shot` | level meters show signal |
| `--layout art/mame/panel.txt` | SVG parsed via `PolyPolygon`; art-only `#404040` present (2456 px sampled), absent without art |
| `--layout art/sample/panel.txt` | SVG buttons drawn (own palette, 1417 colours — not silently skipped) |
| determinism | same bytes across runs and across rebuilds |
| `make test` | all green (unchanged fingerprints — the audio path untouched) |
| VST3/CLAP + `--torture` + `clapprobe` | 0 problems; both renders byte-identical (peak 2449) |
| plug-in linkage | `libcairo.so.2` in `ldd`; Cairo now links into both `.so`s |

### Phase 2 — done (windowed run + English)
`src/gui_linux.cpp` grew an SDL3 windowed run: resizable window, 33 ms
timer, Cairo surface → streaming `ARGB8888` texture (`BLENDMODE_NONE`,
`NEAREST`), mouse press/drag/release/wheel, character-key map (same as
macOS), F5 layout reload, focus-lost release-all, file-drop play,
`--midi/-b/-c/-d`, `--nomidi`, `--latency`, `--audio`, `--audio-in`,
`--exclusive`, `--factory`, `--fast-midi`, `--usb`/`--host-midi`, `--play`,
`--seconds`, boot thread + ALSA audio on `eng.fill`, NVRAM save on exit.
Jack/card right-clicks print a Phase-3 pointer once; F2/F3 a Phase-4 one.
`--selftest` additionally uploads and reads the pixels back (hidden window,
works under `SDL_VIDEODRIVER=dummy`).

Two SDL3 API notes for the next reader: `SDL_Init` returns bool (not int),
and `SDL_RenderReadPixels` returns an `SDL_Surface*` whose format is the
renderer's choice — selftest converts to `ARGB8888` with `SDL_ConvertPixels`
before comparing (an `ABGR8888` readback cost one debugging session).

| Check | Result |
|---|---|
| `--selftest roms` | deterministic PASS (2016000 bytes) + roundtrip PASS (504000/504000 px RGB-identical, 0 alpha-only) |
| `gui roms --nomidi --seconds 6` (dummy driver) | boots, audio opens, frames present, exits 0 |
| `make test`, VST3/CLAP probes | unchanged, all green |
| shared-file edits (`panel`/`editor`/`effects`/`layout`/`engine`/`bootcache`) | mingw `-fsyntax-only` clean; Win/mac output unchanged by construction (table defaults) |

### Phase 3 — done (menus, dialogs, card, settings)
Custom Cairo-drawn popup menus (no toolkit menus in SDL): two levels max,
hover highlight, click-outside/Esc cancel, Up/Down/Return navigation. Same
items in the same order as `gui.cpp`/`gui_mac.cpp`, in English: port picker
(MIDI IN A–D, OUT, THRU A/B, A/D INPUT), card menu (new 16–128 MB, open,
eject, play/stop, ports 3+4 fold/drop), PHONES output menu
(digital/analog), factory reset, F2/F3 placeholders. Native SDL3 file dialogs
(open/save, async with a modal event pump; Esc abandons the wait) and
blocking message boxes (factory confirm with Cancel default, alerts).
SmartMedia new/open/eject + 2 s write-back timer + remembered card.
`gui.ini` in `config_dir()` with the exact Windows key spellings
(`midi_in*`, `midi_out*`, `audio_out/in`, `smartmedia`, `ports34`,
`output`, `volume`), names-not-numbers, keep-on-failure, `--nomidi` skips
saving. THRU/MIDI-OUT ports wired with all-notes-off on exit.

| Check (real X11 session, xdotool + screenshots) | Result |
|---|---|
| top/port/card/phones menus render + navigate | all correct, check marks draw as filled squares (Noto Sans lacks ✓/●) |
| port switch MIDI IN A → device | status line + `gui.ini` persist |
| analog output switch | `output=analog` persists |
| keyboard menu nav (Down/Return) | selects correctly |
| cancel (click-outside, Esc) | verified |
| SDL file dialog | opens (KDE portal window, confirmed visually); headless containers with no portal get Esc-cancel, never a hang |
| `make test`, probes, `--selftest` | all green |

Container caveats (not code bugs): ALSA underruns in `gui_run*.log` are the
sandbox's audio; xdotool click coordinates occasionally land offset under
this WM — natural flows verified repeatedly. Human pass still wanted for:
play/stop via dialog, card new/open/eject round trip, factory reset confirm
+ reboot, THRU to a real port.

### Phase 4 — done (PC editor windows)
`src/ui/pc_window_linux.{h,cpp}`: one SDL3 window + renderer per ImGui view,
driven by the vendored `imgui_impl_sdl3`/`imgui_impl_sdlrenderer3` backends
(same class shape as Win32/D3D11 and AppKit/Metal). Each window owns an
ImGui context; `route_event()` feeds the owning context from the global SDL
event stream; the X button hides (reopen keeps state); drops play through the
shared handler. F2/F3, right-click entries, `--editor/--list-window/
--fx-window`, and overview double-click follow-ups (`ui::pc_frame_all`)
all wired; CJK font is Noto Sans CJK JP via fontconfig (both languages
render, no tofu).

Upstream already ships a language system for these views (`LANGS[]`,
`g_lang`, `lang=` in `editor.ini`, combo in the UI), so Linux only defaults
it: new `ui::xgui::set_help_lang()` (5 lines, default behavior unchanged)
plus `set_help_lang(1)` at startup — a `lang=` in the file still wins.
Window titles map in the Linux shell (shared `title()` untouched).
Coverage caveat, not a port gap: labels upstream hasn't put in its HELP
tables stay Japanese and turn English on their own as upstream translates.

| Check (real X11 session) | Result |
|---|---|
| F2/F3, `--editor` | Editor + List open, English titles |
| editor content | live part table with ROM voice names, Show help + English combo |
| overview content | full part/voice/filter/EQ/insertion/Master UI, piano roll |
| X-button hide, main-window close | clean exit, settings saved |
| `make test`, probes, `--selftest`, CMake | all green |

Two fixes found along the way: the shutdown summary read audio counters
after `stop()` had dropped them (snapshot first — Linux backend only), and
the MIDI IN A disc sat 11 px left of its label (shared layout default,
realigned everywhere).

Container caveats: `live`/`gui` CPU% and starved counts look bad in the
sandbox (`blocktime` on `dense`: 14% average, 0 overruns — the synth is
fine, the virtual ALSA device is not).

### English on Linux

As contributed, everything the Linux GUI showed was English: the front end
installed `ui::english_texts()` at startup (`src/ui/texts.h`, header-only)
and defaulted the ImGui help language with `ui::xgui::set_help_lang(1)`.

Since the refactor onto the shared base, menus and status are **Japanese**
like Windows and macOS — the shared base has one set of texts, and a
per-platform override would be a new mechanism, not a port. Both English
hooks survive and are one call each if wanted: `ui::english_texts()` from
`ui/texts.h` (bottom tabs, hint line, editor/effects pages, layout errors,
boot log) and `ui::xgui::set_help_lang(1)` (ImGui view help); `lang=` in
`editor.ini` still wins for the views. Making the shared texts translatable
per platform is future work (`ui/texts.h` is the seam for it).

### Phase 5 — plug-in editor views (DEFERRED at the author's request)
Was attempted and reverted: SDL-embed via X11 child + wrapped window painted
fine in isolation (in-process PNG dump pixel-perfect), but presenting through
a host-owned parent needs host-side verification per DAW. Plug-ins stay
headless; `vst3probe --view` stops gracefully. Notes for the retry: force
`SDL_VIDEODRIVER=x11` before `SDL_Init` (SDL otherwise prefers Wayland and
paints into the void), keep every SDL/X11 call on one UI thread
(cross-thread create/present showed only black), convert readback with
`SDL_ConvertPixels` (format varies: XRGB8888/ABGR8888).

## Risks and non-goals

* Font metrics will differ from Windows/macOS (third set of numbers); panel
  layout nits go in `panel.txt`, never in the drawing code.
* Interactive verification (clicking menus, looking at the panel) needs a
  human at a machine — same caveat as the macOS port. Everything before that
  is headless-checkable.
* Non-goals: Wayland-specific code (SDL absorbs it), IME/complex text input
  beyond what the panel needs, `midisend`/`rec` (WinMM-only tools, still out).
