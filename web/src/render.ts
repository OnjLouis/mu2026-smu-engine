// Node driver: render a MIDI file to WAV using the smu_render.mjs module.
// Usage: npm run render -- --roms <rom dir> <song.mid> <out.wav> [seconds] [--usb] [--native-engine] [--native-fx-full]
// ROM layout mirrors render.cpp: <dir>/mu2000_flash.bin,
// <dir>/dump/{xv364a0.ic49,xv365a0.ic50,xw848a0.ic53,xw849a0.ic54},
// <dir>/standin/sin-table.bin (optional).
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import createModule from "../out/smu_render.mjs";
import { encodeWav } from "./common/wav.ts";
import type { SmuModule } from "./smu-types.ts";

const rate = 44_100;

function usage(): never {
    console.error(
        "usage: render --roms <dir> <song.mid> <out.wav> [seconds] [--usb] [--native-engine] [--native-fx-full]"
    );
    process.exit(1);
}

const argv = process.argv.slice(2);
let roms: string | undefined;
let seconds = 0;
let isDurationGiven = false;
let isUsb = false;
let isNativeEngine = false;
let isNativeFxFull = false;
const positional: string[] = [];
for (let index = 0; index < argv.length; index++) {
    const a = argv[index];
    // eslint-disable-next-line unicorn/prefer-switch -- five short flag tests read clearer as a chain
    if (a === "--roms") {
        roms = argv[++index];
    } else if (a === "--usb") {
        isUsb = true;
    } else if (a === "--native-engine") {
        isNativeEngine = true;
    } else if (a === "--native-fx-full") {
        isNativeFxFull = true;
    } else if (positional.length === 2 && !Number.isNaN(Number(a))) {
        seconds = Number(a);
        isDurationGiven = true;
    } else {
        positional.push(a);
    }
}
if (roms === undefined || positional.length < 2) usage();
const midPath = positional[0];
const wavPath = positional[1];

const Module: SmuModule = await createModule();

function lastError(): string {
    const length = Module._smu_error_copy(0, 0);
    if (length === 0) return "";
    const p = Module._malloc(length);
    Module._smu_error_copy(p, length);
    const s = Buffer.from(Module.HEAPU8.subarray(p, p + length)).toString(
        "utf8"
    );
    Module._free(p);
    return s;
}

function check(returnCode: number, what: string): number {
    if (returnCode < 0) {
        console.error(`${what} failed: ${lastError()}`);
        process.exit(1);
    }
    return returnCode;
}

function pushFile(kind: number, filePath: string, isRequired: boolean): void {
    let data: Buffer | undefined;
    try {
        data = readFileSync(filePath);
    } catch {
        data = undefined;
    }
    if (data === undefined) {
        if (isRequired) {
            console.error(`cannot open: ${filePath}`);
            process.exit(1);
        }
        console.error(`warning: ${filePath} missing, skipping`);
        return;
    }
    const p = Module._malloc(data.length);
    Module.HEAPU8.set(data, p);
    const returnCode = Module._smu_set_rom(kind, p, data.length);
    Module._free(p);
    check(returnCode, `smu_set_rom(${filePath})`);
    console.log(`rom kind ${kind}: ${filePath} (${data.length} bytes)`);
}

check(Module._smu_init(isUsb ? 1 : 0), "smu_init");
Module._smu_set_usb_host(isUsb ? 1 : 0);
pushFile(0, path.join(roms, "mu2000_flash.bin"), true);
pushFile(1, path.join(roms, "dump", "xv364a0.ic49"), true);
pushFile(2, path.join(roms, "dump", "xv365a0.ic50"), true);
pushFile(3, path.join(roms, "dump", "xw848a0.ic53"), true);
pushFile(4, path.join(roms, "dump", "xw849a0.ic54"), true);
pushFile(5, path.join(roms, "standin", "sin-table.bin"), false);
check(Module._smu_reset(), "smu_reset");

const boot = check(Module._smu_boot(30 * rate), "smu_boot");
console.log(
    `boot: ${boot} samples (${(boot / rate).toFixed(3)} s), MIDI ready`
);
if (isNativeFxFull) {
    Module._smu_set_native_fx(2);
    console.log("native-fx-full: on (MEG off, C++ FX)");
}
if (isNativeEngine) {
    Module._smu_set_native_engine(1);
    console.log("native-engine: on (firmware bypass, opt-in)");
}

const mid = readFileSync(midPath);
{
    const p = Module._malloc(mid.length);
    Module.HEAPU8.set(mid, p);
    const eventCount = Module._smu_load_midi(p, mid.length);
    Module._free(p);
    check(eventCount, "smu_load_midi");
    console.log(
        `MIDI: ${eventCount} events, length ${Module._smu_song_length().toFixed(2)} s`
    );
}

const songLength = Module._smu_song_length();
const totalSec = isDurationGiven ? seconds : songLength + 3;
const totalFrames = Math.floor(totalSec * rate);
const tailFrames = isDurationGiven ? 0 : Math.floor(3 * rate);

const chunk = rate * 10; // 10 s per wasm call
const outPtr = Module._malloc(chunk * 4);
const pcm = Buffer.alloc(totalFrames * 4);
let wrote = 0;
let tailStart = -1;
const t0 = Date.now();
while (wrote < totalFrames) {
    const n = Math.min(chunk, totalFrames - wrote);
    const got = Module._smu_render_frames(outPtr, n);
    if (got < 0) {
        console.error("render failed");
        process.exit(1);
    }
    Buffer.from(Module.HEAPU8.subarray(outPtr, outPtr + got * 4)).copy(
        pcm,
        wrote * 4
    );
    wrote += got;
    if (!isDurationGiven && Module._smu_song_done() !== 0) {
        if (tailStart < 0) {
            tailStart = wrote;
            console.log(
                `MIDI queue drained at ${(wrote / rate).toFixed(3)} s; rendering 3 s tail`
            );
        }
        if (wrote >= tailStart + tailFrames) break;
    }
    if (wrote % (rate * 30) === 0)
        console.log(`  ${(wrote / rate).toFixed(1)} s`);
    if (got === 0) break;
}
Module._free(outPtr);
const secs = (Date.now() - t0) / 1000;
console.log(
    `rendered ${wrote} frames in ${secs.toFixed(1)} s (${(wrote / rate / secs).toFixed(2)}x realtime)`
);
console.log(
    `scheduled events: ${Number(Module._smu_scheduled_events())}, dropped: ${Number(Module._smu_dropped())}`
);
if (isNativeEngine) {
    console.log(
        `native firmware share: ${(Module._smu_native_firmware_share() * 100).toFixed(1)}%`
    );
}

// WAV output: 16-bit stereo 44100.
const data = pcm.subarray(0, wrote * 4);
const pcm16 = new Int16Array(
    data.buffer,
    data.byteOffset,
    Math.floor(data.byteLength / 2)
).subarray(0, wrote * 2);
writeFileSync(wavPath, encodeWav(rate, pcm16));
console.log(`wrote: ${wavPath} (${(wrote / rate).toFixed(1)} s)`);
