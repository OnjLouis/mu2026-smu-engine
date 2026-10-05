# license:BSD-3-Clause
"""2 つの wav を 50ms の窓・オクターブ帯ごとに比べる。
使い方: bands.py a.wav b.wav [start_sec]
出力: 帯ごとの差（dB）の、音のある窓での平均と最大。音の大きさ（全体の rms）の差も"""
import sys, wave
import numpy as np


def load(p):
    w = wave.open(p)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64).reshape(-1, w.getnchannels())
    return a.mean(axis=1), w.getframerate()


def bands(x, sr, start):
    x = x[int(start * sr):]
    win = int(0.05 * sr)
    edges = [63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    out = []
    for i in range(0, len(x) - win, win):
        f = np.abs(np.fft.rfft(x[i:i + win] * np.hanning(win))) ** 2
        fr = np.fft.rfftfreq(win, 1 / sr)
        out.append([f[(fr >= lo) & (fr < hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])])
    return np.array(out)


a, sr = load(sys.argv[1])
b, _ = load(sys.argv[2])
start = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
n = min(len(a), len(b))
A, B = bands(a[:n], sr, start), bands(b[:n], sr, start)
floor = A.max() * 1e-6          # 窓・帯のいちばん大きいところから -60dB より上だけ
m = (A > floor) & (B > floor * 0.01)
d = 10 * np.log10((B[m] + 1e-9) / (A[m] + 1e-9))
ra = np.sqrt(np.mean(a[int(start * sr):n] ** 2)); rb = np.sqrt(np.mean(b[int(start * sr):n] ** 2))
per_band = []
for j in range(A.shape[1]):
    mj = m[:, j]
    per_band.append(np.mean(10 * np.log10((B[mj, j] + 1e-9) / (A[mj, j] + 1e-9))) if mj.any() else float('nan'))
print('rms %+.2f dB  bands: mean|d| %.2f dB  max|d| %.1f dB  (%d cells)  per-band %s' % (
    20 * np.log10(rb / ra), np.mean(np.abs(d)), np.max(np.abs(d)), m.sum(), ' '.join('%+.1f' % v for v in per_band)))
