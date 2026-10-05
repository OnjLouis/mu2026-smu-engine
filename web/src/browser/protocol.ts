// Message protocol between the pages (main thread) and the synth
// AudioWorklet processor. Types only; imported with `import type`.
export interface RomImage {
    kind: number;
    data: ArrayBuffer;
}

// One scheduled song message with its time and explicit synth port.
// Parsed on the page (see smf.ts), fed by the worklet sample clock.
export interface SongEvent {
    time: number;
    bytes: number[];
    port: number;
}

export type MainToWorklet =
    | {
          type: "init";
          roms: RomImage[];
          nativeEngine?: boolean;
          nativeFxFull?: boolean;
          usbHost?: boolean;
      }
    | { type: "midi"; port: number; bytes: number[] }
    | { type: "play"; events: SongEvent[]; length: number }
    | { type: "stop" }
    | { type: "panic" };

export type WorkletToMain =
    | { type: "boot"; fraction: number; text: string }
    | { type: "live" }
    | { type: "song"; position: number; length: number; done: boolean }
    | { type: "error"; message: string };
