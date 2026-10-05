// Song harness: drive the real built worklet bundle (dist/smu-processor.js)
// through a whole MIDI file the way live playing does — timed midi_in
// messages per 128-frame quantum, no song scheduler involved.
// Covers what the single-note harness cannot: dense polyphony, CC sweeps,
// XG SysEx (effect/variation changes), and multi-port routing.
//
// Usage from web/:
//   node scripts/worklet-song-harness.mjs <song.mid> [--seconds 30] [--fast 0|1] [--roms ../roms]
// Defaults mirror the live page (fast synth on). Needs ../roms.
// Exits non-zero on synth error or rendered silence.
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..");
const dist = path.resolve(here, "..", "dist");

const rate = 44_100;
const quantumFrames = 128;
// worrying quantum on the audio thread; the browser budget is ~2.9 ms.
const budgetMs = (quantumFrames / rate) * 1000;

const argv = process.argv.slice(2);
const positional = [];
let secondsCap = 30;
let fast = process.env.FAST === "1";
let romsDir = path.resolve(repoRoot, "roms");
for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--seconds") secondsCap = Number(argv[++i]);
    else if (a === "--fast") fast = argv[++i] !== "0";
    else if (a === "--roms") romsDir = path.resolve(String(argv[++i]));
    else positional.push(a);
}
if (positional.length < 1 || !(secondsCap > 0)) {
    console.error(
        "usage: worklet-song-harness.mjs <song.mid> [--seconds 30] [--fast 0|1] [--roms ../roms]"
    );
    process.exit(1);
}
const songPath = path.resolve(String(positional[0]));

// --- Minimal SMF parser (mirrors the parts of src/smf.cpp the live path needs).
// Tempo map, FF 0x21 port prefix, and the Yamaha sequencer-specific port
// meta (FF 7F 04 43 00 01 pp). Track/device-name routing is skipped:
// MU2000-targeted files announce ports explicitly.
function readVlq(data, pos) {
    let value = 0;
    let i = pos;
    for (;;) {
        if (i >= data.length) throw new Error("truncated VLQ");
        const b = data[i++];
        value = (value << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return [value, i];
}

function parseSmf(buffer) {
    const data = new Uint8Array(buffer);
    const text = (p, n) =>
        String.fromCharCode(...data.subarray(p, p + n));
    if (text(0, 4) !== "MThd") throw new Error("not a MIDI file");
    const headerLen =
        (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    const ntracks = (data[10] << 8) | data[11];
    const division = (data[12] << 8) | data[13];
    if (division & 0x8000) throw new Error("SMPTE division unsupported");
    let p = 8 + headerLen;
    const raw = [];
    for (let t = 0; t < ntracks; t++) {
        if (text(p, 4) !== "MTrk") throw new Error("expected MTrk");
        const len =
            (data[p + 4] << 24) |
            (data[p + 5] << 16) |
            (data[p + 6] << 8) |
            data[p + 7];
        const end = p + 8 + len;
        p += 8;
        let tick = 0;
        let running = 0;
        let port = 0;
        while (p < end) {
            const [delta, p1] = readVlq(data, p);
            p = p1;
            tick += delta;
            let status = data[p];
            if (status < 0x80) {
                if (running === 0) throw new Error("stray data byte");
                status = running;
            } else {
                p++;
                if (status < 0xf0) running = status;
            }
            if (status === 0xff) {
                const type = data[p++];
                const [mlen, p2] = readVlq(data, p);
                p = p2;
                if (type === 0x51 && mlen === 3) {
                    raw.push({
                        tick,
                        tempo:
                            (data[p] << 16) |
                            (data[p + 1] << 8) |
                            data[p + 2]
                    });
                } else if (type === 0x21 && mlen === 1) {
                    port = data[p] & 3;
                } else if (
                    type === 0x7f &&
                    mlen === 4 &&
                    data[p] === 0x43 &&
                    data[p + 1] === 0x00 &&
                    data[p + 2] === 0x01
                ) {
                    port = data[p + 3] & 3;
                }
                p += mlen;
                continue;
            }
            if (status === 0xf0 || status === 0xf7) {
                const [slen, p2] = readVlq(data, p);
                p = p2;
                const bytes =
                    status === 0xf0
                        ? [0xf0, ...data.subarray(p, p + slen)]
                        : [...data.subarray(p, p + slen)];
                p += slen;
                raw.push({ tick, bytes, port });
                continue;
            }
            const hi = status & 0xf0;
            const nb =
                hi === 0xc0 || hi === 0xd0
                    ? 1
                    : hi === 0xf0
                      ? status === 0xf2
                          ? 2
                          : status === 0xf1 || status === 0xf3
                            ? 1
                            : 0
                      : 2;
            const bytes = [status];
            for (let k = 0; k < nb && p < end; k++) bytes.push(data[p++]);
            raw.push({ tick, bytes, port });
        }
        p = end;
    }
    raw.sort((a, b) => a.tick - b.tick);
    const events = [];
    let sec = 0;
    let last = 0;
    let usPerBeat = 500_000;
    for (const e of raw) {
        sec += ((e.tick - last) * usPerBeat) / (division * 1e6);
        last = e.tick;
        if (e.tempo !== undefined) {
            usPerBeat = e.tempo;
            continue;
        }
        if (e.bytes !== undefined) events.push({ sec, bytes: e.bytes, port: e.port });
    }
    return { events, division };
}

const song = readFileSync(songPath);
const { events } = parseSmf(song);
console.log(
    `song: ${songPath} (${song.length} bytes, ${events.length} events)`
);

// --- Stub the worklet environment before importing the bundle. ---
globalThis.sampleRate = rate;

const posted = [];
let messageHandler;

class FakePort {
    addEventListener(_type, handler) {
        messageHandler = handler;
    }

    start() {}

    postMessage(message) {
        posted.push(message);
    }
}

globalThis.AudioWorkletProcessor = class {
    constructor() {
        this.port = new FakePort();
    }
};

let processorCtor;

globalThis.registerProcessor = (name, ctor) => {
    console.log(`registered: ${name}`);
    processorCtor = ctor;
};

await import(pathToFileURL(path.resolve(dist, "smu-processor.js")).href);
if (processorCtor === undefined) throw new Error("no processor registered");
const processor = new processorCtor();

const romFiles = [
    [0, "mu2000_flash.bin"],
    [1, "dump/xv364a0.ic49"],
    [2, "dump/xv365a0.ic50"],
    [3, "dump/xw848a0.ic53"],
    [4, "dump/xw849a0.ic54"],
    [5, "standin/sin-table.bin"]
];
const roms = romFiles.map(([kind, relative]) => {
    const data = readFileSync(path.resolve(romsDir, relative));
    const buffer = data.buffer.slice(
        data.byteOffset,
        data.byteOffset + data.byteLength
    );
    return { kind, data: buffer };
});
console.log(
    `roms: ${roms.length} images, ${(roms.reduce((sum, rom) => sum + rom.data.byteLength, 0) / 1_048_576).toFixed(1)} MB`
);
console.log(`engine: ${fast ? "fast synth (native)" : "exact"}`);

if (messageHandler === undefined) throw new Error("no message handler");
messageHandler({
    data: {
        type: "init",
        roms,
        nativeEngine: fast,
        nativeFxFull: fast,
        // USB ports like the live page, so ports C-D sound too.
        usbHost: true
    }
});
// Let the async init (wasm load + reset) finish.
for (let index = 0; index < 40; index++) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    if (posted.length > 0) break;
    processor.process([], [[new Float32Array(quantumFrames), new Float32Array(quantumFrames)]]);
}

const quantum = () => [new Float32Array(quantumFrames), new Float32Array(quantumFrames)];
// Boot across quanta exactly like the browser would.
let live = false;
let bootMessages = 0;
let quanta = 0;
const bootStart = Date.now();
while (!live && quanta < 4000) {
    processor.process([], [quantum()]);
    quanta++;
    for (const message of posted.splice(0)) {
        if (message.type === "boot") bootMessages++;
        else if (message.type === "live") live = true;
        else if (message.type === "error")
            throw new Error(`worklet error: ${String(message.message)}`);
        else throw new Error(`unexpected message: ${message.type}`);
    }
}
console.log(
    `boot: ${live ? "LIVE" : "NOT LIVE"} after ${quanta} quanta (${((Date.now() - bootStart) / 1000).toFixed(1)} s wall), ${bootMessages} progress posts`
);
if (!live) process.exit(1);

const capSample = Math.floor(secondsCap * rate);
let next = 0;
let fed = 0;
let fedFirstSec = 0;
let sysexFed = 0;
let rendered = 0;
let peak = 0;
let sumSquares = 0;
let frames = 0;
let quantaSong = 0;
let overruns = 0;
let maxMs = 0;
let sumMs = 0;
const songStart = Date.now();
for (;;) {
    const qStart = rendered;
    const qEnd = qStart + quantumFrames;
    if (qStart >= capSample + rate) break;
    while (
        next < events.length &&
        events[next].sec * rate < qEnd &&
        events[next].sec * rate < capSample + rate
    ) {
        const e = events[next++];
        messageHandler({
            data: { type: "midi", port: e.port, bytes: e.bytes }
        });
        fed++;
        if (e.sec < 1) fedFirstSec++;
        if (e.bytes[0] === 0xf0) sysexFed++;
    }
    const [left, right] = quantum();
    const t0 = Date.now();
    processor.process([], [[left, right]]);
    const elapsed = Date.now() - t0;
    sumMs += elapsed;
    maxMs = Math.max(maxMs, elapsed);
    if (elapsed > budgetMs) overruns++;
    quantaSong++;
    for (let frame = 0; frame < left.length; frame++) {
        const sample = left[frame] ?? 0;
        peak = Math.max(peak, Math.abs(sample));
        sumSquares += sample * sample;
        frames++;
    }
    rendered = qEnd;
    if (next >= events.length && rendered >= capSample) break;
    if (quantaSong % 2000 === 0)
        console.log(
            `  ... ${(rendered / rate).toFixed(1)} s audio, max quantum ${maxMs} ms`
        );
}
const wall = (Date.now() - songStart) / 1000;
const rms = Math.sqrt(sumSquares / Math.max(1, frames));
console.log(
    `song: ${(rendered / rate).toFixed(1)} s audio in ${wall.toFixed(1)} s wall (${(rendered / rate / wall).toFixed(2)}x realtime), ${fed} events (${fedFirstSec} in first 1 s, ${sysexFed} sysex), ${quantaSong} quanta`
);
console.log(
    `quanta: avg ${(sumMs / Math.max(1, quantaSong)).toFixed(2)} ms, max ${maxMs} ms, overruns ${overruns} (>${budgetMs.toFixed(1)} ms)`
);
console.log(`level: peak ${peak.toFixed(4)}, rms ${rms.toFixed(4)}`);
if (rms < 0.0001) throw new Error("rendered silence");
console.log("SONG HARNESS OK");
