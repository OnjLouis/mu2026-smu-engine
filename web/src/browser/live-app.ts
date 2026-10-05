// Live page with WebMIDI routing and activity dots.
// The synth runs in the worklet, synchronously.
// Fast CPU required: MIDI then applies within one quantum.
// Nothing groups or lags.
// This thread only moves ROMs and MIDI around and drives the UI.
import type { MainToWorklet, RomImage, WorkletToMain } from "./protocol.ts";
import { BUILD_TAG } from "./build-tag.ts";
import { ROM_SLOTS, setupRomCard } from "./roms.ts";

const portNames = ["A", "B", "C", "D"];

function element<T extends HTMLElement>(id: string, type: new () => T): T {
    const found = document.querySelector(`#${id}`);
    if (found instanceof type) return found;
    throw new Error(`missing element #${id}`);
}

export async function init(): Promise<void> {
    const startButton = element("start", HTMLButtonElement);
    const panicButton = element("panic", HTMLButtonElement);
    const toneButton = element("tone", HTMLButtonElement);
    const progress = element("progress", HTMLProgressElement);
    const status = element("status", HTMLSpanElement);
    const midiButton = element("midi", HTMLButtonElement);
    const midiState = element("midiState", HTMLSpanElement);
    const fastSynth = element("fastSynth", HTMLInputElement);
    const devices = element("devices", HTMLDivElement);
    const lastMessage = element("lastMessage", HTMLDivElement);
    const dots = [0, 1, 2, 3].map((port) =>
        element(`dot${port}`, HTMLDivElement)
    );
    const counts = [0, 1, 2, 3].map((port) =>
        element(`count${port}`, HTMLDivElement)
    );

    let context: AudioContext | undefined;
    let node: AudioWorkletNode | undefined;
    let isLive = false;
    // MIDI input id -> port (0–3) or -1 for off.
    const routing = new Map<string, number>();
    const totals = [0, 0, 0, 0];

    const setStatus = (text: string) => {
        status.textContent = text;
    };

    const refreshStart = (romFiles: Map<string, File>) => {
        const haveRequired = ROM_SLOTS.every(
            (slot) => !slot.isRequired || romFiles.has(slot.fileName)
        );
        startButton.disabled = !haveRequired || node !== undefined;
        panicButton.disabled = !isLive;
        midiButton.disabled = !isLive;
        toneButton.disabled = context === undefined;
        // Engine choice applies at boot; lock it once the node exists.
        fastSynth.disabled = node !== undefined;
    };

    const card = await setupRomCard(
        element("roms", HTMLInputElement),
        element("romFolder", HTMLSpanElement),
        element("romList", HTMLUListElement),
        element("forgetRoms", HTMLButtonElement),
        refreshStart
    );
    refreshStart(card.files);

    const post = (message: MainToWorklet, transfer?: Transferable[]) => {
        if (node === undefined) return;
        node.port.postMessage(message, transfer ?? []);
    };

    startButton.addEventListener("click", () => {
        void boot().catch((error: unknown) => {
            setStatus(
                `Boot failed: ${error instanceof Error ? error.message : String(error)}`
            );
        });
    });

    async function boot(): Promise<void> {
        if (node !== undefined) return;
        setStatus("Creating audio…");
        console.info(`[live] ${BUILD_TAG} creating AudioContext`);
        // Open the device at its native rate (often 48000).
        // Forcing 44100 risks failing device open.
        // Some hardware has no 44100 path.
        // The synth renders at 44100 internally.
        // Device rate differences are resampled away.
        const audio = new AudioContext({ latencyHint: "interactive" });
        context = audio;
        // Later interruptions (device loss, policy mute) surface here.
        // Without this watcher the page keeps showing Live while silent.
        audio.addEventListener("statechange", () => {
            console.info(`[live] audio state: ${audio.state}`);
            if (audio.state !== "running")
                setStatus(
                    `Audio ${audio.state}: output may be muted or blocked.`
                );
        });
        console.info("[live] adding worklet module");
        await audio.audioWorklet.addModule("smu-processor.js");
        console.info("[live] worklet module added");
        const worklet = new AudioWorkletNode(audio, "smu-synth", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });
        // Without this the node never processes and boot never starts.
        worklet.connect(audio.destination);
        console.info("[live] node created and connected to destination");
        node = worklet;
        refreshStart(card.files);
        worklet.port.addEventListener(
            "message",
            (event: MessageEvent): void => {
                void handleWorklet(event.data);
            }
        );
        worklet.port.start();
        const roms: RomImage[] = [];
        const transfer: Transferable[] = [];
        for (const slot of ROM_SLOTS) {
            const file = card.files.get(slot.fileName);
            if (file === undefined) {
                if (slot.isRequired)
                    throw new Error(`missing ${slot.fileName}`);
                continue;
            }
            console.info(
                `[live] reading ${slot.fileName} (${file.size} bytes)`
            );
            const data = await file.arrayBuffer();
            roms.push({ kind: slot.kind, data });
            transfer.push(data);
        }
        const megabytes =
            roms.reduce((sum, rom) => sum + rom.data.byteLength, 0) / 1_048_576;
        setStatus("Loading ROMs…");
        console.info(
            `[live] posting init with ${roms.length} roms (${megabytes.toFixed(1)} MB), fastSynth=${fastSynth.checked ? "on" : "off"}`
        );
        post(
            {
                type: "init",
                roms,
                nativeEngine: fastSynth.checked,
                nativeFxFull: fastSynth.checked,
                // USB ports, like the desktop default.
                // WebMIDI ports C-D sound instead of going quiet on DIN.
                usbHost: true
            },
            transfer
        );
        await audio.resume();
        console.info(`[live] context resumed, state=${audio.state}`);
    }

    function handleWorklet(data: unknown): void {
        if (typeof data !== "object" || data === null) return;
        const message = data as WorkletToMain;
        console.info(`[live] worklet message: ${message.type}`);
        switch (message.type) {
            case "boot": {
                progress.value = message.fraction;
                setStatus(message.text);
                break;
            }
            case "live": {
                progress.value = 1;
                isLive = true;
                refreshStart(card.files);
                setStatus(
                    `Live at ${context?.sampleRate ?? 44_100} Hz (${fastSynth.checked ? "fast synth" : "exact"}). Play MIDI.`
                );
                break;
            }
            case "error": {
                setStatus(`Synth error: ${message.message}`);
                break;
            }
        }
    }

    panicButton.addEventListener("click", () => {
        post({ type: "panic" });
        setStatus("Panic: all notes off on all ports.");
    });

    // Test tone bypasses the worklet entirely.
    // Audible tone plus silent synth points at the worklet path.
    // Silent tone points downstream: device, mute, or policy.
    toneButton.addEventListener("click", () => {
        if (context === undefined) return;
        const osc = context.createOscillator();
        const gain = context.createGain();
        gain.gain.value = 0.2;
        osc.frequency.value = 440;
        osc.connect(gain);
        gain.connect(context.destination);
        osc.start();
        osc.stop(context.currentTime + 0.5);
    });

    midiButton.addEventListener("click", () => {
        void enableMidi().catch((error: unknown) => {
            midiState.textContent = `MIDI failed: ${error instanceof Error ? error.message : String(error)}`;
        });
    });

    async function enableMidi(): Promise<void> {
        if (!("requestMIDIAccess" in navigator)) {
            throw new Error("this browser has no WebMIDI");
        }
        const access = await navigator.requestMIDIAccess({ sysex: true });
        midiState.textContent = "access granted";
        const refresh = () => refreshDevices(access);
        access.addEventListener("statechange", refresh);
        refresh();
    }

    function refreshDevices(access: MIDIAccess): void {
        devices.replaceChildren();
        let position = 0;
        let seen = 0;
        for (const input of access.inputs.values()) {
            seen++;
            if (!routing.has(input.id)) {
                routing.set(input.id, position < 4 ? position : -1);
            }
            const row = document.createElement("div");
            row.className = "device";
            const name = document.createElement("span");
            name.className = "name";
            name.textContent = input.name ?? input.id;
            name.title = input.name ?? input.id;
            const select = document.createElement("select");
            select.setAttribute(
                "aria-label",
                `Port for ${input.name ?? input.id}`
            );
            const off = document.createElement("option");
            off.value = "-1";
            off.textContent = "Off";
            select.append(off);
            for (const [port, portName] of portNames.entries()) {
                const option = document.createElement("option");
                option.value = String(port);
                option.textContent = `Port ${portName}`;
                select.append(option);
            }
            select.value = String(routing.get(input.id) ?? -1);
            select.addEventListener("change", () => {
                routing.set(input.id, Number(select.value));
            });
            input.onmidimessage = (event: MIDIMessageEvent): void => {
                onMidi(input.id, event.data);
            };
            row.append(name, select);
            devices.append(row);
            position++;
        }
        if (seen === 0) {
            midiState.textContent = "access granted, no inputs found";
        }
    }

    function onMidi(inputId: string, data: Uint8Array | null): void {
        if (data === null || data.length === 0) return;
        const port = routing.get(inputId) ?? -1;
        if (port < 0 || port > 3) return;
        post({ type: "midi", port, bytes: [...data] });
        totals[port] = (totals[port] ?? 0) + 1;
        const dot = dots[port];
        const count = counts[port];
        if (dot !== undefined) {
            dot.classList.add("lit");
            setTimeout(() => dot.classList.remove("lit"), 90);
        }
        if (count !== undefined) count.textContent = String(totals[port]);
        lastMessage.textContent =
            `Port ${portNames[port]}: ` +
            [...data]
                .map((byte) => byte.toString(16).padStart(2, "0"))
                .join(" ");
    }

    setStatus(
        card.files.size > 0
            ? "ROMs stored locally. Boot the synth when ready."
            : "Pick a ROM folder, then Boot synth."
    );
}
