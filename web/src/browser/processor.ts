// Synth AudioWorklet processor with one mu2000 instance inside.
// Synchronous emulation needs a fast CPU.
// MIDI applies within one quantum this way.
// No grouping, no lookahead lag.
// Booting runs chunked across quanta (silence out), then realtime audio.
// MIDI arrives as page messages, live or as a scheduled song file.
// Static loader import only: dynamic import is disallowed on the scope.
import type { SmuModule } from "../smu-types.ts";
import type { MainToWorklet, SongEvent, WorkletToMain } from "./protocol.ts";
import { BUILD_TAG } from "./build-tag.ts";
import { loadSmu } from "./smu-standalone.ts";

const rate = 44_100;
const bootCap = 30 * rate;
// Small boot slices keep every render quantum short.
// A full second per quantum blocks the audio thread far too long.
// Browsers then stop calling the processor, stalling boot silently.
const bootSlice = 512;
const scratchFrames = 8192;
// Song playback: 2 s of reverb tail after the last event.
// Progress posts back about 10 times a second (32 quanta of 128).
const songTailFrames = 2 * rate;
const songReportEvery = 32;

const moduleReady: Promise<SmuModule> = (async () => {
    const emu = await loadSmu();
    console.info("[worklet] wasm module loaded");
    return emu;
})();

// Module-load marker: proves which bundle the browser actually runs.
// Worklet rate is logged alongside it.
// Stale dist is the usual suspect: this line tells which build runs.
console.info(`[worklet] processor ${BUILD_TAG}, sampleRate=${sampleRate}`);

interface PendingMidi {
    port: number;
    bytes: number[];
}

function post(port: MessagePort, message: WorkletToMain): void {
    port.postMessage(message);
}

class SmuProcessor extends AudioWorkletProcessor {
    private emu: SmuModule | undefined;
    private state: "idle" | "booting" | "live" = "idle";
    private booted = 0;
    private bootSecond = -1;
    private reportedFatal = false;
    private readonly midi: PendingMidi[] = [];
    private song: SongEvent[] | undefined;
    private songAt = 0;
    private songFrame = 0;
    private songLength = 0;
    private songTail = 0;
    private songNotified = true;
    private songReport = 0;
    private outPtr = 0;
    private midiPtr = 0;
    private midiCap = 0;
    // Resampling FIFO for non-44100 device rates.
    // Typed-array ring buffered at fifoLength, consumed from fifoPosition.
    // No Array.push/splice boxing or GC on the audio thread.
    // Topped up every quantum, so nothing is ever lost.
    private fifoLeft = new Float32Array(16_384);
    private fifoRight = new Float32Array(16_384);
    private fifoLength = 0;
    private fifoPosition = 0;
    private wantNativeEngine = false;
    private loggedFirstQuantum = false;

    public constructor() {
        super();
        this.port.addEventListener("message", (event: MessageEvent): void => {
            void this.handleMessage(event.data);
        });
        // AddEventListener does not start the port (unlike onmessage).
        // Without this call, page-to-worklet messages never arrive.
        this.port.start();
    }

    private async handleMessage(data: unknown): Promise<void> {
        if (typeof data !== "object" || data === null) return;
        const message = data as MainToWorklet;
        switch (message.type) {
            case "init": {
                await this.init(
                    message.roms,
                    message.nativeEngine === true,
                    message.nativeFxFull === true,
                    message.usbHost === true
                );
                break;
            }
            case "midi": {
                if (this.state === "live" && this.midi.length < 4096) {
                    this.midi.push({
                        port: message.port,
                        bytes: message.bytes
                    });
                }
                break;
            }
            case "play": {
                this.playSong(message.events, message.length);
                break;
            }
            case "stop": {
                this.stopSong();
                break;
            }
            case "panic": {
                this.panic();
                break;
            }
        }
    }

    private async init(
        roms: { kind: number; data: ArrayBuffer }[],
        isNativeEngine: boolean,
        isNativeFxFull: boolean,
        isUsbHost: boolean
    ): Promise<void> {
        try {
            console.info(
                `[worklet] init with ${roms.length} roms, nativeEngine=${isNativeEngine ? "on" : "off"}, nativeFx=${isNativeFxFull ? "full" : "off"}, usb=${isUsbHost ? "on" : "off"}`
            );
            const emu = await moduleReady;
            this.emu = emu;
            if (emu._smu_init(isUsbHost ? 1 : 0) < 0)
                throw new Error("init failed");
            for (const rom of roms) {
                const bytes = new Uint8Array(rom.data);
                const pointer = emu._malloc(bytes.length);
                emu.HEAPU8.set(bytes, pointer);
                const result = emu._smu_set_rom(
                    rom.kind,
                    pointer,
                    bytes.length
                );
                emu._free(pointer);
                if (result < 0) throw new Error("bad ROM");
            }
            if (emu._smu_reset() < 0) throw new Error("reset failed");
            // Lightweight FX runs from boot; the native engine waits for
            // Boot completion (it needs firmware), mirroring render.cpp.
            if (isNativeFxFull) emu._smu_set_native_fx(2);
            this.wantNativeEngine = isNativeEngine;
            console.info("[worklet] reset done, booting across quanta");
            this.outPtr = emu._malloc(scratchFrames * 4);
            this.midiCap = 4096;
            this.midiPtr = emu._malloc(this.midiCap);
            this.fifoLength = 0;
            this.fifoPosition = 0;
            this.booted = 0;
            this.bootSecond = -1;
            this.reportedFatal = false;
            this.state = "booting";
            // Heartbeat through the port, independent of process().
            // Delivery proof: this post arrives even if quanta never run.
            post(this.port, {
                type: "boot",
                fraction: 0,
                text: "Boot starting…"
            });
        } catch (error) {
            console.info(
                `[worklet] init failed: ${error instanceof Error ? error.message : String(error)}`
            );
            post(this.port, {
                type: "error",
                message: error instanceof Error ? error.message : String(error)
            });
        }
    }

    private panic(): void {
        const emu = this.emu;
        if (emu === undefined || this.state !== "live") return;
        // All-notes-off on every channel of every port.
        // Reuses the outPtr scratch bytes: no malloc on the audio thread.
        const heap = emu.HEAPU8;
        for (let port = 0; port < 4; port++) {
            for (let channel = 0; channel < 16; channel++) {
                heap[this.outPtr] = 0xb0 | channel;
                heap[this.outPtr + 1] = 123;
                heap[this.outPtr + 2] = 0;
                emu._smu_midi_in(port, this.outPtr, 3);
            }
        }
    }

    // Start a parsed song on the sample clock.
    // Replacing cuts the previous song with all-notes-off first.
    private playSong(events: SongEvent[], length: number): void {
        if (this.emu === undefined || this.state !== "live") {
            post(this.port, { type: "error", message: "synth is not booted" });
            return;
        }
        this.stopSong();
        if (events.length === 0) {
            post(this.port, { type: "song", position: 0, length, done: true });
            return;
        }
        this.song = events;
        this.songAt = 0;
        this.songFrame = 0;
        this.songLength = length;
        this.songTail = songTailFrames;
        this.songNotified = false;
        this.songReport = 0;
        post(this.port, { type: "song", position: 0, length, done: false });
    }

    // Drop the song and silence it.
    // Playback position resets: replaying re-posts the song.
    private stopSong(): void {
        this.song = undefined;
        this.songAt = 0;
        this.songFrame = 0;
        this.songTail = 0;
        this.songNotified = true;
        this.panic();
    }

    // Move the song clock by the internal frames just rendered.
    // Reports progress back, ending with a tail after the last event.
    private advanceSong(internal: number): void {
        if (this.song === undefined) return;
        this.songFrame += internal;
        if (this.songAt >= this.song.length) {
            this.songTail -= internal;
            if (this.songTail <= 0 && !this.songNotified) {
                this.songNotified = true;
                const length = this.songLength;
                this.song = undefined;
                post(this.port, {
                    type: "song",
                    position: length,
                    length,
                    done: true
                });
                return;
            }
        }
        this.songReport++;
        if (this.songReport % songReportEvery === 0) {
            post(this.port, {
                type: "song",
                position: this.songFrame / rate,
                length: this.songLength,
                done: false
            });
        }
    }

    // Fresh int16 view over the scratch buffer.
    // The wasm buffer can grow, so views are never kept.
    private view(emu: SmuModule): Int16Array {
        return new Int16Array(
            emu.HEAPU8.buffer,
            this.outPtr,
            scratchFrames * 2
        );
    }

    private renderResampled(
        emu: SmuModule,
        left: Float32Array,
        right: Float32Array
    ): number {
        const ratio = rate / sampleRate;
        const scale = 1 / 32_768;
        // Top the FIFO up so the whole quantum interpolates from real frames.
        const want = Math.ceil(this.fifoPosition + left.length * ratio) + 1;
        let need = want - this.fifoLength;
        let produced = 0;
        const heap = emu.HEAPU8;
        while (need > 0) {
            const got = emu._smu_render_frames(
                this.outPtr,
                Math.min(512, need, scratchFrames)
            );
            if (got <= 0) break;
            produced += got;
            // Grow the FIFO rarely (first huge chord); steady state never grows.
            if (this.fifoLength + got > this.fifoLeft.length) {
                const grownLeft = new Float32Array(
                    (this.fifoLength + got) * 2
                );
                const grownRight = new Float32Array(
                    (this.fifoLength + got) * 2
                );
                grownLeft.set(this.fifoLeft.subarray(0, this.fifoLength));
                grownRight.set(this.fifoRight.subarray(0, this.fifoLength));
                this.fifoLeft = grownLeft;
                this.fifoRight = grownRight;
            }
            const scratch = new Int16Array(
                heap.buffer,
                this.outPtr,
                got * 2
            );
            for (let index = 0; index < got; index++) {
                this.fifoLeft[this.fifoLength + index] =
                    scratch[index * 2] * scale;
                this.fifoRight[this.fifoLength + index] =
                    scratch[index * 2 + 1] * scale;
            }
            this.fifoLength += got;
            need -= got;
        }
        for (let index = 0; index < left.length; index++) {
            const position = this.fifoPosition + index * ratio;
            const base = Math.floor(position);
            const frac = position - base;
            const left0 = this.fifoLeft[base] ?? 0;
            const left1 = this.fifoLeft[base + 1] ?? left0;
            const right0 = this.fifoRight[base] ?? 0;
            const right1 = this.fifoRight[base + 1] ?? right0;
            left[index] = left0 + (left1 - left0) * frac;
            right[index] = right0 + (right1 - right0) * frac;
        }
        this.fifoPosition += left.length * ratio;
        // Compact the consumed prefix once in a while.
        if (this.fifoPosition > 4096) {
            const drop = Math.floor(this.fifoPosition);
            this.fifoLeft.copyWithin(0, drop, this.fifoLength);
            this.fifoRight.copyWithin(0, drop, this.fifoLength);
            this.fifoLength -= drop;
            this.fifoPosition -= drop;
        }
        return produced;
    }

    // One quantum: boot chunks run here with silence out.
    // Live quanta feed due song events, drain the MIDI queue, then render.
    // Never throws on the audio thread.
    // A throw would kill the processor silently.
    // Failures report to the page once instead.
    // Private work method first: class member order is enforced by lint.
    // The first parameter is inputs (always empty here).
    // Outputs come second: mixing them up stalls boot silently.
    private processInner(
        _inputs: Float32Array[][],
        outputs: Float32Array[][]
    ): boolean {
        if (!this.loggedFirstQuantum) {
            this.loggedFirstQuantum = true;
            console.info("[worklet] first quantum");
        }
        const [left, right] = outputs[0] ?? [];
        if (left === undefined || right === undefined) return true;
        const emu = this.emu;
        if (emu === undefined || this.state === "idle") {
            left.fill(0);
            right.fill(0);
            return true;
        }
        if (this.state === "booting") {
            const remaining = bootCap - this.booted;
            if (remaining <= 0) {
                this.state = "idle";
                post(this.port, {
                    type: "error",
                    message: "firmware did not boot"
                });
                return true;
            }
            const ran = Math.min(bootSlice, remaining);
            const ready = emu._smu_run_blank(ran);
            this.booted += ran;
            left.fill(0);
            right.fill(0);
            if (ready !== 0) {
                if (this.wantNativeEngine) {
                    emu._smu_set_native_engine(1);
                    console.info("[worklet] native engine on");
                }
                this.state = "live";
                console.info("[worklet] live");
                post(this.port, { type: "live" });
            } else if (this.booted >= bootCap) {
                this.state = "idle";
                post(this.port, {
                    type: "error",
                    message: "firmware did not boot"
                });
            } else {
                // Progress posts stay rare: one per emulated second.
                // Every-quantum posts would flood the main thread
                // (about 345 quanta/s at 128 frames).
                const second = Math.floor(this.booted / rate);
                if (second !== this.bootSecond) {
                    this.bootSecond = second;
                    post(this.port, {
                        type: "boot",
                        fraction: this.booted / bootCap,
                        text: `Booting… ${second} s`
                    });
                }
            }
            return true;
        }
        // No copy: message events cannot interleave a running quantum.
        // Single staging buffer: one malloc at init, offsets per message.
        // Avoids per-message malloc/free on the audio thread.
        // Song playback: due events join the MIDI queue within the quantum.
        // File timing stays sample-accurate this way.
        if (this.song !== undefined) {
            const internal =
                sampleRate === rate
                    ? left.length
                    : Math.ceil(left.length * (rate / sampleRate)) + 2;
            const horizon = this.songFrame + internal;
            while (this.songAt < this.song.length) {
                if (this.midi.length >= 4096) break;
                const event = this.song[this.songAt];
                if (event === undefined) break;
                if (event.time * rate > horizon) break;
                this.midi.push({ port: event.port, bytes: event.bytes });
                this.songAt++;
            }
        }
        if (this.midi.length > 0) {
            let total = 0;
            for (const message of this.midi) total += message.bytes.length;
            if (total > this.midiCap) {
                emu._free(this.midiPtr);
                this.midiCap = total;
                this.midiPtr = emu._malloc(this.midiCap);
            }
            const heap = emu.HEAPU8;
            let at = 0;
            const offsets: number[] = [];
            for (const message of this.midi) {
                heap.set(message.bytes, this.midiPtr + at);
                offsets.push(at);
                at += message.bytes.length;
            }
            for (let index = 0; index < this.midi.length; index++) {
                const message = this.midi[index];
                if (message !== undefined) {
                    emu._smu_midi_in(
                        message.port,
                        this.midiPtr + (offsets[index] ?? 0),
                        message.bytes.length
                    );
                }
            }
            this.midi.length = 0;
        }
        let internal: number;
        if (sampleRate === rate) {
            const got = emu._smu_render_frames(this.outPtr, left.length);
            const scratch = this.view(emu);
            const scale = 1 / 32_768;
            for (let index = 0; index < got; index++) {
                left[index] = scratch[index * 2] * scale;
                right[index] = scratch[index * 2 + 1] * scale;
            }
            internal = got;
        } else {
            internal = this.renderResampled(emu, left, right);
        }
        this.advanceSong(internal);
        return true;
    }

    public override process(
        _inputs: Float32Array[][],
        outputs: Float32Array[][]
    ): boolean {
        try {
            return this.processInner(_inputs, outputs);
        } catch (error) {
            const message =
                error instanceof Error && error.stack !== undefined
                    ? error.stack
                    : String(error);
            console.info(`[worklet] process failed: ${message}`);
            const [left, right] = outputs[0] ?? [];
            left?.fill(0);
            right?.fill(0);
            if (!this.reportedFatal) {
                this.reportedFatal = true;
                this.state = "idle";
                post(this.port, { type: "error", message });
            }
            return true;
        }
    }
}

registerProcessor("smu-synth", SmuProcessor);
