# Mu2026 engine integration

This fork of [S-MU2000](https://github.com/tarboh/S-MU2000)
adds internal audio buses for the separate Mu2026 Hybrid VST2 wrapper.
It retains upstream BSD 3-Clause and third-party notices.

The child VST2 effect advertises sixteen input channels in eight stereo pairs:
dry, reverb, chorus, variation, and insertions 1-4. The wrapper advertises
zero inputs to its host and supplies worker-generated audio to the child
internally. The first five pairs enter the MU2000 SWP30 master mixer; insertions
2-4 enter the slave SWP30 mixer, which still runs its original MEG processor.
These are not injected into a final stereo sum. The hybrid selects the original
MU firmware voice path at load; it does not use the experimental native voice
shortcut. No XG50 binary or DSP is loaded by this engine.

The engine runs at 44.1 kHz internally. At other host sample rates it
resamples the sixteen injected channels in lockstep with its existing output
converter. Reusable scratch buffers avoid per-block vector allocations
after initial warm-up. `vstiprobe --hybrid-fx` and `--hybrid-fx-48` assert
that all eight stereo routes produce effect output with the supplied ROMs and
that each insertion responds differently to distortion and bypass settings.
Entirely silent FIR windows skip unnecessary arithmetic while advancing the
same sample clock; nonzero input and filter tails keep their original behaviour.

The injected buses are an audio integration mechanism, not a claim that
the plugin emulates a physical PLG board's command protocol or timing.
Do not merge the modified VST DLL over an upstream S-MU2000 installation:
the child ABI has sixteen inputs and is paired with the Mu2026 wrapper.

## Upstream synchronization

The 2026-10-05 integration includes upstream through
`67c550e5bc58110dd527a745083e4e8aa25af919` (PR 121), including the sampler
waveform generators introduced in PR 107 and subsequent loop, envelope,
SysEx-import/export and waveform-editing corrections. These are MU sampler
features, not a replacement for the hybrid's approximate PLG-AN engine.

The fork deliberately retains these compatibility differences:

- Sixteen external input channels and all four insertion buses.
- Float input headroom and synchronized resampling at non-native rates.
- The wrapper's firmware voice path and existing worker/gain selection.
- Native variation sends, including per-drum-key scaling.
- Relative ROM pointers resolved beside their own `roms.txt`, not the host's
  working directory; incomplete ROM directories cannot shadow complete sets.

Upstream is integrated by a normal merge, preserving its history and notices.
Future merges should be evaluated in an isolated candidate against the current
engine, rather than replacing the hybrid child DLL with an upstream download.
The focused checks are `rom_search_probe`, `resampler_probe`, VSTi state/editor
probes, and `--hybrid-fx` / `--hybrid-fx-48`. Full-mix comparisons should cover
drums and delay, insertion-routed VL, SG, 2006LE fallback, DX/AN, and the heavy
multi-worker case, at both 44.1 and 48 kHz. A compile is not an audio regression
test. New upstream graphical controls do not establish screen-reader access;
the hybrid's native accessible editor remains separate.
