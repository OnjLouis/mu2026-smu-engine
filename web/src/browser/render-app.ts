// Browser render page: upload ROMs + MIDI, render to WAV, play/download.
// Bundled by scripts/build-page.ts into dist/render.js (wasm embedded).
// Importing this module has no side effects; call init() from the page.
import { encodeWav } from "../common/wav.ts";
import { ROM_SLOTS, setupRomCard } from "./roms.ts";
import { loadSmu } from "./smu-standalone.ts";
import type { SmuModule } from "../smu-types.ts";

const rate = 44_100;

function element<T extends HTMLElement>(id: string, type: new () => T): T {
    const found = document.querySelector(`#${id}`);
    if (found instanceof type) return found;
    throw new Error(`missing element #${id}`);
}

// Yield to the browser so the progress bar paints between chunks.
function yieldToBrowser(): Promise<void> {
    return new Promise((resolve) => {
        setTimeout(resolve, 0);
    });
}

function lastError(module_: SmuModule): string {
    const length = module_._smu_error_copy(0, 0);
    if (length === 0) return "unknown error";
    // Read through a fresh view: the buffer may have grown since last access.
    const pointer = module_._malloc(length);
    module_._smu_error_copy(pointer, length);
    const text = new TextDecoder().decode(
        module_.HEAPU8.subarray(pointer, pointer + length)
    );
    module_._free(pointer);
    return text;
}

function copyToWasm(module_: SmuModule, data: Uint8Array): number {
    const pointer = module_._malloc(data.length);
    module_.HEAPU8.set(data, pointer);
    return pointer;
}

export async function init(): Promise<void> {
    const midiInput = element("midi", HTMLInputElement);
    const midiName = element("midiName", HTMLSpanElement);
    const renderButton = element("render", HTMLButtonElement);
    const progress = element("progress", HTMLProgressElement);
    const status = element("status", HTMLSpanElement);
    const player = element("player", HTMLAudioElement);
    const download = element("download", HTMLAnchorElement);
    const result = element("result", HTMLElement);

    let midiFile: File | undefined;
    let module_: SmuModule | undefined;
    let isRendering = false;
    let currentUrl: string | undefined;

    const setStatus = (text: string) => {
        status.textContent = text;
    };
    const setProgress = (fraction: number) => {
        progress.value = Math.max(0, Math.min(1, fraction));
    };

    const refreshButton = (romFiles: Map<string, File>) => {
        const haveRequired = ROM_SLOTS.every(
            (slot) => !slot.isRequired || romFiles.has(slot.fileName)
        );
        renderButton.disabled =
            isRendering || !haveRequired || midiFile === undefined;
    };

    const card = await setupRomCard(
        element("roms", HTMLInputElement),
        element("romFolder", HTMLSpanElement),
        element("romList", HTMLUListElement),
        element("forgetRoms", HTMLButtonElement),
        refreshButton
    );
    refreshButton(card.files);

    midiInput.addEventListener("change", () => {
        midiFile = midiInput.files?.[0];
        midiName.textContent = midiFile?.name ?? "no file";
        refreshButton(card.files);
    });

    renderButton.addEventListener("click", () => {
        void runRender().catch((error: unknown) => {
            setStatus(
                `Render failed: ${error instanceof Error ? error.message : String(error)}`
            );
        });
    });

    async function runRender(): Promise<void> {
        if (isRendering || midiFile === undefined) return;
        isRendering = true;
        refreshButton(card.files);
        // Clear the previous result so renders are reusable.
        // The old object URL is revoked before a new one is minted.
        if (currentUrl !== undefined) {
            URL.revokeObjectURL(currentUrl);
            currentUrl = undefined;
        }
        player.removeAttribute("src");
        download.removeAttribute("href");
        download.textContent = "Render something first";
        result.hidden = true;
        setProgress(0);

        try {
            if (module_ === undefined) {
                setStatus("Loading module…");
                await yieldToBrowser();
                module_ = await loadSmu();
            }
            const emu = module_;

            setStatus("Uploading ROMs…");
            await yieldToBrowser();
            // USB ports, like the desktop default: song ports 1-4 reach
            // A-D discretely instead of folding 3-4 onto A-B.
            if (emu._smu_init(1) < 0) throw new Error(lastError(emu));
            for (const slot of ROM_SLOTS) {
                const file = card.files.get(slot.fileName);
                if (file === undefined) {
                    if (slot.isRequired)
                        throw new Error(`missing ${slot.fileName}`);
                    continue;
                }
                const bytes = new Uint8Array(await file.arrayBuffer());
                const pointer = copyToWasm(emu, bytes);
                const result = emu._smu_set_rom(
                    slot.kind,
                    pointer,
                    bytes.length
                );
                emu._free(pointer);
                if (result < 0) throw new Error(lastError(emu));
            }
            if (emu._smu_reset() < 0) throw new Error(lastError(emu));

            // Boot: the firmware takes ~8 s of audio before accepting MIDI.
            // One sample per step so boot ends on the exact ready sample
            // (identical to the CLI); progress updates once a second.
            const bootCap = 30 * rate;
            let booted = 0;
            let ready = 0;
            while (ready === 0 && booted < bootCap) {
                ready = emu._smu_run_blank(1);
                if (ready < 0) throw new Error(lastError(emu));
                booted++;
                if (booted % rate === 0) {
                    setStatus(`Booting… ${(booted / rate).toFixed(0)} s`);
                    setProgress(0.08 * (booted / bootCap));
                    await yieldToBrowser();
                }
            }
            if (ready === 0)
                throw new Error("firmware did not enable MIDI reception");
            setStatus(`Booted in ${(booted / rate).toFixed(1)} s, rendering…`);

            const midiBytes = new Uint8Array(await midiFile.arrayBuffer());
            {
                const pointer = copyToWasm(emu, midiBytes);
                const count = emu._smu_load_midi(pointer, midiBytes.length);
                emu._free(pointer);
                if (count < 0) throw new Error(lastError(emu));
                setStatus(`MIDI: ${count} events, rendering…`);
            }

            const songLength = emu._smu_song_length();
            const totalFrames = Math.floor((songLength + 3) * rate);
            const tailFrames = Math.floor(3 * rate);
            const chunkFrames = rate * 2;
            const outPtr = emu._malloc(chunkFrames * 4);
            try {
                const chunks: Int16Array[] = [];
                let wrote = 0;
                let tailStart = -1;
                while (wrote < totalFrames) {
                    const n = Math.min(chunkFrames, totalFrames - wrote);
                    const got = emu._smu_render_frames(outPtr, n);
                    if (got < 0) throw new Error(lastError(emu));
                    const heap = emu.HEAPU8;
                    chunks.push(
                        new Int16Array(heap.buffer, outPtr, got * 2).slice()
                    );
                    wrote += got;
                    if (emu._smu_song_done() !== 0) {
                        if (tailStart < 0) {
                            tailStart = wrote;
                            setStatus(
                                `MIDI queue drained at ${(wrote / rate).toFixed(1)} s; rendering tail…`
                            );
                        }
                        if (wrote >= tailStart + tailFrames) break;
                    }
                    setStatus(
                        `Rendering… ${(wrote / rate).toFixed(0)} s / ${songLength.toFixed(0)} s`
                    );
                    setProgress(0.08 + 0.92 * (wrote / totalFrames));
                    await yieldToBrowser();
                    if (got === 0) break;
                }
                const pcm = new Int16Array(wrote * 2);
                let at = 0;
                for (const chunk of chunks) {
                    pcm.set(chunk, at);
                    at += chunk.length;
                }
                const url = URL.createObjectURL(
                    new Blob([encodeWav(rate, pcm) as BlobPart], {
                        type: "audio/wav"
                    })
                );
                currentUrl = url;
                player.src = url;
                download.href = url;
                download.download =
                    midiFile.name.replace(/\.[^.]*$/, "") + ".wav";
                download.textContent = `Download ${download.download}`;
                result.hidden = false;
                setProgress(1);
                setStatus(
                    `Done: ${(wrote / rate).toFixed(1)} s, ${Number(emu._smu_scheduled_events())} events.`
                );
            } finally {
                emu._free(outPtr);
            }
        } finally {
            isRendering = false;
            refreshButton(card.files);
        }
    }

    setStatus(
        card.files.size > 0
            ? "ROMs stored locally. Pick a MIDI file, then Render."
            : "Pick a ROM folder and a MIDI file, then Render."
    );
}
