// Minimal AudioWorkletGlobalScope surface for processor.ts.
// The default libs do not declare these, so they live here instead.
declare const sampleRate: number;
declare function registerProcessor(
    name: string,
    processor: new (options?: unknown) => AudioWorkletProcessor
): void;
declare class AudioWorkletProcessor {
    public readonly port: MessagePort;
    public process(
        inputs: Float32Array[][],
        outputs: Float32Array[][],
        parameters: Record<string, Float32Array>
    ): boolean;
}
