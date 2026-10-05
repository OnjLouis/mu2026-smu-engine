// SMF reader for the playback page, mirroring src/smf.cpp.
// Tempo map, port prefixes, track names and device names included.
// SMPTE division stays unsupported, like on the C++ side.
import type { SongEvent } from "./protocol.ts";

export interface Song {
    events: SongEvent[];
    length: number;
}

// Map a file port to a synth port, mirroring smf::mu_port.
// USB keeps four discrete ports; DIN folds higher ports down.
export function muPort(
    port: number,
    shouldFold: boolean,
    isUsb = false
): number {
    const count = isUsb ? 4 : 2;
    if (port < count) return port;
    return shouldFold ? port % count : -1;
}

// Resolve render-style F5 overrides into explicit ports.
// F5 nn sends following events to port nn (1-based), like render.cpp.
// Dropped ports vanish from the output.
export function resolvePorts(
    events: SongEvent[],
    shouldFold: boolean,
    isUsb = false
): SongEvent[] {
    const out: SongEvent[] = [];
    let override = -1;
    for (const event of events) {
        if (event.bytes.length === 2 && event.bytes[0] === 0xf5) {
            override = Math.min(Math.max(event.bytes[1], 1) - 1, 3);
            continue;
        }
        const to =
            override >= 0 ? override : muPort(event.port, shouldFold, isUsb);
        if (to < 0) continue;
        out.push({ time: event.time, bytes: event.bytes, port: to });
    }
    return out;
}

// Read a port from a track name (issue #63): PartA, Part A, A01, A1,
// A-1, A 16, A01-name. Case-insensitive. Returns -1 when unreadable.
// Bare A or names like B3 Organ stay music, not routing.
export function portFromTrackName(raw: string): number {
    let s = raw.replaceAll("\0", "").toLowerCase();
    while (s.endsWith(" ") || s.endsWith("\t")) s = s.slice(0, -1);
    let index = 0;
    while (s[index] === " ") index++;
    const skipSeparators = (): void => {
        while ([" ", "-", "_"].includes(s[index] ?? "")) index++;
    };
    let isPart = false;
    if (s.startsWith("part", index)) {
        isPart = true;
        index += 4;
        skipSeparators();
    }
    const port = index >= s.length ? -1 : "abcd".indexOf(s[index] ?? "");
    if (port === -1) return -1;
    index++;
    skipSeparators();
    if (index >= s.length) return isPart ? port : -1;
    let n = 0;
    let digits = 0;
    while (
        index < s.length &&
        (s[index] ?? "") >= "0" &&
        (s[index] ?? "") <= "9" &&
        digits < 3
    ) {
        n = n * 10 + Number(s[index]);
        index++;
        digits++;
    }
    if (digits === 0 || n < 1 || n > 16) return -1;
    if (index >= s.length) return port;
    // Only a two-digit number plus separator takes a suffix, so B3
    // Organ stays unread.
    if (digits === 2 && ["-", " ", "_", ":"].includes(s[index] ?? ""))
        return port;
    return -1;
}

function be16(data: Uint8Array, p: number): number {
    return data[p] * 256 + data[p + 1];
}

function be32(data: Uint8Array, p: number): number {
    return (
        ((data[p] * 256 + data[p + 1]) * 256 + data[p + 2]) * 256 + data[p + 3]
    );
}

function readVlq(
    data: Uint8Array,
    pos: number,
    end: number
): { value: number; next: number } {
    let value = 0;
    let p = pos;
    for (;;) {
        if (p >= end) break;
        const byte = data[p];
        p++;
        value = (value << 7) | (byte & 0x7f);
        if ((byte & 0x80) === 0) break;
    }
    return { value, next: p };
}

const decoder = new TextDecoder("windows-1252");

interface Raw {
    tick: number;
    bytes?: number[];
    tempo?: number;
    port: number;
}

interface PortState {
    port: number;
    hasExplicit: boolean;
}

// One meta event at p (after the FF): tempo, port select, or naming.
// Returns where parsing continues plus the updated port state.
function parseMeta(
    data: Uint8Array,
    p: number,
    end: number,
    tick: number,
    raw: Raw[],
    state: PortState
): { next: number; truncated: boolean } {
    if (p + 1 > end) return { next: p, truncated: true };
    const type = data[p];
    const field = readVlq(data, p + 1, end);
    const fieldLength = field.value;
    const body = field.next;
    if (body + fieldLength > end) return { next: p, truncated: true };
    if (type === 0x51 && fieldLength === 3) {
        raw.push({
            tick,
            tempo:
                data[body] * 65_536 + data[body + 1] * 256 + data[body + 2],
            port: 0
        });
    } else if (type === 0x21 && fieldLength === 1) {
        state.port = data[body];
        state.hasExplicit = true;
    } else if (
        type === 0x7f &&
        fieldLength === 4 &&
        data[body] === 0x43 &&
        data[body + 1] === 0x00 &&
        data[body + 2] === 0x01
    ) {
        // Yamaha sequencer port meta: FF 7F 04 43 00 01 pp.
        state.port = data[body + 3];
        state.hasExplicit = true;
    } else if (
        type === 0x03 &&
        !state.hasExplicit &&
        fieldLength >= 1 &&
        fieldLength <= 64
    ) {
        const name = decoder.decode(data.subarray(body, body + fieldLength));
        const named = portFromTrackName(name);
        if (named !== -1) state.port = named;
    } else if (type === 0x09 && fieldLength >= 1 && fieldLength <= 32) {
        // Device-name routing: A-D or Port 1-4.
        let name = decoder.decode(data.subarray(body, body + fieldLength));
        while (name.endsWith(" ") || name.endsWith("\0"))
            name = name.slice(0, -1);
        name = name.toLowerCase();
        if (name.length === 1) {
            const named = "abcd".indexOf(name);
            if (named !== -1) {
                state.port = named;
                state.hasExplicit = true;
            }
        } else if (
            name.length === 6 &&
            name.startsWith("port ") &&
            (name[5] ?? "") >= "1" &&
            (name[5] ?? "") <= "4"
        ) {
            state.port = Number(name[5]) - 1;
            state.hasExplicit = true;
        }
    }
    return { next: body + fieldLength, truncated: false };
}

// One system-exclusive event at p (status byte included).
function parseSysex(
    data: Uint8Array,
    p: number,
    end: number,
    tick: number,
    raw: Raw[],
    port: number,
    status: number
): { next: number; truncated: boolean } {
    const field = readVlq(data, p, end);
    const fieldLength = field.value;
    const body = field.next;
    if (body + fieldLength > end) return { next: p, truncated: true };
    const bytes =
        status === 0xf0
            ? [0xf0, ...data.subarray(body, body + fieldLength)]
            : [...data.subarray(body, body + fieldLength)];
    raw.push({ tick, bytes, port });
    return { next: body + fieldLength, truncated: false };
}

// One channel message at p (status byte already consumed).
// Data length mirrors src/smf.cpp: one byte for C0/D0, two otherwise
// (so F5 nn reads as one two-byte event).
function parseChannel(
    data: Uint8Array,
    p: number,
    end: number,
    tick: number,
    raw: Raw[],
    port: number,
    status: number
): { next: number; truncated: boolean } {
    const high = status & 0xf0;
    const count = high === 0xc0 || high === 0xd0 ? 1 : 2;
    if (p + count > end) return { next: p, truncated: true };
    const bytes = [status];
    for (let k = 0; k < count && p < end; k++) {
        bytes.push(data[p]);
        p++;
    }
    raw.push({ tick, bytes, port });
    return { next: p, truncated: false };
}

// One track of (tick, bytes) records with its own port state.
function parseTrack(
    data: Uint8Array,
    start: number,
    end: number,
    raw: Raw[]
): void {
    const state: PortState = { port: 0, hasExplicit: false };
    let tick = 0;
    let running = 0;
    let p = start;
    while (p < end) {
        const delta = readVlq(data, p, end);
        p = delta.next;
        if (p >= end) return;
        tick += delta.value;

        let status = data[p];
        if (status < 0x80) {
            if (running === 0) throw new Error("stray data byte");
            status = running;
        } else {
            p++;
        }

        if (status === 0xff) {
            const meta = parseMeta(data, p, end, tick, raw, state);
            if (meta.truncated) return;
            p = meta.next;
        } else if (status === 0xf0 || status === 0xf7) {
            const sysex = parseSysex(data, p, end, tick, raw, state.port, status);
            if (sysex.truncated) return;
            p = sysex.next;
        } else {
            running = status;
            const channel = parseChannel(
                data,
                p,
                end,
                tick,
                raw,
                state.port,
                status
            );
            if (channel.truncated) return;
            p = channel.next;
        }
    }
}

// Format 0/1, following tempo changes. Throws on non-SMF input.
export function parseSmf(data: Uint8Array): Song {
    if (
        data.length < 14 ||
        data[0] !== 0x4d ||
        data[1] !== 0x54 ||
        data[2] !== 0x68 ||
        data[3] !== 0x64
    )
        throw new Error("not a MIDI file (no MThd)");
    const headerLength = be32(data, 4);
    const tracks = be16(data, 10);
    const division = be16(data, 12);
    if (division >= 32_768)
        throw new Error("SMPTE-division MIDI is unsupported");
    if (division === 0) throw new Error("invalid division 0");

    const raw: Raw[] = [];
    let pos = 8 + headerLength;
    for (let t = 0; t < tracks && pos + 8 <= data.length; t++) {
        if (
            data[pos] !== 0x4d ||
            data[pos + 1] !== 0x54 ||
            data[pos + 2] !== 0x72 ||
            data[pos + 3] !== 0x6b
        )
            break;
        const trackLength = be32(data, pos + 4);
        const start = pos + 8;
        const end = Math.min(start + trackLength, data.length);
        pos = start + trackLength;
        parseTrack(data, start, end, raw);
    }

    raw.sort((a, b) => a.tick - b.tick);

    const events: SongEvent[] = [];
    let sec = 0;
    let last = 0;
    let usPerBeat = 500_000;
    for (const entry of raw) {
        sec += ((entry.tick - last) * usPerBeat) / (division * 1_000_000);
        last = entry.tick;
        if (entry.tempo !== undefined) {
            usPerBeat = entry.tempo;
            continue;
        }
        if (entry.bytes !== undefined)
            events.push({ time: sec, bytes: entry.bytes, port: entry.port });
    }
    return {
        events,
        length: events.length > 0 ? (events[events.length - 1]?.time ?? 0) : 0
    };
}
