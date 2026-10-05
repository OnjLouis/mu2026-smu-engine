#!/usr/bin/env python3
# license:BSD-3-Clause
"""
songcheck: find wrong-sounding voices in real songs, without a reference unit.

The emulator has no hardware to compare against on most machines. What it
can do is play a real song, solo each channel, and look for the kinds of
faults people have actually reported: a voice that makes no sound, a layer
detuned by a semitone, a note that dies while it is still held, a click that
repeats at the loop point, a sound that never stops, clipping, DC offset.
Every finding names the bank, program, note and time, and points at a solo
MIDI file that reproduces it, so it can be pasted into an issue as-is.

    python3 tools/songcheck.py song.mid [more.mid ...]     check songs
    python3 tools/songcheck.py --sweep gm                  play every GM voice once
    python3 tools/songcheck.py --sweep kits                every XG drum kit
    python3 tools/songcheck.py --sweep list.txt            "msb lsb prog" per line
    python3 tools/songcheck.py --selftest                  test the analysers

Options: --roms DIR (default roms), --render PATH (default build/render),
--out DIR (default build/songcheck), --no-solo (whole mix only), --json.

Needs numpy. Nothing here is a pass/fail oracle: it is a filter that turns a
40-minute listen into a short list worth listening to.
"""
import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

try:
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("songcheck needs numpy: pip install numpy")

RATE = 44100
TAIL = 4.0          # seconds rendered after the last note-off
GM_NAMES = (
    "GrandPno BritePno E.Grand HnkyTonk E.Piano1 E.Piano2 Harpsi. Clavi. Celesta Glocken MusicBox Vibes Marimba Xylophon TubulBel Dulcimer DrawOrgn PercOrgn RockOrgn ChrchOrg ReedOrgn Acordion Harmnica TangoAcd NylonGtr SteelGtr JazzGtr CleanGtr MuteGtr Ovrdrive DistGtr GtrHarmo AcoBass FngrBass PickBass Fretless SlapBas1 SlapBas2 SynBass1 SynBass2 Violin Viola Cello Contrabs TremStr PizzStr Harp Timpani Strings1 Strings2 SynStr1 SynStr2 ChoirAah VoiceOoh SynVoice OrchHit Trumpet Trombone Tuba MuteTrp FrHorn BrasSect SynBras1 SynBras2 SprnoSax AltoSax TenorSax BariSax Oboe EngHorn Bassoon Clarinet Piccolo Flute Recorder PanFlute Bottle Shakhchi Whistle Ocarina SquareLd SawLead CaliopLd ChiffLd CharanLd VoiceLd FifthLd Bass&Ld NewAgePd WarmPad PolySyPd ChoirPad BowedPad MetalPad HaloPad SweepPad Rain SoundTrk Crystal Atmosphr Bright Goblins Echoes Sci-Fi Sitar Banjo Shamisen Koto Kalimba Bagpipe Fiddle Shanai TnklBell Agogo SteelDrm Woodblok TaikoDrm MelodTom SynDrum RevCymbl FretNoiz BrthNoiz Seashore Tweet Telphone Helicptr Applause Gunshot"
).split()
assert len(GM_NAMES) == 128
XG_KITS = [0, 1, 2, 8, 16, 24, 25, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 126, 127]
SUSTAIN_FAMILIES = set(range(16, 24)) | set(range(40, 56)) | set(range(56, 96)) | {110}   # organs, strings, choir, brass, reeds, pipes, leads, pads


def note_name(n):
    return "CCDDEFFGGAAB"[n % 12] + ("", "#", "", "#", "", "", "#", "", "#", "", "#", "")[n % 12] + str(n // 12 - 1)


def voice_name(msb, lsb, prog):
    if msb in (126, 127):
        return f"drum kit {prog}"
    if msb == 0 and lsb == 0:
        return GM_NAMES[prog]
    return f"{GM_NAMES[prog]} variation" if msb == 0 else f"bank {msb}/{lsb}"


def voice_label(v):
    msb, lsb, prog = v
    return f"bank {msb}/{lsb} PC {prog + 1} ({voice_name(msb, lsb, prog)})"


# ---------------------------------------------------------------- MIDI reading

def vlq_read(data, i):
    n = 0
    while True:
        b = data[i]; i += 1
        n = (n << 7) | (b & 0x7f)
        if not b & 0x80:
            return n, i


def vlq(n):
    out = [n & 0x7f]; n >>= 7
    while n:
        out.append(0x80 | (n & 0x7f)); n >>= 7
    return bytes(reversed(out))


def read_midi(path):
    """Return (tpq, tracks) where each track is a list of (tick, bytes)."""
    data = Path(path).read_bytes()
    if data[:4] == b"RIFF" and data[8:12] == b"RMID":      # RIFF-wrapped SMF (Windows .rmi, some XG library files)
        i = 12
        while i + 8 <= len(data):
            tag, ln = data[i:i + 4], struct.unpack("<I", data[i + 4:i + 8])[0]
            if tag == b"data":
                data = data[i + 8:i + 8 + ln]; break
            i += 8 + ln + (ln & 1)
    if data[:4] != b"MThd":
        raise ValueError(f"{path}: not a Standard MIDI File (RCP/XWS must be converted first)")
    hlen, fmt, ntrk, tpq = struct.unpack(">IHHH", data[4:14])
    if tpq & 0x8000:
        raise ValueError(f"{path}: SMPTE time base is not supported")
    i = 8 + hlen
    tracks = []
    for _ in range(ntrk):
        if data[i:i + 4] != b"MTrk":
            raise ValueError(f"{path}: bad track header")
        tlen = struct.unpack(">I", data[i + 4:i + 8])[0]
        j, end = i + 8, i + 8 + tlen
        tick, status, ev = 0, 0, []
        while j < end:
            d, j = vlq_read(data, j); tick += d
            b = data[j]
            if b == 0xFF:
                ln, k = vlq_read(data, j + 2)
                ev.append((tick, data[j:k + ln])); j = k + ln
            elif b in (0xF0, 0xF7):
                ln, k = vlq_read(data, j + 1)
                ev.append((tick, bytes([b]) + data[k:k + ln])); j = k + ln
            else:
                if b & 0x80:
                    status = b; j += 1
                n = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
                ev.append((tick, bytes([status]) + data[j:j + n])); j += n
        tracks.append(ev)
        i = end
    return tpq, tracks


def tempo_map(tpq, tracks):
    """Return a function tick -> seconds honouring every tempo change."""
    changes = [(0, 500000)]
    for trk in tracks:
        for tick, b in trk:
            if b[:2] == b"\xff\x51":
                changes.append((tick, int.from_bytes(b[3:6], "big")))
    changes.sort(key=lambda c: c[0])          # stable: the file's tick-0 tempo stays after the default and wins
    table, sec, last_tick, tempo = [], 0.0, 0, changes[0][1]
    for tick, t in changes:
        sec += (tick - last_tick) * tempo / tpq / 1e6
        table.append((tick, sec, t)); last_tick, tempo = tick, t

    def at(tick):
        lo = 0
        for k in range(len(table)):
            if table[k][0] <= tick:
                lo = k
        t0, s0, tp = table[lo]
        return s0 + (tick - t0) * tp / tpq / 1e6
    return at


class Song:
    """Everything the analysers need to know about one MIDI file."""

    def __init__(self, path):
        self.path = Path(path)
        self.tpq, self.tracks = read_midi(path)
        self.at = tempo_map(self.tpq, self.tracks)
        merged = sorted(((tick, ti, b) for ti, trk in enumerate(self.tracks) for tick, b in trk), key=lambda e: (e[0], e[1]))
        self.events = [(self.at(tick), b) for tick, ti, b in merged]
        self.notes = defaultdict(list)      # ch -> [(t_on, t_off, note, vel, voice)]
        self.bend = defaultdict(list)       # ch -> [(t, value)]
        self.untrusted = defaultdict(list)  # ch -> [(t, reason)]
        self.master = []                    # [(t, value)] master volume from SysEx (universal 7F 04 01, XG 00 00 04)
        self.level = defaultdict(list)      # ch -> [(t, cc, value)] CC7 / CC11 changes
        self.drum_ch = {9}
        bank = {ch: [0, 0] for ch in range(16)}
        prog = {ch: 0 for ch in range(16)}
        rpn = {ch: [127, 127] for ch in range(16)}
        open_notes = {}
        self.end = 0.0
        for t, b in self.events:
            s = b[0]
            if s == 0xF0 and b[1:4] == b"\x43\x10\x4c" and len(b) >= 8 and b[4] == 0x08 and b[6] == 0x07:
                if b[7]:
                    self.drum_ch.add(b[5])          # XG part mode = drum
                else:
                    self.drum_ch.discard(b[5])
                continue
            if s == 0xF0 and len(b) >= 7 and b[1] == 0x7F and b[3:5] == b"\x04\x01":
                self.master.append((t, b[6]))                          # universal real-time master volume (MSB)
            elif s == 0xF0 and b[1:4] == b"\x43\x10\x4c" and len(b) >= 8 and b[4:7] == b"\x00\x00\x04":
                self.master.append((t, b[7]))                          # XG master volume
            if s >= 0xF0:
                continue
            ch, kind = s & 0x0F, s & 0xF0
            self.end = max(self.end, t)
            if kind == 0xB0:
                c, v = b[1], b[2]
                if c == 0: bank[ch][0] = v
                elif c == 32: bank[ch][1] = v
                elif c in (7, 11): self.level[ch].append((t, c, v))
                elif c == 101: rpn[ch][0] = v
                elif c == 100: rpn[ch][1] = v
                elif c == 6 and rpn[ch] in ([0, 1], [0, 2]) and v != 64:
                    self.untrusted[ch].append((t, "tuning RPN"))
                elif c == 65 and v >= 64:
                    self.untrusted[ch].append((t, "portamento"))
                elif c in (120, 123):
                    for (och, n), (t0, vel, voice) in list(open_notes.items()):
                        if och == ch:
                            self.notes[ch].append((t0, t, n, vel, voice)); del open_notes[(och, n)]
            elif kind == 0xC0:
                prog[ch] = b[1]
                if bank[ch][0] in (126, 127):
                    self.drum_ch.add(ch)
                elif ch != 9:
                    self.drum_ch.discard(ch)
            elif kind == 0xE0:
                self.bend[ch].append((t, (b[1] | (b[2] << 7)) - 8192))
            elif kind == 0x90 and b[2] > 0:
                if (ch, b[1]) in open_notes:
                    t0, vel, voice = open_notes.pop((ch, b[1]))
                    self.notes[ch].append((t0, t, b[1], vel, voice))
                open_notes[(ch, b[1])] = (t, b[2], (bank[ch][0], bank[ch][1], prog[ch]))
            elif kind == 0x80 or (kind == 0x90 and b[2] == 0):
                if (ch, b[1]) in open_notes:
                    t0, vel, voice = open_notes.pop((ch, b[1]))
                    self.notes[ch].append((t0, t, b[1], vel, voice))
        for (ch, n), (t0, vel, voice) in open_notes.items():
            self.notes[ch].append((t0, self.end, n, vel, voice))
        for ch in self.notes:
            self.notes[ch].sort()

    def bend_active(self, ch, t0, t1):
        v = 0
        for t, val in self.bend.get(ch, []):
            if t <= t0:
                v = val
            elif t <= t1:
                return True
        return v != 0

    def is_untrusted(self, ch, t):
        return any(tt <= t for tt, _ in self.untrusted.get(ch, []))

    def master_at(self, t):
        v = 127
        for tt, val in self.master:
            if tt <= t:
                v = val
        return v

    def faded(self, ch, t0, t1):
        """True if master volume, CC7 or CC11 moved while [t0, t1] was sounding, or was already low at t0."""
        if self.master_at(t0) < 16:
            return True
        if any(t0 <= tt <= t1 for tt, _ in self.master):
            return True
        return any(t0 <= tt <= t1 for tt, _, _ in self.level.get(ch, []))

    def write_solo(self, ch, out):
        """Write a format-0 file with every event kept except notes on other channels."""
        merged = sorted(((tick, ti, b) for ti, trk in enumerate(self.tracks) for tick, b in trk), key=lambda e: (e[0], e[1]))
        trk, last = bytearray(), 0
        for tick, ti, b in merged:
            s = b[0]
            if s < 0xF0 and (s & 0xF0) in (0x80, 0x90, 0xA0) and (s & 0x0F) != ch:
                continue
            if b[:2] == b"\xff\x2f":
                continue
            trk += vlq(tick - last); last = tick
            if s in (0xF0, 0xF7):
                trk += bytes([s]) + vlq(len(b) - 1) + b[1:]
            else:
                trk += b
        trk += vlq(0) + b"\xff\x2f\x00"
        Path(out).write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, self.tpq) + b"MTrk" + struct.pack(">I", len(trk)) + trk)


# ---------------------------------------------------------------- rendering

def render(render_bin, roms, midi, wav, seconds, reuse=False):
    side = Path(str(wav) + ".boot")
    if reuse and Path(wav).exists() and side.exists():
        return float(side.read_text())
    cmd = [str(render_bin), str(roms), str(midi), str(wav), f"{seconds:.2f}"]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise RuntimeError(f"render failed: {' '.join(cmd)}\n{p.stdout}\n{p.stderr}")
    m = re.search(r"(?:起動に ([\d.]+) 秒|Booted in ([\d.]+) s)", p.stdout + p.stderr)
    if not m:
        raise RuntimeError("render did not report the boot time; cannot align the WAV to the MIDI")
    boot = m.group(1) or m.group(2)
    side.write_text(boot)
    return float(boot)


def read_wav(path):
    import wave
    with wave.open(str(path)) as w:
        assert w.getframerate() == RATE and w.getsampwidth() == 2
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        return raw.reshape(-1, w.getnchannels()).astype(np.float64)


# ---------------------------------------------------------------- analysers
# Every analyser takes float samples (mono, LSB units) and returns findings.

def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def seg(x, t0, t1):
    a, b = max(0, int(t0 * RATE)), min(len(x), int(t1 * RATE))
    return x[a:b] if b > a else x[0:0]


def estimate_f0(x, lo=30.0, hi=4000.0):
    """Fundamental in Hz by harmonic product spectrum with parabolic refinement, or None."""
    if len(x) < 2048 or rms(x) < 5:
        return None
    x = x - x.mean()
    n = 1 << 19                                   # 0.084 Hz bins: 5 cents at 30 Hz, so low notes are not rounded to the bin
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    freqs = np.fft.rfftfreq(n, 1 / RATE)
    hps = np.log(spec + 1e-9).copy()
    for h in (2, 3, 4):
        d = spec[::h]
        hps[:len(d)] += np.log(d + 1e-9)
        hps[len(d):] = -1e9
    lo_i, hi_i = np.searchsorted(freqs, lo), np.searchsorted(freqs, hi)
    hps[:lo_i] = -1e9; hps[hi_i:] = -1e9
    k = int(np.argmax(hps))
    if k <= 0 or k >= len(spec) - 1:
        return None
    f_h = k * RATE / n
    if f_h < 500.0:
        # Low notes in short windows: the spectral main lobe is wider than the
        # error we are looking for, and two slightly detuned layers smear the
        # fundamental. Autocorrelation uses the whole period and is exact to a
        # fraction of a sample; the HPS pick only chooses which peak to refine.
        lo, hi = int(RATE / f_h * 0.8), int(RATE / f_h * 1.25)
        if hi + 1 < len(x):
            m = 1 << (2 * len(x) - 1).bit_length()
            X = np.fft.rfft(x, m)
            ac = np.fft.irfft(X * np.conj(X), m)[:len(x)]     # autocorrelation by FFT; direct correlate is O(n^2) on 26k samples
            j = lo + int(np.argmax(ac[lo:hi]))
            if 0 < j < len(ac) - 1:
                a, b, c = ac[j - 1], ac[j], ac[j + 1]
                off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
                f_ac = float(RATE / (j + off))
                # the two methods must agree; inharmonic or sub-harmonic sounds (tuba, bars) make them diverge
                return f_ac if abs(cents(f_ac, f_h)) < 50 else None
    # refine on the plain spectrum around the HPS pick
    a, b, c = np.log(spec[k - 1] + 1e-9), np.log(spec[k] + 1e-9), np.log(spec[k + 1] + 1e-9)
    off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return float((k + off) * RATE / n)


def cents(f, ref):
    return 1200 * math.log2(f / ref)


def comb_energy(spec, freqs, f0, harmonics=2, width_cents=25):
    e = 0.0
    for h in range(1, harmonics + 1):
        f = f0 * h
        lo, hi = f * 2 ** (-width_cents / 1200), f * 2 ** (width_cents / 1200)
        a, b = np.searchsorted(freqs, lo), np.searchsorted(freqs, hi)
        if b > a:
            e += float(spec[a:b].max())
    return e


def detuned_layer(x, f0):
    """Ratio of harmonic-comb energy one semitone below/above f0 to the comb at f0."""
    if len(x) < 4096 or f0 is None:
        return 0.0
    x = x - x.mean()
    n = 1 << 19
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    freqs = np.fft.rfftfreq(n, 1 / RATE)
    main = comb_energy(spec, freqs, f0)
    if main <= 0:
        return 0.0
    side = max(comb_energy(spec, freqs, f0 * 2 ** (-1 / 12)), comb_energy(spec, freqs, f0 * 2 ** (1 / 12)))
    return side / main


def click_times(x, t_offset=0.0, f0=None):
    """Times of isolated jumps that the waveform's own period does not explain, in seconds.

    With a known pitch the signal is compared with itself one cycle earlier
    (a saw wave's steep edge recurs every cycle and cancels; a click does not).
    Without one, the slope is compared with its own 99.9th percentile.
    """
    if len(x) < 1000:
        return []
    if f0 and RATE / f0 >= 8:
        P = int(RATE / f0)                                   # the period is fractional: try the lags either side of it
        e = np.minimum(np.abs(x[P + 1:] - x[1:-P]), np.abs(x[P + 1:] - x[:-P - 1]))
        d = np.concatenate([np.zeros(P + 1), e])
        thr = max(6 * float(np.percentile(e, 99)), 300.0)
    else:
        d = np.abs(np.diff(x))
        thr = max(6 * float(np.percentile(d, 99.9)), 300.0)
    idx = np.flatnonzero(d > thr)
    if not len(idx):
        return []
    # merge neighbours within 2 ms
    out, last = [], -10 ** 9
    for i in idx:
        if i - last > 0.002 * RATE:
            out.append(i)
        last = i
    return [t_offset + i / RATE for i in out]


def periodic(times, tol=0.15):
    if len(times) < 3:
        return None
    gaps = np.diff(times)
    m = float(np.median(gaps))
    if m <= 0:
        return None
    if np.std(gaps) / m < tol:
        return m
    return None


def mono_windows(song, ch, min_len=0.25):
    """Stretches where exactly one note sounds on the channel: (t0, t1, note, voice)."""
    notes = song.notes[ch]
    edges = sorted({t for n in notes for t in (n[0], n[1])})
    out = []
    for a, b in zip(edges, edges[1:]):
        if b - a < min_len:
            continue
        live = [n for n in notes if n[0] <= a and n[1] >= b]
        if len(live) == 1:
            out.append((a, b, live[0][2], live[0][4]))
    return out


def analyse_channel(song, ch, x, boot):
    """Findings for one soloed channel. x is mono float, boot is the WAV offset of MIDI time 0."""
    findings = []
    notes = song.notes[ch]
    if not notes:
        return findings
    drums = ch in song.drum_ch
    by_voice = defaultdict(list)
    for n in notes:
        by_voice[n[4]].append(n)

    def F(kind, voice, t, detail, notes_=()):
        findings.append(dict(kind=kind, channel=ch + 1, voice=voice, time=round(t, 2), detail=detail,
                             notes=sorted({note_name(n) for n in notes_})))

    # 1. silent voice: loud notes that produce nothing
    for voice, vn in by_voice.items():
        loud = [n for n in vn if n[3] >= 40 and song.master_at(n[0]) >= 16]
        if not loud:
            continue
        levels = [rms(seg(x, boot + n[0], boot + min(n[1], n[0] + 0.5) + 0.05)) for n in loud]
        if max(levels) < 20:
            sfx = " (SFX bank: many slots are empty on the hardware too)" if voice[0] == 64 else ""
            F("silent", voice, loud[0][0], f"{len(loud)} notes at velocity >= 40 produce no sound (peak RMS {max(levels):.0f} LSB){sfx}", [n[2] for n in loud])

    # 2. stuck / runaway after the last note-off
    last_off = max(n[1] for n in notes)
    a, b = rms(seg(x, boot + last_off + 1.0, boot + last_off + 2.0)), rms(seg(x, boot + last_off + 3.0, boot + last_off + 4.0))
    if b > 100 and b > 0.7 * a:
        F("stuck", notes[-1][4], last_off, f"still {b:.0f} LSB RMS 3-4 s after the last note-off, not decaying (1-2 s: {a:.0f})", [notes[-1][2]])

    # 3. clipping and DC in the solo
    clip = int(np.sum(np.abs(x) >= 32767))
    if clip:
        F("clipping", notes[0][4], notes[0][0], f"{clip} samples at full scale with this channel alone")
    body = seg(x, boot + notes[0][0], boot + last_off)
    if len(body) and abs(body.mean()) > 200:
        F("dc", notes[0][4], notes[0][0], f"DC offset {body.mean():+.0f} LSB while playing")

    if drums:
        return findings

    # 4. pitch, detuned layers and clicks on monophonic stretches
    pitch_err = defaultdict(list)
    detune = defaultdict(list)
    for t0, t1, note, voice in mono_windows(song, ch):
        if song.bend_active(ch, t0, t1) or song.is_untrusted(ch, t0):
            continue
        # no pitch to check on the SFX bank, GM 113-128 (percussion, effects) or GM 9-16 (bars and bells: inharmonic partials)
        tuned = voice[0] != 64 and not (voice[0] == 0 and (voice[2] >= 112 or 8 <= voice[2] <= 15))
        onset = next(n[0] for n in notes if n[2] == note and n[0] <= t0 and n[1] >= t1)
        start = max(t0, onset + 0.08)
        s = seg(x, boot + start, boot + min(t1, start + 0.6))
        if rms(s) < 20:
            continue
        f0 = estimate_f0(s) if tuned else None
        expected = 440.0 * 2 ** ((note - 69) / 12)
        e_oct = 9999
        if f0:
            e = cents(f0, expected)
            e_oct = ((e + 600) % 1200) - 600          # ignore octave ambiguity of the estimator
            pitch_err[voice].append((e_oct, note, t0))
            # harmonic and in tune, and enough cycles in the window for the
            # spectral lobe to be narrower than a semitone: look for a second series
            if abs(e_oct) < 35 and len(s) / RATE * expected >= 70:
                detune[voice].append((detuned_layer(s, expected), note, t0))
        # clicks: look at the sustained part after the attack
        sus = seg(x, boot + start + 0.15, boot + t1)
        if len(sus) > RATE // 4:
            ct = click_times(sus, start + 0.15, f0 if tuned and f0 and abs(e_oct) < 35 else None)
            if len(ct) >= 3:
                p = periodic(ct)
                if p and p < 0.02:
                    continue                       # repeating every cycle: that is the waveform, not a loop point
                if p:
                    F("loop-click", voice, t0, f"click repeating every {p * 1000:.0f} ms while {note_name(note)} is held (loop point?)", [note])
                elif len(ct) >= 6:
                    F("clicks", voice, t0, f"{len(ct)} clicks while {note_name(note)} is held", [note])

    for voice, errs in pitch_err.items():
        vals = np.array([e for e, _, _ in errs])
        med = float(np.median(vals))
        if abs(med) > 35 and len(vals) >= 2 and np.mean(np.abs(vals - med) < 30) >= 0.6:
            F("pitch", voice, errs[0][2], f"median {med:+.0f} cents from the written note over {len(vals)} notes", [n for _, n, _ in errs])
    for voice, rs in detune.items():
        bad = [(r, n, t) for r, n, t in rs if 0.5 < r < 5.0]
        if bad and len(bad) >= max(1, len(rs) // 2):
            F("detuned-layer", voice, bad[0][2], f"a second harmonic series one semitone away carries {max(b[0] for b in bad) * 100:.0f}% of the main one (a layer 100 cents off?)", [n for _, n, _ in bad])

    # 5. note dies while held (sustain families only)
    for voice, vn in by_voice.items():
        if voice[0] in (64, 126, 127) or voice[2] not in SUSTAIN_FAMILIES:   # drums and the SFX bank are one-shots
            continue
        for t0, t1, note, vel, _ in vn:
            if t1 - t0 < 1.5 or vel < 40 or song.faded(ch, t0, t1):    # a written fade is not an envelope fault
                continue
            peak = rms(seg(x, boot + t0 + 0.05, boot + t0 + 0.55))
            tail = rms(seg(x, boot + t1 - 0.3, boot + t1))
            if peak > 100 and tail < 0.01 * peak:
                F("dies-early", voice, t0, f"{note_name(note)} held {t1 - t0:.1f} s but silent before note-off (RMS {peak:.0f} -> {tail:.0f})", [note])
                break
    return findings


def analyse_mix(x):
    findings = []
    clip = int(np.sum(np.abs(x) >= 32767))
    if clip:
        findings.append(dict(kind="clipping", channel=0, voice=None, time=float(np.argmax(np.abs(x) >= 32767)) / RATE, detail=f"{clip} samples at full scale in the full mix", notes=[]))
    return findings


# ---------------------------------------------------------------- sweep MIDI

def sweep_midi(voices, out, hold=0.7, gap=0.4, note=60, bpm=120):
    """One note per voice, sequentially on channel 1 (drum kits on channel 10, a short pattern)."""
    tpq = 480
    tick_per_s = tpq * bpm / 60
    ev = []
    t = 0
    ev.append((0, bytes([0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7])))
    t = int(0.5 * tick_per_s)
    for msb, lsb, prog in voices:
        ch = 9 if msb in (126, 127) else 0
        ev.append((t, bytes([0xB0 | ch, 0, msb]))); ev.append((t, bytes([0xB0 | ch, 32, lsb]))); ev.append((t, bytes([0xC0 | ch, prog])))
        if ch == 9:
            for k, n in enumerate((36, 38, 42, 46, 49)):
                on = t + int((0.1 + k * 0.12) * tick_per_s)
                ev.append((on, bytes([0x99, n, 100]))); ev.append((on + int(0.1 * tick_per_s), bytes([0x89, n, 0])))
        else:
            on = t + int(0.05 * tick_per_s)
            ev.append((on, bytes([0x90, note, 100]))); ev.append((on + int(hold * tick_per_s), bytes([0x80, note, 0])))
        t += int((hold + gap) * tick_per_s)
    ev.sort(key=lambda e: e[0])
    trk, last = bytearray(), 0
    trk += vlq(0) + b"\xff\x51\x03" + struct.pack(">I", int(60e6 / bpm))[1:]
    for tick, b in ev:
        trk += vlq(tick - last); last = tick
        trk += (bytes([0xF0]) + vlq(len(b) - 1) + b[1:]) if b[0] == 0xF0 else b
    trk += vlq(int(tick_per_s)) + b"\xff\x2f\x00"
    Path(out).write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, tpq) + b"MTrk" + struct.pack(">I", len(trk)) + trk)


def sweep_voices(spec):
    if spec == "gm":
        return [(0, 0, p) for p in range(128)]
    if spec == "kits":
        return [(127, 0, p) for p in XG_KITS]
    out = []
    for line in Path(spec).read_text().splitlines():
        line = line.split("#")[0].strip()
        if line:
            msb, lsb, prog = (int(v) for v in line.split()[:3])
            out.append((msb, lsb, prog))
    return out


# ---------------------------------------------------------------- report

def write_report(results, out_md, out_json=None):
    lines = ["# songcheck report", ""]
    total = 0
    for r in results:
        fs = r["findings"]
        total += len(fs)
        lines.append(f"## {r['song']}  —  {len(fs)} finding(s), {r['channels']} channel(s) rendered")
        lines.append("")
        if not fs:
            lines.append("Nothing flagged.")
            lines.append("")
            continue
        lines.append("| kind | ch | voice | notes | at | what | reproduce |")
        lines.append("|---|---|---|---|---|---|---|")
        for f in sorted(fs, key=lambda f: (f["channel"], f["time"])):
            v = voice_label(f["voice"]) if f["voice"] else "mix"
            repro = r["solo"].get(f["channel"], r["mix"])
            lines.append(f"| {f['kind']} | {f['channel'] or '-'} | {v} | {' '.join(f['notes'])} | {f['time']:.1f} s | {f['detail']} | `{repro}` |")
        lines.append("")
    lines.insert(2, f"{total} finding(s) across {len(results)} file(s). Each row is a candidate for a listen, not a verdict; "
                    "attach the reproduce file and the time when filing an issue.")
    lines.insert(3, "")
    Path(out_md).write_text("\n".join(lines))
    if out_json:
        Path(out_json).write_text(json.dumps(results, indent=1, default=str))
    return total


def check_song(song, args, out_dir):
    name = song.path.stem
    seconds = song.end + TAIL
    mix_wav = out_dir / f"{name}.mix.wav"
    boot = render(args.render, args.roms, song.path, mix_wav, seconds, args.reuse)
    mix = read_wav(mix_wav).mean(axis=1)
    findings = analyse_mix(mix)
    solo_files = {}
    chans = [ch for ch in range(16) if song.notes.get(ch)]
    if not args.no_solo:
        for ch in chans:
            smid = out_dir / f"{name}.ch{ch + 1:02d}.mid"
            swav = out_dir / f"{name}.ch{ch + 1:02d}.wav"
            song.write_solo(ch, smid)
            b = render(args.render, args.roms, smid, swav, seconds, args.reuse)
            x = read_wav(swav).mean(axis=1)
            fs = analyse_channel(song, ch, x, b)
            findings += fs
            solo_files[ch + 1] = str(smid)
            print(f"  ch {ch + 1:2d}: {len(song.notes[ch]):4d} notes, {len({n[4] for n in song.notes[ch]})} voice(s), {len(fs)} finding(s)", flush=True)
    return dict(song=str(song.path), channels=len(chans), findings=findings, solo=solo_files, mix=str(song.path))


# ---------------------------------------------------------------- self-test

def selftest():
    """Synthetic signals with known faults; every analyser must fire on its fault and stay quiet otherwise."""
    fails = []

    def expect(cond, msg):
        (print("  ok  " if cond else "  FAIL"), print(msg))
        if not cond:
            fails.append(msg)

    t = np.arange(int(1.0 * RATE)) / RATE

    def tone(f, amp=6000, harmonics=(1, 0.5, 0.25, 0.12)):
        return sum(amp * a * np.sin(2 * np.pi * f * h * t) for h, a in zip((1, 2, 3, 4), harmonics))

    f0 = estimate_f0(tone(440))
    expect(f0 and abs(cents(f0, 440)) < 5, f"f0 of a 440 Hz tone = {f0}")
    f0 = estimate_f0(tone(110))
    expect(f0 and abs(cents(f0, 110)) < 8, f"f0 of a 110 Hz tone = {f0}")
    expect(estimate_f0(np.zeros(RATE)) is None, "f0 of silence is None")
    x = tone(440) + tone(440 * 2 ** (-1 / 12), amp=4000)
    expect(detuned_layer(x, 440) > 0.5, f"detuned layer ratio {detuned_layer(x, 440):.2f} > 0.5 when a layer sits a semitone low")
    expect(detuned_layer(tone(440), 440) < 0.2, f"detuned layer ratio {detuned_layer(tone(440), 440):.2f} < 0.2 on a clean tone")
    x = tone(220).copy()
    for k in range(5):
        x[int((0.2 + k * 0.087) * RATE)] += 20000
    ct = click_times(x, 0.0, 220.0)
    p = periodic(ct)
    expect(len(ct) == 5 and p and abs(p - 0.087) < 0.003, f"5 planted clicks found ({len(ct)}), period {p}")
    expect(len(click_times(tone(220))) == 0, "no clicks on a clean tone (no pitch given)")
    saw = 8000 * (2 * ((261.63 * t) % 1.0) - 1)          # a bright saw has a steep edge every 3.8 ms
    expect(len(click_times(saw, 0.0, 261.63)) == 0, f"no clicks on a raw saw wave ({len(click_times(saw, 0.0, 261.63))} found)")
    saw2 = saw.copy(); saw2[int(0.5 * RATE)] += 25000
    expect(len(click_times(saw2, 0.0, 261.63)) == 1, f"one planted click found on the saw ({len(click_times(saw2, 0.0, 261.63))})")

    # a fake song: channel 1 plays C4 for 0.8 s; a stuck channel keeps sounding
    class S:
        pass
    s = S(); s.notes = {0: [(0.0, 0.8, 60, 100, (0, 0, 0))]}; s.drum_ch = set(); s.bend = {}; s.untrusted = {}
    s.master = []; s.level = {}
    s.bend_active = lambda ch, a, b: False; s.is_untrusted = lambda ch, t: False
    s.master_at = lambda t: Song.master_at(s, t); s.faded = lambda ch, a, b: Song.faded(s, ch, a, b)
    boot = 0.5
    x = np.zeros(int((boot + 0.8 + TAIL + 1) * RATE))
    seg_t = np.arange(int(0.8 * RATE)) / RATE
    x[int(boot * RATE):int(boot * RATE) + len(seg_t)] = 6000 * np.sin(2 * np.pi * 261.63 * seg_t)
    fs = analyse_channel(s, 0, x, boot)
    expect(not fs, f"clean C4 note yields no findings: {[f['kind'] for f in fs]}")
    fs = analyse_channel(s, 0, np.zeros_like(x), boot)
    expect([f["kind"] for f in fs] == ["silent"], f"silent voice flagged: {[f['kind'] for f in fs]}")
    y = np.zeros_like(x)
    for k in range(2):                                   # two sequential C4s, both 100 cents flat, so the pitch rule has a quorum
        a = int((boot + k * 1.0) * RATE); y[a:a + len(seg_t)] = 6000 * np.sin(2 * np.pi * 261.63 * 2 ** (-1 / 12) * seg_t)
    s.notes = {0: [(0.0, 0.8, 60, 100, (0, 0, 0)), (1.0, 1.8, 60, 100, (0, 0, 0))]}
    fs = analyse_channel(s, 0, y, boot)
    expect("pitch" in [f["kind"] for f in fs], f"100 cents flat flagged as pitch: {[f['kind'] for f in fs]}")
    s.notes = {0: [(0.0, 0.8, 60, 100, (0, 0, 0))]}
    z = x.copy(); z[int((boot + 0.8) * RATE):] = 3000 * np.sin(2 * np.pi * 261.63 * np.arange(len(z) - int((boot + 0.8) * RATE)) / RATE)
    fs = analyse_channel(s, 0, z, boot)
    expect("stuck" in [f["kind"] for f in fs], f"never-ending tail flagged as stuck: {[f['kind'] for f in fs]}")
    s.notes = {0: [(0.0, 2.0, 60, 100, (0, 0, 48))]}    # strings, held 2 s, dies at 0.7 s
    w = np.zeros_like(x); a, b = int(boot * RATE), int((boot + 0.7) * RATE)
    w[a:b] = 6000 * np.sin(2 * np.pi * 261.63 * np.arange(b - a) / RATE)
    fs = analyse_channel(s, 0, w, boot)
    expect("dies-early" in [f["kind"] for f in fs], f"strings dying while held flagged: {[f['kind'] for f in fs]}")
    s.master = [(0.5, 60), (0.6, 30), (0.7, 0)]                      # the song fades the master volume: not a fault
    fs = analyse_channel(s, 0, w, boot)
    expect("dies-early" not in [f["kind"] for f in fs], f"master-volume fade suppresses dies-early: {[f['kind'] for f in fs]}")
    s.master = []; s.level = {0: [(0.5, 11, 40), (0.6, 11, 0)]}     # or an expression fade
    fs = analyse_channel(s, 0, w, boot)
    expect("dies-early" not in [f["kind"] for f in fs], f"expression fade suppresses dies-early: {[f['kind'] for f in fs]}")
    s.level = {}
    c = x.copy(); c[int(boot * RATE) + 1000:int(boot * RATE) + 1010] = 32767
    fs = analyse_channel(s, 0, c, boot)
    expect("clipping" in [f["kind"] for f in fs], f"full-scale samples flagged as clipping: {[f['kind'] for f in fs]}")

    # tempo map: a tick-0 tempo faster than the 120 BPM default must win over the default
    at = tempo_map(480, [[(0, b"\xff\x51\x03" + (250000).to_bytes(3, "big")), (480 * 4, b"\xff\x51\x03" + (500000).to_bytes(3, "big"))]])
    expect(abs(at(480 * 4) - 1.0) < 1e-9 and abs(at(480 * 8) - 3.0) < 1e-9, f"tempo map: 4 beats at 240 BPM = {at(480 * 4):.3f} s, then 4 at 120 = {at(480 * 8):.3f} s")
    at = tempo_map(480, [[(0, b"\xff\x51\x03" + (1000000).to_bytes(3, "big"))]])
    expect(abs(at(480) - 1.0) < 1e-9, f"tempo map: 1 beat at 60 BPM = {at(480):.3f} s")

    # MIDI round trip: write a sweep, read it back, solo it
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "sweep.mid"
        sweep_midi([(0, 0, 0), (0, 0, 48), (127, 0, 0)], p)
        song = Song(p)
        expect(len(song.notes[0]) == 2 and len(song.notes[9]) == 5, f"sweep round trip: {len(song.notes.get(0, []))} melodic, {len(song.notes.get(9, []))} drum notes")
        expect(song.notes[0][1][4] == (0, 0, 48), f"voice tracked through bank/program: {song.notes[0][1][4]}")
        song.write_solo(0, Path(d) / "solo.mid")
        solo = Song(Path(d) / "solo.mid")
        expect(len(solo.notes[0]) == 2 and not solo.notes.get(9), "solo keeps only channel 1 notes")
        expect(solo.end <= song.end and abs(solo.notes[0][-1][1] - song.notes[0][-1][1]) < 1e-6, "solo keeps its channel's notes in place and never outlasts the song")
    print(f"\nselftest: {len(fails)} failure(s)")
    return 1 if fails else 0


def e2e(args, out_dir):
    """Two one-note songs through the real renderer: a normal piano must be clean, a part at volume 0 must be 'silent'."""
    fails = []
    def make(path, volume):
        tpq = 480
        ev = vlq(0) + bytes([0xF0, 0x08, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7])
        ev += vlq(0) + bytes([0xB0, 7, volume]) + vlq(0) + bytes([0xC0, 0])
        ev += vlq(240) + bytes([0x90, 60, 100]) + vlq(tpq) + bytes([0x80, 60, 0])
        ev += vlq(240) + bytes([0x90, 64, 100]) + vlq(tpq) + bytes([0x80, 64, 0])
        ev += vlq(0) + b"\xff\x2f\x00"
        Path(path).write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, tpq) + b"MTrk" + struct.pack(">I", len(ev)) + ev)
    for name, vol, want in (("e2e-normal", 100, []), ("e2e-muted", 0, ["silent"])):
        p = out_dir / f"{name}.mid"; make(p, vol)
        r = check_song(Song(p), args, out_dir)
        got = sorted({f["kind"] for f in r["findings"]})
        ok = got == want
        print(("  ok   " if ok else "  FAIL ") + f"{name}: findings {got}, wanted {want}")
        if not ok:
            fails.append(name)
    print(f"\ne2e: {len(fails)} failure(s)")
    return 1 if fails else 0


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("songs", nargs="*", help="Standard MIDI Files or directories of them")
    ap.add_argument("--roms", default=os.environ.get("SMU2000_ROMS", "roms"))
    ap.add_argument("--render", default="build/render")
    ap.add_argument("--out", default="build/songcheck")
    ap.add_argument("--no-solo", action="store_true", help="analyse the whole mix only (fast, finds less)")
    ap.add_argument("--sweep", help="gm | kits | file with 'msb lsb prog' lines: play each voice once and check it")
    ap.add_argument("--json", action="store_true", help="also write report.json")
    ap.add_argument("--reuse", action="store_true", help="re-analyse solo renders already in --out instead of rendering again")
    ap.add_argument("--selftest", action="store_true", help="test the analysers on synthetic signals (no ROMs needed)")
    ap.add_argument("--e2e", action="store_true", help="test the whole pipeline through the renderer (needs ROMs)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    out_dir = Path(args.out); out_dir.mkdir(parents=True, exist_ok=True)
    if not Path(args.render).exists():
        sys.exit(f"render binary not found: {args.render} (run make first)")
    if not (Path(args.roms) / "mu2000_flash.bin").exists():
        sys.exit(f"no ROMs at {args.roms}")
    if args.e2e:
        return e2e(args, out_dir)
    songs = []
    if args.sweep:
        voices = sweep_voices(args.sweep)
        p = out_dir / f"sweep-{Path(args.sweep).stem}.mid"
        sweep_midi(voices, p)
        print(f"sweep: {len(voices)} voices -> {p}")
        songs.append(p)
    for s in args.songs:
        p = Path(s)
        if p.is_dir():
            found = sorted(p.glob("*.mid")) + sorted(p.glob("*.MID"))
            if not found:
                print(f"{p}: no .mid files in this folder")
            songs += found
        elif p.is_file():
            songs.append(p)
        else:
            sys.exit(f"{p}: no such file or folder")
    if not songs:
        ap.error("give at least one MIDI file, --sweep, or --selftest")
    results = []
    for p in songs:
        print(f"{p}:", flush=True)
        try:
            song = Song(p)
        except ValueError as e:
            print(f"  skipped: {e}")
            continue
        results.append(check_song(song, args, out_dir))
        # rewrite the report after every song, so a long run that is interrupted still leaves its findings behind
        total = write_report(results, out_dir / "report.md", out_dir / "report.json" if args.json else None)
    if not results:
        sys.exit("nothing was checked")
    print(f"\n{total} finding(s). Report: {out_dir / 'report.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
