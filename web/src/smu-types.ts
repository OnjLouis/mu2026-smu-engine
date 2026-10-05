// Types for the emcc-generated out/smu_render.mjs module (see src/wasm/wasm_render.cpp).
export interface SmuModule {
    _malloc(size: number): number;
    _free(ptr: number): void;
    readonly HEAPU8: Uint8Array;
    _smu_sample_rate(): number;
    _smu_init(usbHost: number): number;
    _smu_set_usb_host(on: number): void;
    _smu_set_rom(kind: number, ptr: number, length: number): number;
    _smu_reset(): number;
    _smu_boot(maxSamples: number): number;
    _smu_midi_ready(): number;
    _smu_run_blank(nframes: number): number;
    _smu_load_midi(ptr: number, length: number): number;
    _smu_song_length(): number;
    _smu_midi_in(port: number, ptr: number, length: number): number;
    _smu_set_native_engine(mode: number): void;
    _smu_set_native_fx(mode: number): void;
    _smu_native_firmware_share(): number;
    _smu_render_frames(outPtr: number, nframes: number): number;
    _smu_song_done(): number;
    _smu_scheduled_events(): bigint;
    _smu_dropped(): bigint;
    _smu_error_copy(outPtr: number, cap: number): number;
}

export type SmuFactory = (
    overrides?: Record<string, unknown>
) => Promise<SmuModule>;
