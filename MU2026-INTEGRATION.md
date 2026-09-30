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
