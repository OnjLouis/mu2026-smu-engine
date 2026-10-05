// Harness: drive the real built worklet bundle (dist/smu-processor.js)
// with stubbed AudioWorklet globals, real ROMs, and 128-frame quanta.
// Calls process() with the real browser arity: process(inputs, outputs).
// Verifies: boot reaches live, progress flows, MIDI renders audio.
// Run from web/: npm run harness (needs ../roms).
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..", "..");
const dist = path.resolve(here, "..", "dist");

// --- Stub the worklet environment before importing the bundle. ---
globalThis.sampleRate = 44_100;

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
    [0, "roms/mu2000_flash.bin"],
    [1, "roms/dump/xv364a0.ic49"],
    [2, "roms/dump/xv365a0.ic50"],
    [3, "roms/dump/xw848a0.ic53"],
    [4, "roms/dump/xw849a0.ic54"],
    [5, "roms/standin/sin-table.bin"]
];
const roms = romFiles.map(([kind, relative]) => {
    const data = readFileSync(path.resolve(root, relative));
    const buffer = data.buffer.slice(
        data.byteOffset,
        data.byteOffset + data.byteLength
    );
    return { kind, data: buffer };
});
console.log(
    `roms: ${roms.length} images, ${(roms.reduce((sum, rom) => sum + rom.data.byteLength, 0) / 1_048_576).toFixed(1)} MB`
);

if (messageHandler === undefined) throw new Error("no message handler");
const fast = process.env.FAST !== "0";
console.log(`engine: ${fast ? "fast synth (native)" : "exact"}`);
messageHandler({
    data: {
        type: "init",
        roms,
        nativeEngine: fast,
        nativeFxFull: fast
    }
});
// Let the async init (wasm load + reset) finish.
for (let index = 0; index < 40; index++) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    if (posted.length > 0) break;
    // Drive idle quanta while init is pending.
    processor.process([], [[new Float32Array(128), new Float32Array(128)]]);
}

const quantum = () => [new Float32Array(128), new Float32Array(128)];
// Boot across quanta exactly like the browser would.
let live = false;
let bootMessages = 0;
let maxQuantumMs = 0;
let quanta = 0;
const bootStart = Date.now();
while (!live && quanta < 3000) {
    const start = Date.now();
    const keepAlive = processor.process([], [quantum()]);
    const elapsed = Date.now() - start;
    maxQuantumMs = Math.max(maxQuantumMs, elapsed);
    quanta++;
    if (!keepAlive) throw new Error("processor returned false during boot");
    for (const message of posted.splice(0)) {
        if (message.type === "boot") bootMessages++;
        else if (message.type === "live") live = true;
        else if (message.type === "error")
            throw new Error(`worklet error: ${String(message.message)}`);
        else throw new Error(`unexpected message: ${message.type}`);
    }
    if (quanta % 200 === 0)
        console.log(`  ... ${quanta} quanta, max slice ${maxQuantumMs} ms`);
}
const bootWall = (Date.now() - bootStart) / 1000;
console.log(
    `boot: ${live ? "LIVE" : "NOT LIVE"} after ${quanta} quanta (${bootWall.toFixed(1)} s wall), ${bootMessages} progress posts, max quantum ${maxQuantumMs} ms`
);
if (!live) process.exit(1);

// Play a note on port A and render 2 s; check the output is non-silent.
messageHandler({
    data: { type: "midi", port: 0, bytes: [0x90, 60, 100] }
});
let peak = 0;
let sumSquares = 0;
let frames = 0;
const renderStart = Date.now();
let maxLiveMs = 0;
for (let index = 0; index < 345; index++) {
    const [left, right] = quantum();
    const start = Date.now();
    processor.process([], [[left, right]]);
    maxLiveMs = Math.max(maxLiveMs, Date.now() - start);
    for (let frame = 0; frame < left.length; frame++) {
        const sample = left[frame] ?? 0;
        peak = Math.max(peak, Math.abs(sample));
        sumSquares += sample * sample;
        frames++;
    }
}
const rms = Math.sqrt(sumSquares / Math.max(1, frames));
console.log(
    `live render: ${frames} frames in ${((Date.now() - renderStart) / 1000).toFixed(1)} s wall, peak ${peak.toFixed(4)}, rms ${rms.toFixed(4)}, max quantum ${maxLiveMs} ms`
);
if (rms < 0.0001) throw new Error("rendered silence after note-on");
console.log("HARNESS OK");
