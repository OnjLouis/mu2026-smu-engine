# license:BSD-3-Clause
#
# Draws the "real" front-panel art set (art/real/) from measurements.
#
#   python tools/panel_art/make_panel.py            SVGs + PNGs + panel.txt
#   python tools/panel_art/make_panel.py --svg-only  SVGs only (no Inkscape)
#
# Every position and size here was measured off a catalog photo of the unit
# (the photo is not part of the repository and nothing from it is embedded).
# Coordinates are in those photo pixels: the front face runs x 75-1698 and
# y 68-693; LX()/LY() map them to the panel's logical 1000 x 385.
#
# The background carries everything that never changes, printed labels
# included. Parts that change (buttons, LEDs, the volume pointer, the dial's
# dimple) are separate PNGs drawn over it at the positions in panel.txt.
# The PNGs are rendered with Inkscape (INKSCAPE or the usual install path).

import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "art", "real")
WORK = os.path.join(ROOT, "build", "panel_art")

# Placeholder brand, and the model name as drawn in the squarish logo.
BRAND = "EMULATOR"
MODEL = "S-MU2000"

X0, Y0, X1, Y1 = 75, 68, 1698, 693          # front face in photo pixels
S = 1000.0 / (X1 - X0)                       # photo pixel -> logical unit
BG_SCALE = 2                                  # background PNG: 2 px per unit
PART_SCALE = 4                                # parts: 4 px per unit


def LX(x):
    return round((x - X0) * S, 2)


def LY(y):
    return round((y - Y0) * S, 2)


def LS(v):
    return round(v * S, 2)


INK = "#2b2a28"
FONT = "Arial, Helvetica, sans-serif"
HEAVY = "Arial Black, Arial, sans-serif"

# ---------------------------------------------------------------- measurements
JACKS = [(180, 195), (180, 332)]
ADIN_KNOB = (333, 197)
VOLUME = (333, 332)
KNOB_R = 48
KNOB_CAP = 38                                  # the cap that rises from the base
MIDI = (305, 497)
PHONES = (435, 505)
LCD_GLASS = (478, 140, 670, 211)             # x y w h
CAT_X = (565, 662, 760, 858, 956, 1054)
CAT_Y = (433, 508, 582)                      # centers
CAT_W, CAT_H = 84, 26
CAT_NAMES = (("Piano", "Chrom. perc.", "Organ", "Guitar", "Bass", "Strings"),
             ("Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad"),
             ("Synth effects", "Ethnic", "Percussive", "SFX", "Model excl.", "Drum"))
MODES = (("play", 1277, 175, "PLAY"), ("edit", 1348, 175, "EDIT"),
         ("util", 1277, 248, "UTIL"), ("effect", 1348, 248, "EFFECT"),
         ("sampling", 1277, 322, "SAMPLING"), ("seq", 1348, 322, "SEQ"))
MODE_R = 15
NAV = (("mute_solo", 1450, 176), ("part-", 1550, 176), ("part+", 1628, 176),
       ("enter", 1450, 250), ("select-", 1550, 250), ("select+", 1628, 250),
       ("exit", 1450, 324), ("value-", 1550, 324), ("value+", 1628, 324))
NAV_W, NAV_H = 76, 54
ROUND = (("select", 1203, 510, "SELECT"), ("audition", 1312, 510, "AUDITION"))
ROUND_R = 24                                   # the cap is ROUND_R - 1 = 23
PLG_X = (883, 948, 1014, 1078)
PLG_Y = 650
PLG_W, PLG_H = 30, 18
DIAL = (1540, 515, 118)
DIMPLE = (-20, -62, 45)                       # relative to the dial centre, r
BAND = (632, 668)                             # ridged channel, top and bottom
BAND_DIP_R = 131                              # the face bulges round the dial

# ---------------------------------------------------------------- SVG bits
DEFS = """
<linearGradient id="face" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#c2b8a2"/><stop offset="0.55" stop-color="#b2a891"/>
  <stop offset="1" stop-color="#a39883"/></linearGradient>
<linearGradient id="sheen" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#fff" stop-opacity="0.03"/>
  <stop offset="0.7" stop-color="#fff" stop-opacity="0.05"/>
  <stop offset="0.93" stop-color="#fff" stop-opacity="0.2"/>
  <stop offset="1" stop-color="#fff" stop-opacity="0.06"/></linearGradient>
<linearGradient id="lip" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#d9d1be"/><stop offset="0.12" stop-color="#b8ae97"/>
  <stop offset="1" stop-color="#9b917b"/></linearGradient>
<linearGradient id="tooth" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#f2ede1"/><stop offset="0.5" stop-color="#d8d0bd"/>
  <stop offset="1" stop-color="#a99f88"/></linearGradient>
<pattern id="teeth" width="9" height="60" patternUnits="userSpaceOnUse">
  <rect width="9" height="60" fill="#3c362c"/>
  <rect x="0.8" width="5.4" height="60" fill="url(#tooth)"/>
  <rect x="6.2" width="1.2" height="60" fill="#6a604f"/></pattern>
<linearGradient id="bandShade" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#000" stop-opacity="0.55"/>
  <stop offset="0.25" stop-color="#000" stop-opacity="0.08"/>
  <stop offset="0.8" stop-color="#000" stop-opacity="0"/>
  <stop offset="1" stop-color="#000" stop-opacity="0.25"/></linearGradient>
<linearGradient id="cat" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fbeac6"/><stop offset="0.45" stop-color="#f1d8a8"/>
  <stop offset="1" stop-color="#d8b67e"/></linearGradient>
<linearGradient id="catDown" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#e3cd9f"/><stop offset="1" stop-color="#c9a970"/></linearGradient>
<linearGradient id="key" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#f6f6f2"/><stop offset="0.5" stop-color="#dfdfd9"/>
  <stop offset="1" stop-color="#bbbbb3"/></linearGradient>
<linearGradient id="keyDown" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#cfcfc8"/><stop offset="1" stop-color="#b3b3ab"/></linearGradient>
<linearGradient id="standbyH" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#c9c6be"/><stop offset="0.06" stop-color="#e6e4de"/>
  <stop offset="0.16" stop-color="#fbfbf8"/><stop offset="0.3" stop-color="#f4f3ef"/>
  <stop offset="0.65" stop-color="#e5e3dd"/><stop offset="0.9" stop-color="#d2cfc7"/>
  <stop offset="1" stop-color="#b9b5ac"/></linearGradient>
<linearGradient id="standbyV" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fff" stop-opacity="0.45"/><stop offset="0.08" stop-color="#fff" stop-opacity="0"/>
  <stop offset="0.85" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#3a3528" stop-opacity="0.22"/></linearGradient>
<linearGradient id="standby" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#ffffff"/><stop offset="0.6" stop-color="#ecebe6"/>
  <stop offset="1" stop-color="#cfcdc6"/></linearGradient>
<radialGradient id="knob" cx="0.42" cy="0.35" r="0.7">
  <stop offset="0" stop-color="#ffffff"/><stop offset="0.6" stop-color="#f0eee8"/>
  <stop offset="1" stop-color="#cbc7bd"/></radialGradient>
<radialGradient id="knobDown" cx="0.45" cy="0.4" r="0.7">
  <stop offset="0" stop-color="#e9e7e1"/><stop offset="1" stop-color="#bdb9af"/></radialGradient>
<radialGradient id="skirt" cx="0.5" cy="0.4" r="0.6">
  <stop offset="0.75" stop-color="#e9e6de"/><stop offset="1" stop-color="#a9a499"/></radialGradient>
<radialGradient id="dial" cx="0.4" cy="0.33" r="0.75">
  <stop offset="0" stop-color="#fbfaf8"/><stop offset="0.65" stop-color="#e7e4df"/>
  <stop offset="1" stop-color="#bdb8b0"/></radialGradient>
<radialGradient id="dimple" cx="0.5" cy="0.5" r="0.5">
  <stop offset="0" stop-color="#ebe8e2"/><stop offset="0.55" stop-color="#dedad3"/>
  <stop offset="0.82" stop-color="#c7c2b8"/><stop offset="0.94" stop-color="#b3ada2"/>
  <stop offset="1" stop-color="#faf8f4"/></radialGradient>
<radialGradient id="modebtn" cx="0.4" cy="0.35" r="0.7">
  <stop offset="0" stop-color="#8d928e"/><stop offset="0.7" stop-color="#5d625e"/>
  <stop offset="1" stop-color="#3b3f3c"/></radialGradient>
<radialGradient id="modebtnDown" cx="0.45" cy="0.4" r="0.7">
  <stop offset="0" stop-color="#6c716d"/><stop offset="1" stop-color="#343835"/></radialGradient>
<radialGradient id="led" cx="0.45" cy="0.4" r="0.6">
  <stop offset="0" stop-color="#e2ffb4"/><stop offset="0.5" stop-color="#74cf46"/>
  <stop offset="1" stop-color="#2f7d22"/></radialGradient>
<linearGradient id="plgOn" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#c9ff96"/><stop offset="0.5" stop-color="#78d24a"/>
  <stop offset="1" stop-color="#3f9a2c"/></linearGradient>
<linearGradient id="plgBezel" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#c8bfac"/><stop offset="1" stop-color="#b1a893"/></linearGradient>
<linearGradient id="plgOff" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#3e5a33"/><stop offset="1" stop-color="#23351e"/></linearGradient>
<radialGradient id="gold" cx="0.4" cy="0.3" r="0.75">
  <stop offset="0" stop-color="#fff6d2"/><stop offset="0.45" stop-color="#dcc27f"/>
  <stop offset="1" stop-color="#7f6427"/></radialGradient>
<linearGradient id="holeWall" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#000"/><stop offset="0.55" stop-color="#0d0c0a"/>
  <stop offset="1" stop-color="#4a4438"/></linearGradient>
<radialGradient id="deep" cx="0.5" cy="0.4" r="0.5">
  <stop offset="0" stop-color="#000"/><stop offset="0.8" stop-color="#050504"/>
  <stop offset="1" stop-color="#1b1a17"/></radialGradient>
<linearGradient id="bezel" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="#f8f5ec"/><stop offset="0.5" stop-color="#ebe6d8"/>
  <stop offset="1" stop-color="#cdc6b2"/></linearGradient>
<linearGradient id="slopeTop" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#8f8878"/><stop offset="1" stop-color="#6c6556"/></linearGradient>
<linearGradient id="slopeLeft" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#aaa393"/><stop offset="1" stop-color="#8d8676"/></linearGradient>
<linearGradient id="slopeBottom" x1="0" y1="1" x2="0" y2="0">
  <stop offset="0" stop-color="#ece8dd"/><stop offset="1" stop-color="#cfc9bc"/></linearGradient>
<linearGradient id="slopeRight" x1="1" y1="0" x2="0" y2="0">
  <stop offset="0" stop-color="#dcd6c9"/><stop offset="1" stop-color="#c2bcae"/></linearGradient>
<linearGradient id="floorShade" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#5a5446" stop-opacity="0.45"/>
  <stop offset="1" stop-color="#5a5446" stop-opacity="0"/></linearGradient>
<linearGradient id="recessTop" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#4a4438" stop-opacity="0.75"/>
  <stop offset="0.08" stop-color="#6f685a" stop-opacity="0.35"/>
  <stop offset="0.2" stop-color="#8a8374" stop-opacity="0"/>
  <stop offset="1" stop-color="#fff" stop-opacity="0.12"/></linearGradient>
<linearGradient id="recessLeft" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#4a4438" stop-opacity="0.5"/>
  <stop offset="0.04" stop-color="#6f685a" stop-opacity="0"/>
  <stop offset="0.97" stop-color="#fff" stop-opacity="0"/>
  <stop offset="1" stop-color="#fff" stop-opacity="0.25"/></linearGradient>
<linearGradient id="glass" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#8fbf3a"/><stop offset="1" stop-color="#7fb030"/></linearGradient>
<radialGradient id="dinFace" cx="0.45" cy="0.35" r="0.7">
  <stop offset="0" stop-color="#3a3936" stop-opacity="0.9"/><stop offset="1" stop-color="#1f1e1c" stop-opacity="0.9"/></radialGradient>
<linearGradient id="dinBody" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#2b2925"/><stop offset="1" stop-color="#171613"/></linearGradient>
<filter id="brushed" x="0" y="0" width="1" height="1">
  <feTurbulence type="fractalNoise" baseFrequency="0.0015 0.75" numOctaves="3" seed="7" result="n"/>
  <feColorMatrix in="n" type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  2.2 0 0 0 -1.1"/></filter>
<filter id="brushedDark" x="0" y="0" width="1" height="1">
  <feTurbulence type="fractalNoise" baseFrequency="0.0015 0.75" numOctaves="3" seed="7" result="n"/>
  <feColorMatrix in="n" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  -2.2 0 0 0 1.1"/></filter>
<filter id="grain" x="0" y="0" width="1" height="1">
  <feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="2" seed="3" result="n"/>
  <feColorMatrix in="n" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  -1.6 0 0 0 0.9"/></filter>
<filter id="ds" x="-30%" y="-30%" width="160%" height="170%">
  <feGaussianBlur in="SourceAlpha" stdDeviation="2.2"/><feOffset dx="0.8" dy="2.4" result="b"/>
  <feFlood flood-color="#000" flood-opacity="0.45"/><feComposite in2="b" operator="in" result="s"/>
  <feMerge><feMergeNode in="s"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
<filter id="dsBig" x="-30%" y="-30%" width="160%" height="170%">
  <feGaussianBlur in="SourceAlpha" stdDeviation="7"/><feOffset dx="3" dy="9" result="b"/>
  <feFlood flood-color="#000" flood-opacity="0.45"/><feComposite in2="b" operator="in" result="s"/>
  <feMerge><feMergeNode in="s"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
<filter id="inset" x="-10%" y="-10%" width="120%" height="120%">
  <feFlood flood-color="#000" flood-opacity="0.75"/>
  <feComposite in2="SourceAlpha" operator="out"/>
  <feGaussianBlur stdDeviation="2.5"/><feOffset dx="1" dy="2.5" result="sh"/>
  <feComposite in="sh" in2="SourceAlpha" operator="in" result="ins"/>
  <feMerge><feMergeNode in="SourceGraphic"/><feMergeNode in="ins"/></feMerge></filter>
<filter id="blur2"><feGaussianBlur stdDeviation="2"/></filter>
<filter id="blur1" x="-20%" y="-50%" width="140%" height="200%"><feGaussianBlur stdDeviation="1.1"/></filter>
<filter id="blur05" x="-20%" y="-50%" width="140%" height="200%"><feGaussianBlur stdDeviation="0.5"/></filter>
<linearGradient id="catUpV" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#f6e2b6"/><stop offset="0.36" stop-color="#fbeac4"/>
  <stop offset="0.44" stop-color="#e3c38b"/><stop offset="0.8" stop-color="#c79f64"/>
  <stop offset="1" stop-color="#9e7944"/></linearGradient>
<linearGradient id="catUpH" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#fff" stop-opacity="0.35"/><stop offset="0.12" stop-color="#fff" stop-opacity="0"/>
  <stop offset="0.86" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#5a3e1c" stop-opacity="0.3"/></linearGradient>
<linearGradient id="catUpFace" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#f3d9a6"/><stop offset="1" stop-color="#e9ca92"/></linearGradient>
<linearGradient id="catDnV" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#e4cfa4"/><stop offset="0.36" stop-color="#eadab5"/>
  <stop offset="0.44" stop-color="#d0b17c"/><stop offset="0.8" stop-color="#b58f58"/>
  <stop offset="1" stop-color="#8e6c3c"/></linearGradient>
<linearGradient id="catDnH" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#fff" stop-opacity="0.2"/><stop offset="0.12" stop-color="#fff" stop-opacity="0"/>
  <stop offset="0.86" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#5a3e1c" stop-opacity="0.3"/></linearGradient>
<linearGradient id="catDnFace" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#e2c998"/><stop offset="1" stop-color="#d9bc86"/></linearGradient>
<filter id="blur4" x="-100%" y="-100%" width="300%" height="300%"><feGaussianBlur stdDeviation="4"/></filter>
<filter id="blur8" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="8"/></filter>
"""


class Doc:
    def __init__(self):
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=18, anchor="middle", fill=INK, weight="normal",
             family=FONT, extra=""):
        s = s.replace("&", "&amp;").replace("<", "&lt;")
        self.add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" '
                 f'font-weight="{weight}" text-anchor="{anchor}" fill="{fill}" {extra}>{s}</text>')

    def dots(self, x0, y0, x1, y1, step=6, r=1.3, fill=INK):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) / step)
        for i in range(n + 1):
            t = i / max(n, 1)
            self.add(f'<circle cx="{x0 + (x1 - x0) * t:.1f}" cy="{y0 + (y1 - y0) * t:.1f}" '
                     f'r="{r}" fill="{fill}"/>')

    def svg(self, vb, w, h, bg=None):
        body = "\n".join(self.parts)
        back = f'<rect x="{vb[0]}" y="{vb[1]}" width="{vb[2]}" height="{vb[3]}" fill="{bg}"/>' if bg else ""
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb[0]} {vb[1]} {vb[2]} {vb[3]}" '
                f'width="{w}" height="{h}"><defs>{DEFS}</defs>{back}{body}</svg>')


# ---------------------------------------------------------------- logos
def squarish(kind, x, y, w, h, r=7):
    """Centre-line path of one glyph of the squarish model logo."""
    x1, y1, ym = x + w, y + h, y + h / 2
    if kind == "M":
        return f"M{x},{y1} V{y + r} Q{x},{y} {x + r},{y} H{x1 - r} Q{x1},{y} {x1},{y + r} V{y1} M{x + w / 2},{y} V{y1}"
    if kind == "U":
        return f"M{x},{y} V{y1 - r} Q{x},{y1} {x + r},{y1} H{x1 - r} Q{x1},{y1} {x1},{y1 - r} V{y}"
    if kind == "2":
        return (f"M{x},{y} H{x1 - r} Q{x1},{y} {x1},{y + r} V{ym - r / 2} Q{x1},{ym} {x1 - r},{ym} "
                f"H{x + r} Q{x},{ym} {x},{ym + r / 2} V{y1} H{x1}")
    if kind == "S":
        return (f"M{x1},{y} H{x + r} Q{x},{y} {x},{y + r} V{ym - r / 2} Q{x},{ym} {x + r},{ym} "
                f"H{x1 - r} Q{x1},{ym} {x1},{ym + r / 2} V{y1 - r} Q{x1},{y1} {x1 - r},{y1} H{x}")
    if kind == "0":
        return (f"M{x + r},{y} H{x1 - r} Q{x1},{y} {x1},{y + r} V{y1 - r} Q{x1},{y1} {x1 - r},{y1} "
                f"H{x + r} Q{x},{y1} {x},{y1 - r} V{y + r} Q{x},{y} {x + r},{y} Z")
    if kind == "-":
        return f"M{x},{ym} H{x1}"
    return ""


def model_logo(d, x, y):
    """S-MU2000 in the style of the MU2000 badge: letters hollow, digits solid."""
    gw, gh, gap = 33, 29, 5
    hollow, solid = [], []
    at = x
    for ch in MODEL:
        w = 14 if ch == "-" else gw
        path = squarish(ch, at, y, w, gh)
        (solid if ch.isdigit() else hollow).append(path)
        at += w + gap
    d.add(f'<path d="{" ".join(hollow)}" fill="none" stroke="#4b4944" stroke-width="7.5" '
          f'stroke-linejoin="round" stroke-linecap="round"/>')
    d.add(f'<path d="{" ".join(hollow)}" fill="none" stroke="#c4baa4" stroke-width="4.3" '
          f'stroke-linejoin="round" stroke-linecap="round"/>')
    d.add(f'<path d="{" ".join(solid)}" fill="none" stroke="#4b4944" stroke-width="7.5" '
          f'stroke-linejoin="round"/>')
    return at


def brand_logo(d):
    """Placeholder for the maker's mark: a waveform roundel and a word."""
    cx, cy, r = 170, 112, 23
    d.add(f'<circle cx="{cx}" cy="{cy}" r="{r + 2.5}" fill="none" stroke="#33343c" stroke-width="2"/>')
    d.add(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#33343c" stroke="#dcd3bd" stroke-width="2"/>')
    pts = []
    for i in range(41):
        t = i / 40
        x = cx - 16 + 32 * t
        y = cy - 9 * math.sin(t * 2 * math.pi) * math.exp(-1.2 * t)
        pts.append(f"{x:.1f},{y:.1f}")
    d.add(f'<polyline points="{" ".join(pts)}" fill="none" stroke="#d9ceb2" stroke-width="3" '
          f'stroke-linecap="round" stroke-linejoin="round"/>')
    d.add(f'<text x="200" y="128" font-family="{HEAVY}" font-size="40" font-weight="900" '
          f'fill="#31333b" textLength="185" lengthAdjust="spacingAndGlyphs">{BRAND}</text>')


def usb_logo(d, x, y):
    """USB badge, from the manual's drawing (its own coordinates, 339 wide):
    heavy italic USB with a cable running through the letters -- a dark bar
    from just left of the U, with a thin line of panel showing above it --
    that swells into the plug's head, then the small B-type socket.
    x, y is where the letters start and their baseline."""
    col = "#34332f"
    hole = "#bdb39d"                # the panel showing through
    k = 0.34
    d.add(f'<g transform="translate({x - 30 * k:.2f},{y - 115 * k:.2f}) scale({k})">')
    # the letters (x 30-198, y 32-115)
    # slanted by about 13 degrees, the baseline kept where it was
    d.add(f'<text x="30" y="115" font-family="{HEAVY}" font-size="112" font-weight="900" '
          f'fill="{col}" stroke="{col}" stroke-width="4" transform="translate(26.5,0) skewX(-13)" '
          f'textLength="162" lengthAdjust="spacingAndGlyphs">USB</text>')
    # the cable: a thin line of panel cuts the letters, the dark bar runs under it
    d.add(f'<rect x="36" y="67.5" width="164" height="3.8" fill="{hole}"/>')
    d.add(f'<rect x="33" y="71.3" width="172" height="8.7" fill="{col}"/>')
    # the plug's head: swelling out of the cable, its far end cut on a slant
    d.add(f'<path d="M198,71.3 C202,54 214,47 232,47 L280,47 L266,101 L232,101 '
          f'C214,101 202,95 198,80 Z" fill="{col}"/>')
    # the B-type socket: a slanted block with a window and a pin in it
    d.add(f'<path d="M276,54 L313,54 L304,95 L267,95 Z" fill="{col}"/>')
    d.add(f'<path d="M284.5,62 L302,62 L297,87 L279.5,87 Z" fill="{hole}"/>')
    d.add(f'<path d="M289,69.5 L297.5,69.5 L295.5,79.5 L287,79.5 Z" fill="{col}"/>')
    d.add('</g>')


def format_logos(d):
    """GM2, XG, Plug for XG and GS, each drawn in the coordinates of a clean
    drawing of the mark, then stacked in a 290-wide column and scaled into
    the space right of the LCD."""
    col = "#2f2e2b"
    hole = "#bab09a"            # the panel showing through
    k = 59.0 / 290.0
    d.add(f'<g transform="translate(1166,128) scale({k:.5f})">')

    # ---- GENERAL midi 2 (drawing: 600 wide, content x 30-570, y 165-435)
    d.add('<g transform="scale(0.537) translate(-30,-165)">')
    d.add(f'<text x="38" y="218" font-family="{FONT}" font-size="74" font-weight="bold" fill="{col}" '
          f'textLength="367" lengthAdjust="spacingAndGlyphs">GENERAL</text>')
    d.add(f'<path d="M38,372 V252 Q38,232 58,232 H195 V372 Z" fill="{col}"/>')             # m
    for x in (90, 142):
        d.add(f'<rect x="{x}" y="265" width="13" height="107" fill="{hole}"/>')
    d.add(f'<rect x="205" y="232" width="38" height="140" fill="{col}"/>')                # I
    d.add(f'<path d="M253,232 H326 Q348,232 348,254 V350 Q348,372 326,372 H253 Z '
          f'M282,262 V342 H320 V262 Z" fill="{col}" fill-rule="evenodd"/>')                # D
    d.add(f'<rect x="360" y="232" width="38" height="140" fill="{col}"/>')                # I
    d.add(f'<path d="M443,224 Q443,186 496,186 Q549,186 549,224 Q549,252 515,275 L444,351 H572" '
          f'fill="none" stroke="{col}" stroke-width="42" stroke-linejoin="round"/>')        # 2
    d.add(f'<rect x="30" y="385" width="540" height="50" fill="{col}"/>')
    d.add('</g>')

    # ---- XG (drawing: content x 85-481, y 86-298)
    xg_outer = ("M85,86 L140,86 L203,150 L263,86 L481,86 L481,298 L263,298 L203,232 L140,298 L85,298 "
                "L152,212 Q162,192 152,172 Z")
    xg_hole = ("M462.5,105 L317.5,105 L245,180 L245,200 L310,280 L462.5,280 L462.5,175 L372.5,175 "
               "L372.5,210 L420,210 L420,240 L332.5,240 L294,192.5 L332.5,144 L462.5,144 Z")
    d.add(f'<g transform="translate(0,165) scale(0.7323) translate(-85,-86)">'
          f'<path d="{xg_outer} {xg_hole}" fill="{col}" fill-rule="evenodd" stroke="{col}" '
          f'stroke-width="2" stroke-linejoin="round"/></g>')

    # ---- Plug for XG (drawing: 220 wide, content x 10-212, y 20-122):
    #      a P with a thin line inside, "lug" cut out of a box, "for XG"
    d.add('<g transform="translate(0,345) scale(1.436) translate(-10,-20)">')
    # the thin line inside the P runs on over the box, round the bowl
    ppath = "M21,122 V31 H70 A19,19 0 0 1 70,69 H32"
    d.add(f'<path d="{ppath}" fill="none" stroke="{col}" stroke-width="22"/>')
    d.add(f'<rect x="91" y="24" width="121" height="58" rx="6" fill="{col}"/>')
    d.add(f'<path d="{ppath}" fill="none" stroke="{hole}" stroke-width="3"/>')
    d.add(f'<text x="160" y="74" font-family="{HEAVY}" font-size="52" font-weight="900" fill="{hole}" '
          f'text-anchor="middle" textLength="88" lengthAdjust="spacingAndGlyphs">lug</text>')
    d.add(f'<text x="44" y="121" font-family="{HEAVY}" font-size="40" font-weight="900" fill="{col}" '
          f'textLength="166" lengthAdjust="spacingAndGlyphs">for XG</text>')
    d.add('</g>')

    # ---- GS (drawing: content x 105-445, y 40-294)
    gs_g = ("M190,40 L327.5,40 L282.5,89 L219,89 L175,132 L219,184 L245,184 L276,152 L221,151 "
            "L260,112.5 L307.5,112.5 L342,152 L267.5,230 L197.5,230 L105,135 Z")
    gs_s = "M360,40 L430,40 L390,88 L445,152 L305,294 L239,294 L375,152 L318,90 Z"
    d.add(f'<g transform="translate(18,520) scale(0.75) translate(-105,-40)">'
          f'<path d="{gs_g} {gs_s}" fill="{col}" stroke="{col}" stroke-width="2" '
          f'stroke-linejoin="round"/></g>')
    d.add('</g>')


# ---------------------------------------------------------------- the background
def band_top(x):
    """Top edge of the ridged channel: straight, but dipping round the dial."""
    dx = x - DIAL[0]
    if abs(dx) >= BAND_DIP_R:
        return BAND[0]
    return max(BAND[0], DIAL[1] + math.sqrt(BAND_DIP_R ** 2 - dx * dx))


def band_path():
    # the groove is cut right through to both sides of the case
    left, right = X0, X1
    pts = [f"M{left},{BAND[1]}", f"L{left},{BAND[0]}"]
    x = left
    while x < right:
        pts.append(f"L{x:.1f},{band_top(x):.2f}")
        x += 2
    pts.append(f"L{right},{BAND[0]} L{right},{BAND[1]} Z")
    return " ".join(pts)


def background():
    d = Doc()
    # ---- the face
    d.add(f'<rect x="{X0 - 10}" y="{Y0 - 10}" width="{X1 - X0 + 20}" height="{Y1 - Y0 + 20}" fill="#1a1c20"/>')
    d.add(f'<rect x="{X0}" y="{Y0}" width="{X1 - X0}" height="{Y1 - Y0 + 20}" rx="10" fill="url(#face)"/>')
    d.add(f'<clipPath id="faceclip"><rect x="{X0}" y="{Y0}" width="{X1 - X0}" height="{BAND[1] - Y0}" rx="10"/></clipPath>')
    w, h = X1 - X0, BAND[1] - Y0
    d.add(f'<g clip-path="url(#faceclip)">'
          f'<rect x="{X0}" y="{Y0}" width="{w}" height="{h}" filter="url(#brushed)" opacity="0.22"/>'
          f'<rect x="{X0}" y="{Y0}" width="{w}" height="{h}" filter="url(#brushedDark)" opacity="0.18"/>'
          f'<rect x="{X0}" y="{Y0}" width="{w}" height="{h}" filter="url(#grain)" opacity="0.10"/>'
          f'<rect x="{X0}" y="{Y0}" width="{w}" height="{h}" fill="url(#sheen)"/></g>')
    d.add(f'<rect x="{X0 + 1}" y="{Y0 + 1}" width="{X1 - X0 - 2}" height="{BAND[1] - Y0}" rx="9" '
          f'fill="none" stroke="#efe9da" stroke-opacity="0.7" stroke-width="2"/>')

    # ---- the ridged channel along the bottom, and the lip under it
    bp = band_path()
    d.add(f'<path d="{bp}" fill="url(#teeth)"/>')
    d.add(f'<path d="{bp}" fill="url(#bandShade)"/>')
    # its top edge: a dark line in the channel, a light chamfer on the face above
    top = " ".join(f"{x:.1f},{band_top(x):.2f}" for x in range(96, X1 - 16, 2))
    d.add(f'<polyline points="{top}" fill="none" stroke="#f3eee2" stroke-width="1.6" transform="translate(0,-1.2)"/>')
    d.add(f'<polyline points="{top}" fill="none" stroke="#221e18" stroke-width="1.4" transform="translate(0,1)"/>')
    # the lip: a chamfer catching the light, then the front edge
    d.add(f'<rect x="{X0}" y="{BAND[1]}" width="{X1 - X0}" height="{Y1 - BAND[1] + 20}" rx="6" fill="url(#lip)"/>')
    d.add(f'<rect x="{X0 + 4}" y="{BAND[1]}" width="{X1 - X0 - 8}" height="2.2" fill="#f6f1e4"/>')
    d.add(f'<rect x="{X0 + 4}" y="{BAND[1] + 2.2}" width="{X1 - X0 - 8}" height="1.2" fill="#8c826d"/>')
    # seen from the front, the case outline steps in where the groove runs
    # out at the sides: the groove floor sits a little inside the face
    for x, w in ((X0 - 10, 13), (X1 - 3, 13)):
        d.add(f'<rect x="{x}" y="{BAND[0]}" width="{w}" height="{BAND[1] - BAND[0]}" fill="#1a1c20"/>')
    d.add(f'<rect x="{X0 + 3}" y="{BAND[0]}" width="1.2" height="{BAND[1] - BAND[0]}" fill="#5a5243"/>')
    d.add(f'<rect x="{X1 - 4.2}" y="{BAND[0]}" width="1.2" height="{BAND[1] - BAND[0]}" fill="#e2dccd"/>')

    # card slot in the channel
    d.add(f'<rect x="165" y="638" width="296" height="30" rx="2" fill="#5f5847"/>')
    d.add(f'<rect x="169" y="642" width="288" height="21" rx="2" fill="#0a0908" filter="url(#inset)"/>')
    d.add(f'<rect x="169" y="660" width="288" height="3" fill="#3a3530"/>')
    # PLG LED windows in the channel (the LEDs themselves are parts)
    # each sits in a flat square bezel that breaks the ridges: a dark gap
    # round it, a top edge catching the light, and the lens window in the middle
    for cx in PLG_X:
        d.add(f'<rect x="{cx - 22}" y="{PLG_Y - 16}" width="44" height="32" fill="#2f2a22" opacity="0.85"/>')
        d.add(f'<rect x="{cx - 21}" y="{PLG_Y - 15}" width="42" height="30" fill="url(#plgBezel)"/>')
        d.add(f'<rect x="{cx - 21}" y="{PLG_Y - 15}" width="42" height="1.2" fill="#f2ecde" opacity="0.9"/>')
        d.add(f'<rect x="{cx - 21}" y="{PLG_Y - 15}" width="1" height="30" fill="#e9e2d2" opacity="0.7"/>')
        d.add(f'<rect x="{cx - 21}" y="{PLG_Y + 14}" width="42" height="1" fill="#7d7462" opacity="0.8"/>')
        d.add(f'<rect x="{cx + 20}" y="{PLG_Y - 15}" width="1" height="30" fill="#8a816e" opacity="0.7"/>')
        d.add(f'<rect x="{cx - 17}" y="{PLG_Y - 11}" width="34" height="22" fill="#8e8572"/>')
        d.add(f'<rect x="{cx - 16}" y="{PLG_Y - 10}" width="32" height="20" rx="1" fill="#0d0c0a"/>')
        d.add(f'<rect x="{cx - 17}" y="{PLG_Y + 10.2}" width="34" height="0.8" fill="#ece5d6" opacity="0.8"/>')

    # ---- logos and printed words
    brand_logo(d)
    # the logo line sits well clear of the LCD's frame; the bottoms of the
    # model logo, TONE GENERATOR and USB line up
    end = model_logo(d, 432, 81)
    d.text(end + 10, 110, "TONE GENERATOR", 21, "start")
    usb_logo(d, 1052, 110)
    format_logos(d)

    # ---- left block
    for cx, cy in JACKS:
        d.add(f'<circle cx="{cx}" cy="{cy}" r="38" fill="#181613" filter="url(#inset)"/>')
        d.add(f'<circle cx="{cx}" cy="{cy}" r="31" fill="url(#gold)" filter="url(#ds)"/>')
        d.add(f'<circle cx="{cx}" cy="{cy}" r="24.5" fill="#6b5a2f"/>')
        # the hole: the wall is lit at the bottom, in shadow at the top,
        # and the sleeve deep inside is darker still and seen a little low
        d.add(f'<circle cx="{cx}" cy="{cy}" r="23" fill="url(#holeWall)"/>')
        d.add(f'<circle cx="{cx}" cy="{cy + 2.5}" r="16.5" fill="url(#deep)"/>')
        d.add(f'<circle cx="{cx}" cy="{cy + 3.5}" r="9" fill="#000"/>')
        d.add(f'<path d="M{cx - 17},{cy + 12} a21,21 0 0 0 34,0" fill="none" stroke="#8a7d62" '
              f'stroke-width="1.4" opacity="0.8"/>')
    # both knobs turn, so their pointers are parts (knob.png), not painted here
    for (cx, cy), shadow_only in ((ADIN_KNOB, True), (VOLUME, True)):
        d.add(f'<circle cx="{cx + 3}" cy="{cy + 9}" r="{KNOB_R}" fill="#000" opacity="0.45" filter="url(#blur8)"/>')
        # hard plastic, in two parts: a flat base disc, and a smaller cylinder
        # (the cap) rising from a little inside it. Only the base and the
        # cap's body are painted here; the grooves cut into the cap's top
        # edge and its pointer turn with it, so they are the part knob.png
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{KNOB_R}" fill="#aaa59a"/>')
        d.add(f'<circle cx="{cx - 0.6}" cy="{cy - 0.8}" r="{KNOB_R - 0.8}" fill="#f6f5f0"/>')
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{KNOB_R - 1.8}" fill="#e4e1da"/>')
        # the cap throws a soft shadow on the base, down and to the right
        d.add(f'<circle cx="{cx + 1.5}" cy="{cy + 3.5}" r="{KNOB_CAP + 1}" fill="#000" opacity="0.35" '
              f'filter="url(#blur2)"/>')
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{KNOB_CAP}" fill="#9d988c"/>')
        d.add(f'<circle cx="{cx - 0.7}" cy="{cy - 0.9}" r="{KNOB_CAP - 0.7}" fill="#ffffff"/>')
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{KNOB_CAP - 1.8}" fill="#f1f0eb"/>')
        d.add(f'<ellipse cx="{cx - 13}" cy="{cy - 16}" rx="9" ry="3.8" transform="rotate(-30 {cx - 13} {cy - 16})" '
              f'fill="#fff" opacity="0.9"/>')
        if not shadow_only:
            knob_pointer(d, cx, cy, -12)
    d.text(182, 270, "1", 20)
    d.dots(193, 263, 270, 263)
    d.text(277, 270, "A/D INPUT", 20, "start")
    d.text(182, 403, "2", 20)
    d.dots(193, 396, 252, 396)
    d.dots(255, 268, 255, 396)
    d.text(292, 403, "VOLUME", 20, "start")

    # STANDBY
    d.add('<rect x="90" y="447" width="122" height="71" rx="5" fill="#2d2a25" filter="url(#inset)"/>')
    # the key is shaped like a kamaboko: its face bulges out across its width
    # (the axis runs up and down), so the shading runs left to right -- dark
    # where the left end turns away, a band of light just in from it, then a
    # slow fall into shade towards the right end. A little light along the top
    # edge and shade along the bottom
    d.add('<rect x="97" y="453" width="108" height="58" rx="4" fill="#8f8b82" filter="url(#ds)"/>')
    d.add('<rect x="97" y="453" width="108" height="57" rx="4" fill="url(#standbyH)"/>')
    d.add('<rect x="97" y="453" width="108" height="57" rx="4" fill="url(#standbyV)"/>')
    d.add('<rect x="108" y="455" width="7" height="53" rx="3.5" fill="#fff" opacity="0.7" filter="url(#blur1)"/>')
    d.add(f'<rect x="100" y="549" width="9" height="11" fill="{INK}"/>')
    d.add(f'<rect x="100" y="577" width="9" height="4" fill="{INK}"/>')
    d.text(116, 562, "STANDBY", 20, "start")
    d.text(116, 584, "ON", 20, "start")

    # MIDI IN A (measured off a close-up): the round hole in the face with
    # its lit lower edge; the socket's black shell, flat along the bottom;
    # a groove; the matte face with a square key notch at the top; and five
    # contact holes, each a round hole with ears where the spring sits
    mx, my = MIDI
    d.add(f'<circle cx="{mx}" cy="{my}" r="69" fill="none" stroke="#f1ece0" stroke-width="2.2" '
          f'stroke-dasharray="190 244" transform="rotate(5 {mx} {my})"/>')
    defs_clip = f'<clipPath id="dinhole"><circle cx="{mx}" cy="{my}" r="68"/></clipPath>'
    d.add(defs_clip)
    d.add(f'<g clip-path="url(#dinhole)">'
          f'<circle cx="{mx}" cy="{my}" r="68" fill="#0d0d0c"/>'
          f'<rect x="{mx - 70}" y="{my + 57}" width="140" height="14" fill="#060606"/>'
          f'<rect x="{mx - 70}" y="{my + 56}" width="140" height="1.2" fill="#2e2d2a"/></g>')
    d.add(f'<circle cx="{mx}" cy="{my}" r="68" fill="none" stroke="#000" stroke-width="3" opacity="0.6" filter="url(#blur2)"/>')
    fx, fy, R = mx, my - 3.5, 45
    d.add(f'<circle cx="{fx}" cy="{fy}" r="{R + 7}" fill="#1b1b1a"/>')
    d.add(f'<circle cx="{fx}" cy="{fy}" r="{R + 4}" fill="#070707"/>')
    d.add(f'<circle cx="{fx}" cy="{fy}" r="{R}" fill="#2d2c2a"/>')
    d.add(f'<circle cx="{fx}" cy="{fy}" r="{R}" fill="url(#dinFace)"/>')
    # key notch, cut into the face from the top
    d.add(f'<rect x="{fx - 0.2 * R}" y="{fy - R - 2}" width="{0.4 * R}" height="{0.29 * R + 2}" fill="#050505"/>')
    d.add(f'<rect x="{fx - 0.2 * R}" y="{fy - R + 0.29 * R - 0.6}" width="{0.4 * R}" height="1" fill="#3c3b38"/>')
    # contacts: (x, y) as a fraction of R, and the angle of the ears
    for ux, uy, ang in ((-0.60, -0.05, -25), (0.60, -0.05, 25),
                        (-0.43, 0.36, -60), (0.43, 0.36, 60), (0.0, 0.52, 90)):
        cx, cy, hr = fx + ux * R, fy + uy * R, 0.15 * R
        d.add(f'<g transform="translate({cx:.2f},{cy:.2f}) rotate({ang})">'
              f'<rect x="{-1.55 * hr:.2f}" y="{-0.26 * hr:.2f}" width="{3.1 * hr:.2f}" height="{0.52 * hr:.2f}" fill="#040404"/>'
              f'<circle r="{hr:.2f}" fill="#040404"/>'
              f'<rect x="{-1.55 * hr:.2f}" y="{0.2 * hr:.2f}" width="{3.1 * hr:.2f}" height="0.5" fill="#46443f"/>'
              f'</g>')
        d.add(f'<path d="M{cx - hr:.2f},{cy + 0.3:.2f} a{hr:.2f},{hr:.2f} 0 0 0 {2 * hr:.2f},0" '
              f'fill="none" stroke="#4b4944" stroke-width="0.7"/>')
    d.text(310, 584, "MIDI IN A", 20)

    # PHONES
    px, py = PHONES
    d.add(f'<circle cx="{px}" cy="{py}" r="30" fill="#161411" filter="url(#inset)"/>')
    d.add(f'<circle cx="{px}" cy="{py}" r="23" fill="#34312b" stroke="#5f5a4e" stroke-width="2"/>')
    d.add(f'<circle cx="{px}" cy="{py}" r="12" fill="url(#holeWall)"/>')
    d.add(f'<circle cx="{px}" cy="{py + 1.5}" r="7.5" fill="#000"/>')
    d.text(438, 584, "PHONES", 20)

    # CARD label
    d.add('<rect x="173" y="603" width="284" height="21" fill="#5f5e5c"/>')
    d.add('<path d="M258,608 h7 l3,3 v9 h-10 z" fill="#f2f0ea"/>')
    d.text(271, 613, "3.3V", 10, "start", "#f2f0ea", "bold")
    d.text(313, 621, "CARD", 18, "start", "#f2f0ea", "bold")

    # ---- LCD window (the code draws the LCD itself over the glass)
    gx, gy, gw, gh = LCD_GLASS
    # The window is a recess: its floor is a lighter silver, the face's edge
    # throws a shadow on the top and left walls, and the bottom and right
    # edges catch the light. The bottom ledge is the wide one (the ticks)
    fx0, fy0, fx1, fy1 = gx - 22, gy - 19, gx + gw + 14, gy + gh + 21
    fw, fh = fx1 - fx0, fy1 - fy0
    # One straight slope all the way from the face down to the glass, on all
    # four sides, mitred at the corners. The light comes from above, so the
    # top slope (facing down) is darkest, the bottom one (facing up) lightest;
    # each also darkens a little towards the glass
    gx1, gy1 = gx + gw, gy + gh
    slopes = (
        (f"M{fx0},{fy0} H{fx1} L{gx1},{gy} H{gx} Z", "slopeTop"),
        (f"M{fx0},{fy0} L{gx},{gy} V{gy1} L{fx0},{fy1} Z", "slopeLeft"),
        (f"M{fx0},{fy1} L{gx},{gy1} H{gx1} L{fx1},{fy1} Z", "slopeBottom"),
        (f"M{fx1},{fy0} V{fy1} L{gx1},{gy1} V{gy} Z", "slopeRight"),
    )
    for path, grad in slopes:
        d.add(f'<path d="{path}" fill="url(#{grad})"/>')
    # the fold lines: where the slope leaves the face, and the mitres
    d.add(f'<path d="M{fx0},{fy1} H{fx1} V{fy0}" fill="none" stroke="#f3efe4" stroke-width="1.2"/>')
    d.add(f'<path d="M{fx0},{fy1} V{fy0} H{fx1}" fill="none" stroke="#6a6252" stroke-width="0.9"/>')
    for x0, y0, x1, y1 in ((fx0, fy0, gx, gy), (fx1, fy0, gx1, gy),
                           (fx0, fy1, gx, gy1), (fx1, fy1, gx1, gy1)):
        d.add(f'<path d="M{x0},{y0} L{x1},{y1}" stroke="#8a826f" stroke-width="0.6" opacity="0.6"/>')
    d.add(f'<rect x="{gx}" y="{gy}" width="{gw}" height="{gh}" fill="url(#glass)" filter="url(#inset)"/>')
    for tx in (615, 720, 800, 850, 893, 940, 990, 1040, 1090):
        d.add(f'<rect x="{tx - 1.5}" y="{gy + gh - 1}" width="3" height="13" fill="#e9e4d6" stroke="#8a806b" stroke-width="0.6"/>')
    for lab, cx in (("PART", 617), ("BANK/PGM#", 718), ("VOL", 803), ("EXP", 850), ("PAN", 897),
                    ("REV", 945), ("CHO", 992), ("VAR", 1041), ("KEY", 1092)):
        d.text(cx, 389, lab, 17)
    # The words and the little ridges on the frame sit level with the LCD's
    # mode arrows as the program draws them at the default window size
    # (1000 x 426: dots 3.75 px, lower row top at 164 px). Measured off a
    # render; at other sizes the LCD's whole-dot steps move the arrows a bit
    for y, lab in ((294.6, "XG"), (310.2, "GS"), (325.9, "PERFORM")):
        d.add(f'<rect x="1141" y="{y - 1.5}" width="16" height="3" fill="#e9e4d6" stroke="#8a806b" stroke-width="0.6"/>')
        d.text(1166, round(y + 5.8, 1), lab, 16, "start")

    # ---- category buttons: only their shadows and names here
    for row, cy in enumerate(CAT_Y):
        for col, cx in enumerate(CAT_X):
            d.add(f'<rect x="{cx - 42}" y="{cy - 11}" width="84" height="26" rx="7" fill="#000" '
                  f'opacity="0.45" filter="url(#blur2)"/>')
            name = CAT_NAMES[row][col]
            d.text(cx, cy - 21, name, 18 if len(name) < 12 else 16)

    # chevron, SELECT / AUDITION sockets
    d.add('<path d="M1152,438 L1121,515 L1152,592 Z" fill="#a89e88"/>')
    d.add('<path d="M1152,438 L1121,515 L1152,592 Z" fill="#000" opacity="0.18" filter="url(#inset)"/>')
    d.add('<path d="M1152,438 L1121,515 L1152,592" fill="none" stroke="#6f6653" stroke-width="1.4"/>')
    d.add('<path d="M1122.4,517 L1151,588" fill="none" stroke="#e9e3d4" stroke-width="1" opacity="0.7"/>')
    d.add('<path d="M1152.8,439 V591" fill="none" stroke="#f4efe3" stroke-width="2"/>')
    d.add('<path d="M1150.6,441 V589" fill="none" stroke="#8a806b" stroke-width="0.8"/>')
    for _, cx, cy, lab in ROUND:
        d.text(cx, 477, lab, 20)
        # a thin dark ring: the catalog shows about 2 px of it round the cap
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{ROUND_R + 1.5}" fill="#191714"/>')
    d.dots(1203, 536, 1203, 620)
    d.dots(1125, 620, 1200, 620)

    # PLG labels
    for i, (cx, lab) in enumerate(zip(PLG_X, ("MU", "PLG-1", "PLG-2", "PLG-3"))):
        d.text(cx, 626, lab, 17)
        if i < 3:
            d.add(f'<path d="M{cx + (20 if i == 0 else 32)},617 l5,4 l-5,4 z" fill="{INK}"/>')
    d.dots(1110, 620, 1122, 620)

    # ---- mode buttons: the recesses and names
    for _, cx, cy, lab in MODES:
        if lab == "SAMPLING":
            d.text(cx - 2, cy - 27, lab, 19, extra='textLength="66" lengthAdjust="spacingAndGlyphs"')
        else:
            d.text(cx + (3 if lab == "PLAY" else 0), cy - 27, lab, 19)
        d.add(f'<circle cx="{cx}" cy="{cy}" r="{MODE_R + 4}" fill="#26241f" filter="url(#inset)"/>')

    # ---- nav keys: the dark wells around them
    # ":··· ALL ···:" over the PART pair (measured off a close-up): three dots
    # each side at mid height of the word, and at each end a colon whose
    # lower dot sits on the baseline, pointing down at the keys
    ax, dot_y, base_y = 1589, 127.5, 135
    d.text(ax, base_y, "ALL", 19)
    for dx in (-43.5, -36.8, -28.0, 27.5, 35.8, 42.9):
        d.add(f'<circle cx="{ax + dx}" cy="{dot_y}" r="1.3" fill="{INK}"/>')
    for dx in (-49.7, 50.7):
        d.add(f'<circle cx="{ax + dx}" cy="{dot_y}" r="1.3" fill="{INK}"/>'
              f'<circle cx="{ax + dx}" cy="{base_y - 1.3}" r="1.3" fill="{INK}"/>')
    for x0, x1 in ((1410, 1490), (1510, 1668)):
        for cy in (176, 250, 324):
            d.add(f'<rect x="{x0}" y="{cy - 29}" width="{x1 - x0}" height="58" rx="5" fill="#151411"/>')

    # ---- the big dial's body (the dimple turns, so it is a part)
    dx, dy, dr = DIAL
    d.add(f'<circle cx="{dx}" cy="{dy}" r="{dr + 6}" fill="#8f856f" opacity="0.55"/>')
    d.add(f'<circle cx="{dx + 4}" cy="{dy + 12}" r="{dr}" fill="#000" opacity="0.5" filter="url(#blur8)"/>')
    d.add(f'<circle cx="{dx}" cy="{dy}" r="{dr}" fill="#a39e94"/>')
    d.add(f'<circle cx="{dx - 1}" cy="{dy - 1.4}" r="{dr - 1}" fill="#ffffff"/>')
    d.add(f'<circle cx="{dx}" cy="{dy}" r="{dr - 2.6}" fill="#ecebe6"/>')
    d.add(f'<ellipse cx="{dx - 52}" cy="{dy - 70}" rx="22" ry="6" transform="rotate(-38 {dx - 52} {dy - 70})" '
          f'fill="#fff" opacity="0.85"/>')
    return d


def knob_pointer(d, cx, cy, deg):
    """The pointer is a groove cut into the cap's top, from near the middle
    out to the edge: a dark slot with a thin lit lip on one side."""
    a = math.radians(deg - 90)
    ux, uy = math.cos(a), math.sin(a)
    nx, ny = -uy, ux
    r0, r1 = 8, KNOB_CAP - 2.5
    d.add(f'<line x1="{cx + r0 * ux:.2f}" y1="{cy + r0 * uy:.2f}" x2="{cx + r1 * ux:.2f}" y2="{cy + r1 * uy:.2f}" '
          f'stroke="#6b675e" stroke-width="3.2" stroke-linecap="round"/>')
    d.add(f'<line x1="{cx + r0 * ux + 1.7 * nx:.2f}" y1="{cy + r0 * uy + 1.7 * ny:.2f}" '
          f'x2="{cx + r1 * ux + 1.7 * nx:.2f}" y2="{cy + r1 * uy + 1.7 * ny:.2f}" '
          f'stroke="#ffffff" stroke-width="0.9" stroke-linecap="round" opacity="0.9"/>')


def knob_notches(d, cx, cy):
    """Short grooves cut into the cap's top edge, every 30 degrees (skipping
    the pointer): just nicks at the rim, not ribs down the side."""
    for i in range(1, 12):
        a = math.radians(i * 30 - 90)
        ux, uy = math.cos(a), math.sin(a)
        nx, ny = -uy, ux
        r0, r1 = KNOB_CAP - 6, KNOB_CAP - 1.2
        d.add(f'<line x1="{cx + r0 * ux:.2f}" y1="{cy + r0 * uy:.2f}" x2="{cx + r1 * ux:.2f}" y2="{cy + r1 * uy:.2f}" '
              f'stroke="#aca79c" stroke-width="2.2" stroke-linecap="butt"/>')
        d.add(f'<line x1="{cx + r0 * ux + 1.4 * nx:.2f}" y1="{cy + r0 * uy + 1.4 * ny:.2f}" '
              f'x2="{cx + r1 * ux + 1.4 * nx:.2f}" y2="{cy + r1 * uy + 1.4 * ny:.2f}" '
              f'stroke="#ffffff" stroke-width="0.7" opacity="0.8"/>')


# ---------------------------------------------------------------- parts
def hard_rect(d, x, y, w, h, r, face, rim_light, rim_dark, spec=True):
    """A hard plastic block seen from the front: an almost flat face, a thin
    crisp highlight along the top edge, a darker lip along the bottom, and a
    small sharp-edged glint. Nothing soft or pillowy."""
    d.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{rim_dark}"/>')
    d.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h - 1.6}" rx="{r}" fill="{rim_light}"/>')
    d.add(f'<rect x="{x + 0.6}" y="{y + 1.1}" width="{w - 1.2}" height="{h - 3.2}" rx="{max(r - 0.6, 0)}" fill="{face}"/>')
    if spec:
        d.add(f'<rect x="{x + r}" y="{y + 1.8}" width="{w * 0.42}" height="1.3" rx="0.65" fill="#fff" opacity="0.75"/>')


def part_cat(down):
    """The category buttons are not flat on top: they are close to a
    triangular prism lying on its side, a ridge running along the length at
    about 40% of the height, with rounded ends. The upper slope faces the
    light, the lower one is in shade; the crease between them is soft, with
    a thin highlight along it, and a glint sits on the upper slope at the
    left end. A thin dark edge where the button meets the recess."""
    d = Doc()
    dy = 1.0 if down else 0.0
    g = "catDn" if down else "catUp"
    x0, y0, w, h = -41, -12 + dy, 82, 24 - dy
    ridge = y0 + h * 0.4
    d.add(f'<rect x="{x0 - 0.8}" y="{y0 - 0.8}" width="{w + 1.6}" height="{h + 1.6}" rx="8.5" fill="#3a2e20"/>')
    # the two slopes, meeting at the ridge; the ends roll round
    d.add(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" rx="7.5" fill="url(#{g}V)"/>')
    d.add(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" rx="7.5" fill="url(#{g}H)"/>')
    if not down:
        # the crease catches a thin line of light
        d.add(f'<path d="M{x0 + 5},{ridge} H{x0 + w - 6}" stroke="#fff7e6" stroke-width="0.9" '
              f'stroke-linecap="round" opacity="0.75" filter="url(#blur05)"/>')
        # a glint on the upper slope at the left end, narrowing to the ridge
        d.add(f'<path d="M{x0 + 2.5},{y0 + 6} Q{x0 + 3},{y0 + 2.2} {x0 + 8},{y0 + 2.2} '
              f'L{x0 + 16},{y0 + 2.6} L{x0 + 6},{ridge - 0.6} Z" fill="#fffbf0" opacity="0.85" '
              f'filter="url(#blur05)"/>')
    return d, (-CAT_W / 2, -CAT_H / 2, CAT_W, CAT_H)


def part_key(down):
    d = Doc()
    # the key is a block: its sides show in the narrow gap between a pair,
    # lit on the left, in shade on the right, so the gap is not a black slot
    w, x = 74, -37
    if down:
        d.add(f'<rect x="{x}" y="-24" width="{w}" height="49" rx="4" fill="#77766f"/>')
        hard_rect(d, x + 1.5, -23, w - 3, 47.5, 4, "#d2d2cb", "#e2e2dc", "#8e8e87", spec=False)
    else:
        d.add(f'<rect x="{x}" y="-25" width="{w}" height="51" rx="4.5" fill="#6d6c65"/>')
        d.add(f'<rect x="{x}" y="-25" width="3" height="50" rx="1.5" fill="#d8d8d1"/>')
        d.add(f'<rect x="{x + w - 3}" y="-25" width="3" height="50" rx="1.5" fill="#9c9b94"/>')
        hard_rect(d, x + 2, -25, w - 4, 48, 4, "#e1e1db", "#fbfbf8", "#8e8e87")
        # the block's front bevel: a slightly darker band at the bottom
        d.add(f'<rect x="{x + 2.6}" y="15.5" width="{w - 5.2}" height="5.2" rx="2" fill="#c9c9c2"/>')
    return d, (-NAV_W / 2, -NAV_H / 2, NAV_W, NAV_H)


# キートップの印刷（panel.cpp の draw_key_print と同じ置き方）。長さは論理単位で書いて
# 写真の画素に直す。字は Arial Bold で 9 単位、記号は半径 4.2 単位の丸に白抜き
KEY_PRINT = {
    "mute_solo": ("MUTE/", "SOLO"), "part-": ("PART", "-"), "part+": ("PART", "+"),
    "enter": ("ENTER", ""), "select-": ("SELECT", "<"), "select+": ("SELECT", ">"),
    "exit": ("EXIT", ""), "value-": ("VALUE", "-"), "value+": ("VALUE", "+"),
}
KEY_INK = "#3e3c36"
KEY_WHITE = "#eceae2"


def key_print(d, name, down):
    u = 1.0 / S                                   # 論理 1 単位 = 写真の画素
    em = 9.0 * u
    top, h = -NAV_H / 2, NAV_H
    dy = (1.0 * u) if down else 0.0
    label, sub = KEY_PRINT[name]

    def line(txt, f):
        # 大文字の見える部分の真ん中を f の高さへ（Arial の大文字の高さ 0.716 em）
        base = top + h * f + dy + 0.716 * em / 2
        d.text(0, round(base, 2), txt, size=round(em, 2), weight="bold", family="Arial", fill=KEY_INK)

    if sub == "SOLO":
        line(label, 0.33)
        line(sub, 0.61)
        return
    if not sub:
        line(label, 0.49)
        return
    line(label, 0.33)
    cy = top + h * 0.61 + dy
    r = 4.2 * u
    a, t = 0.6 * r, 0.14 * r
    d.add(f'<circle cx="0" cy="{cy:.2f}" r="{r:.2f}" fill="{KEY_INK}"/>')
    if sub in "+-":
        d.add(f'<rect x="{-a:.2f}" y="{cy - t:.2f}" width="{2 * a:.2f}" height="{2 * t:.2f}" fill="{KEY_WHITE}"/>')
        if sub == "+":
            d.add(f'<rect x="{-t:.2f}" y="{cy - a:.2f}" width="{2 * t:.2f}" height="{2 * a:.2f}" fill="{KEY_WHITE}"/>')
    else:
        s = 1 if sub == ">" else -1
        pts = [(s * a, cy), (-s * a / 2, cy - a), (-s * a / 2, cy + a)]
        d.add('<polygon points="' + " ".join(f"{x:.2f},{y:.2f}" for x, y in pts) + f'" fill="{KEY_WHITE}"/>')


def part_key_face(name, down):
    d, vb = part_key(down)
    key_print(d, name, down)
    return d, vb


def key_file(name, down):
    base = name.replace("-", "_minus").replace("+", "_plus")
    return f"key-{base}{'-down' if down else ''}.png"


def hard_disc(d, r, face, light, dark, cy=0.0, glint=True):
    """A hard round cap: flat face, crisp light rim at the top-left and a dark
    one at the bottom-right, a small sharp glint."""
    d.add(f'<circle cy="{cy}" r="{r}" fill="{dark}"/>')
    d.add(f'<circle cx="-0.5" cy="{cy - 0.7}" r="{r - 0.6}" fill="{light}"/>')
    d.add(f'<circle cy="{cy}" r="{r - 1.4}" fill="{face}"/>')
    if glint:
        d.add(f'<ellipse cx="{-0.35 * r:.2f}" cy="{cy - 0.45 * r:.2f}" rx="{0.28 * r:.2f}" ry="{0.13 * r:.2f}" '
              f'transform="rotate(-25 {-0.35 * r:.2f} {cy - 0.45 * r:.2f})" fill="#fff" opacity="0.8"/>')


LAMP = {"green": ("#7bdc4a", "#6fce43", "#c8ff9a", "#2f6e22", "#c9ff9d"),
        "red":   ("#ff3a6a", "#f0245a", "#ffa3bb", "#7a0f28", "#ffc0d0")}


def part_mode(state, lamp="green"):
    d = Doc()
    r = MODE_R - 1
    if state == "on":
        glow, face, light, dark, core = LAMP[lamp]
        d.add(f'<circle r="{r + 0.5}" fill="{glow}" opacity="0.6" filter="url(#blur4)"/>')
        hard_disc(d, r, face, light, dark)
        d.add(f'<circle r="{r * 0.55:.2f}" fill="{core}" opacity="0.55" filter="url(#blur2)"/>')
    elif state == "down":
        hard_disc(d, r, "#4c514d", "#6d726e", "#232624", cy=0.6, glint=False)
    else:
        hard_disc(d, r, "#5a5f5b", "#8a8f8b", "#2a2d2b")
    return d, (-MODE_R, -MODE_R, 2 * MODE_R, 2 * MODE_R)


def part_round(down):
    d = Doc()
    if down:
        hard_disc(d, ROUND_R - 1, "#e6e5e0", "#f2f1ed", "#9c988e", cy=0.8, glint=False)
    else:
        hard_disc(d, ROUND_R - 1, "#f3f2ee", "#ffffff", "#9c988e")
    return d, (-ROUND_R, -ROUND_R, 2 * ROUND_R, 2 * ROUND_R)


def part_plg(on):
    d = Doc()
    if on:
        # the whole lens lights: a bright face, hotter in the middle, and a
        # little light spilling over the thin rim
        d.add('<rect x="-14" y="-8" width="28" height="16" rx="2" fill="#8dff4a" opacity="0.55" filter="url(#blur2)"/>')
        d.add('<rect x="-13" y="-7" width="26" height="14" rx="1" fill="#6fe834"/>')
        d.add('<rect x="-11" y="-5" width="22" height="10" rx="2" fill="#b8ff7c" opacity="0.8" filter="url(#blur2)"/>')
        d.add('<rect x="-12" y="-6" width="24" height="2" fill="#eaffd6" opacity="0.5"/>')
    else:
        d.add('<rect x="-13" y="-7" width="26" height="14" rx="1" fill="#1c2c1a"/>')
        d.add('<rect x="-13" y="-7" width="26" height="4" fill="#32462c" opacity="0.7"/>')
    return d, (-PLG_W / 2, -PLG_H / 2, PLG_W, PLG_H)


def part_knob():
    d = Doc()
    knob_notches(d, 0, 0)
    knob_pointer(d, 0, 0, 0)
    return d, (-KNOB_R, -KNOB_R, 2 * KNOB_R, 2 * KNOB_R)


def part_dial():
    d = Doc()
    ox, oy, r = DIMPLE
    # concave: dark just inside the rim, lighter in the middle, a bright lip.
    # The shading is round so it still reads right when the dial turns
    d.add(f'<circle cx="{ox}" cy="{oy}" r="{r + 1.2}" fill="#ffffff"/>')
    d.add(f'<circle cx="{ox}" cy="{oy}" r="{r}" fill="#b9b4aa"/>')
    d.add(f'<circle cx="{ox}" cy="{oy}" r="{r - 3}" fill="#d3cfc7"/>')
    d.add(f'<circle cx="{ox}" cy="{oy}" r="{r - 9}" fill="#e3e0da"/>')
    R = DIAL[2]
    return d, (-R, -R, 2 * R, 2 * R)


PARTS = {
    "cat.png": lambda: part_cat(False), "cat-down.png": lambda: part_cat(True),
    "key.png": lambda: part_key(False), "key-down.png": lambda: part_key(True),
    "btn.png": lambda: part_mode("off"), "btn-on.png": lambda: part_mode("on"),
    "btn-down.png": lambda: part_mode("down"),
    "btn-on-red.png": lambda: part_mode("on", "red"),
    "rnd.png": lambda: part_round(False), "rnd-down.png": lambda: part_round(True),
    "plg.png": lambda: part_plg(False), "plg-on.png": lambda: part_plg(True),
    "knob.png": part_knob, "dial.png": part_dial,
}
# キーごとの絵（印刷入り）。panel.txt の nav.face で貼る
for _name in KEY_PRINT:
    for _down in (False, True):
        PARTS[key_file(_name, _down)] = (lambda n=_name, dn=_down: part_key_face(n, dn))


# ---------------------------------------------------------------- panel.txt
def panel_txt():
    L = []
    L.append("# The \"real\" art set, drawn by tools/panel_art/make_panel.py.")
    L.append("# Regenerate with that script rather than editing the PNGs by hand.")
    L.append("")
    L.append(f"body_h {LY(Y1)}")
    gx, gy, gw, gh = LCD_GLASS
    L.append(f"lcd    {LX(gx)} {LY(gy)} {LS(gw)} {LS(gh)}")
    L.append("lcd.frame 0")
    L.append("labels art")
    L.append("")
    L.append("cat.x  " + " ".join(str(LX(x)) for x in CAT_X))
    L.append("cat.y  " + " ".join(str(LY(y - CAT_H / 2)) for y in CAT_Y))
    L.append(f"cat.size {LS(CAT_W)} {LS(CAT_H)}")
    L.append("")
    for name, x, y, _ in MODES:
        L.append(f"mode.{name:<9} {LX(x)} {LY(y)}")
    L.append(f"mode.r {LS(MODE_R)} {LS(MODE_R * 0.6)}")
    L.append("")
    for name, x, y in NAV:
        L.append(f"nav.{name:<10} {LX(x - NAV_W / 2)} {LY(y - NAV_H / 2)} {LS(NAV_W)} {LS(NAV_H)}")
    L.append("")
    for name, x, y, _ in ROUND:
        L.append(f"round.{name:<9} {LX(x)} {LY(y)} {LS(2 * ROUND_R)} {LS(2 * ROUND_R)}")
    L.append("")
    L.append(f"plg {LX(PLG_X[0])} {LS(PLG_X[1] - PLG_X[0])} {LY(PLG_Y)}")
    L.append(f"plg.size {LS(PLG_W)} {LS(PLG_H)}")
    L.append("")
    L.append(f'volume {LX(VOLUME[0])} {LY(VOLUME[1])} {LS(KNOB_R)} "knob.png"')
    L.append(f'adgain {LX(ADIN_KNOB[0])} {LY(ADIN_KNOB[1])} {LS(KNOB_R)} "knob.png"')
    L.append(f'dial   {LX(DIAL[0])} {LY(DIAL[1])} {LS(DIAL[2])} "dial.png"')
    L.append("")
    L.append(f"card   {LX(160)} {LY(598)} {LS(305)} {LS(72)}")
    L.append(f"adin   {LX(135)} {LY(150)} {LS(95)} {LS(230)}")
    L.append(f"phones {LX(400)} {LY(470)} {LS(70)} {LS(70)}")
    L.append("")
    L.append('mode.art  "btn.png" "btn-on.png" "btn-down.png"')
    L.append('mode.on sampling "btn-on-red.png"')
    L.append('nav.art   "key.png" "key-down.png"')
    for name in KEY_PRINT:
        L.append(f'nav.face {name:<10} "{key_file(name, False)}" "{key_file(name, True)}"')
    L.append('cat.art   "cat.png" "cat-down.png"')
    L.append('round.art "rnd.png" "rnd-down.png"')
    L.append('plg.art   "plg.png" "plg-on.png"')
    L.append("")
    L.append(f'art 0 0 1000 {LY(Y1)} "panel.png"')
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------- output
def inkscape():
    exe = os.environ.get("INKSCAPE") or shutil.which("inkscape")
    if not exe:
        for p in (r"C:\Program Files\Inkscape\bin\inkscape.com",
                  "/Applications/Inkscape.app/Contents/MacOS/inkscape"):
            if os.path.exists(p):
                exe = p
                break
    return exe


def main():
    svg_only = "--svg-only" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(WORK, exist_ok=True)
    jobs = []

    bg = background()
    vw, vh = X1 - X0, Y1 - Y0
    path = os.path.join(WORK, "panel.svg")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(bg.svg((X0, Y0, vw, vh), vw, vh))
    jobs.append((path, os.path.join(OUT, "panel.png"), round(1000 * BG_SCALE)))

    for name, make in PARTS.items():
        d, vb = make()
        path = os.path.join(WORK, name.replace(".png", ".svg"))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(d.svg(vb, vb[2], vb[3]))
        jobs.append((path, os.path.join(OUT, name), max(8, round(vb[2] * S * PART_SCALE))))

    with open(os.path.join(OUT, "panel.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(panel_txt())

    if svg_only:
        print("SVG written to", WORK)
        return
    exe = inkscape()
    if not exe:
        sys.exit("Inkscape not found (set INKSCAPE)")
    for svg, png, width in jobs:
        subprocess.run([exe, svg, "--export-type=png", f"--export-filename={png}",
                        f"--export-width={width}"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("wrote", os.path.relpath(png, ROOT))


if __name__ == "__main__":
    main()
