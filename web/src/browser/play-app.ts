// Playback page: play a MIDI file through the worklet synth in realtime.
// Booting matches the live page (worklet, fast synth, boot progress).
// File picking and progress match the render page (no WebMIDI input).
// Files are parsed on the page and scheduled in the worklet clock.
// Picking another file while playing replaces the song immediately.
import type { MainToWorklet, SongEvent, WorkletToMain } from "./protocol.ts";
import { BUILD_TAG } from "./build-tag.ts";
import { ROM_SLOTS, setupRomCard } from "./roms.ts";
import { parseSmf, resolvePorts } from "./smf.ts";

function element<T extends HTMLElement>(id: string, type: new () => T): T {
    const found = document.querySelector(`#${id}`);
    if (found instanceof type) return found;
    throw new Error(`missing element #${id}`);
}

interface Staged {
    events: SongEvent[];
    length: number;
    name: string;
}

function formatTime(sec: number): string {
    const total = Math.max(0, Math.floor(sec));
    return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

export async function init(): Promise<void> {
    const bootButton = element("boot", HTMLButtonElement);
    const fastSynth = element("fastSynth", HTMLInputElement);
    const progress = element("progress", HTMLProgressElement);
    const status = element("status", HTMLSpanElement);
    const midiInput = element("midi", HTMLInputElement);
    const midiName = element("midiName", HTMLSpanElement);
    const playButton = element("play", HTMLButtonElement);
    const stopButton = element("stop", HTMLButtonElement);
    const panicButton = element("panic", HTMLButtonElement);
    const songProgress = element("songProgress", HTMLProgressElement);
    const songStatus = element("songStatus", HTMLSpanElement);

    let context: AudioContext | undefined;
    let node: AudioWorkletNode | undefined;
    let isLive = false;
    let staged: Staged | undefined;
    let isPlaying = false;

    const setStatus = (text: string): void => {
        status.textContent = text;
    };

    const refresh = (romFiles: Map<string, File>): void => {
        const haveRequired = ROM_SLOTS.every(
            (slot) => !slot.isRequired || romFiles.has(slot.fileName)
        );
        bootButton.disabled = !haveRequired || node !== undefined;
        playButton.disabled = !isLive || staged === undefined || isPlaying;
        stopButton.disabled = !isPlaying;
        panicButton.disabled = !isLive;
        // Engine choice applies at boot; lock it once the node exists.
        fastSynth.disabled = node !== undefined;
    };

    const card = await setupRomCard(
        element("roms", HTMLInputElement),
        element("romFolder", HTMLSpanElement),
        element("romList", HTMLUListElement),
        element("forgetRoms", HTMLButtonElement),
        refresh
    );
    refresh(card.files);

    const post = (message: MainToWorklet, transfer?: Transferable[]): void => {
        if (node === undefined) return;
        node.port.postMessage(message, transfer ?? []);
    };

    // USB ports: four discrete parts, like the desktop default.
    // Higher stray ports fold the way the offline renderer folds them.
    const playNow = (): void => {
        if (node === undefined || staged === undefined) return;
        const song = staged;
        post({ type: "play", events: song.events, length: song.length });
        isPlaying = true;
        songProgress.value = 0;
        songStatus.textContent = `${song.name} — 0:00 / ${formatTime(song.length)}`;
        setStatus(`Playing ${song.name} (${song.events.length} events).`);
        refresh(card.files);
    };

    bootButton.addEventListener("click", () => {
        void boot().catch((error: unknown) => {
            setStatus(
                `Boot failed: ${error instanceof Error ? error.message : String(error)}`
            );
        });
    });

    async function boot(): Promise<void> {
        if (node !== undefined) return;
        setStatus("Creating audio…");
        console.info(`[play] ${BUILD_TAG} creating AudioContext`);
        // Open the device at its native rate (often 48000).
        // Forcing 44100 risks failing device open.
        // The synth renders at 44100 internally; the worklet resamples.
        const audio = new AudioContext({ latencyHint: "interactive" });
        context = audio;
        audio.addEventListener("statechange", () => {
            console.info(`[play] audio state: ${audio.state}`);
            if (audio.state !== "running")
                setStatus(
                    `Audio ${audio.state}: output may be muted or blocked.`
                );
        });
        console.info("[play] adding worklet module");
        await audio.audioWorklet.addModule("smu-processor.js");
        console.info("[play] worklet module added");
        const worklet = new AudioWorkletNode(audio, "smu-synth", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });
        // Without this the node never processes and boot never starts.
        worklet.connect(audio.destination);
        console.info("[play] node created and connected to destination");
        node = worklet;
        refresh(card.files);
        worklet.port.addEventListener(
            "message",
            (event: MessageEvent): void => {
                void handleWorklet(event.data);
            }
        );
        worklet.port.start();
        const roms: { kind: number; data: ArrayBuffer }[] = [];
        const transfer: Transferable[] = [];
        for (const slot of ROM_SLOTS) {
            const file = card.files.get(slot.fileName);
            if (file === undefined) {
                if (slot.isRequired)
                    throw new Error(`missing ${slot.fileName}`);
                continue;
            }
            console.info(
                `[play] reading ${slot.fileName} (${file.size} bytes)`
            );
            const data = await file.arrayBuffer();
            roms.push({ kind: slot.kind, data });
            transfer.push(data);
        }
        const megabytes =
            roms.reduce((sum, rom) => sum + rom.data.byteLength, 0) / 1_048_576;
        setStatus("Loading ROMs…");
        console.info(
            `[play] posting init with ${roms.length} roms (${megabytes.toFixed(1)} MB), fastSynth=${fastSynth.checked ? "on" : "off"}`
        );
        post(
            {
                type: "init",
                roms,
                nativeEngine: fastSynth.checked,
                nativeFxFull: fastSynth.checked,
                usbHost: true
            },
            transfer
        );
        await audio.resume();
        console.info(`[play] context resumed, state=${audio.state}`);
    }

    function handleWorklet(data: unknown): void {
        if (typeof data !== "object" || data === null) return;
        const message = data as WorkletToMain;
        switch (message.type) {
            case "boot": {
                progress.value = message.fraction;
                setStatus(message.text);
                break;
            }
            case "live": {
                progress.value = 1;
                isLive = true;
                refresh(card.files);
                setStatus(
                    `Live at ${context?.sampleRate ?? 44_100} Hz (${fastSynth.checked ? "fast synth" : "exact"}). Pick a MIDI file.`
                );
                if (staged !== undefined) playNow();
                break;
            }
            case "song": {
                const name = staged?.name ?? "song";
                songProgress.value =
                    message.length > 0
                        ? Math.max(
                              0,
                              Math.min(1, message.position / message.length)
                          )
                        : 0;
                if (message.done) {
                    isPlaying = false;
                    songStatus.textContent = `${name} — finished (${formatTime(message.length)})`;
                    setStatus(
                        `Finished ${name}. Pick another file or press Play to replay.`
                    );
                    refresh(card.files);
                } else {
                    songStatus.textContent = `${name} — ${formatTime(message.position)} / ${formatTime(message.length)}`;
                }
                break;
            }
            case "error": {
                setStatus(`Synth error: ${message.message}`);
                break;
            }
        }
    }

    midiInput.addEventListener("change", () => {
        const file = midiInput.files?.[0];
        if (file === undefined) return;
        void stageFile(file).catch((error: unknown) => {
            setStatus(
                `MIDI error: ${error instanceof Error ? error.message : String(error)}`
            );
        });
    });

    async function stageFile(file: File): Promise<void> {
        const bytes = new Uint8Array(await file.arrayBuffer());
        const parsed = parseSmf(bytes);
        const events = resolvePorts(parsed.events, true, true);
        if (events.length === 0) throw new Error("no playable events");
        staged = { events, length: parsed.length, name: file.name };
        midiName.textContent = file.name;
        if (isLive) {
            // A new pick replaces whatever is playing.
            // Re-uploads take effect without touching Stop first.
            playNow();
        } else {
            setStatus(
                `Selected ${file.name} (${events.length} events, ${formatTime(parsed.length)}). Boot the synth, then Play.`
            );
            refresh(card.files);
        }
    }

    playButton.addEventListener("click", () => {
        if (!isPlaying) playNow();
    });

    stopButton.addEventListener("click", () => {
        post({ type: "stop" });
        isPlaying = false;
        songProgress.value = 0;
        songStatus.textContent = "Stopped.";
        setStatus("Stopped. Press Play to replay.");
        refresh(card.files);
    });

    panicButton.addEventListener("click", () => {
        post({ type: "panic" });
        setStatus("Panic: all notes off on all ports.");
    });

    songStatus.textContent = "No song yet.";
    setStatus(
        card.files.size > 0
            ? "ROMs stored locally. Boot the synth, then pick a MIDI file."
            : "Pick a ROM folder, boot, then pick a MIDI file."
    );
}
