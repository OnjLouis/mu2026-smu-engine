// 16-bit stereo WAV encoder shared by the Node driver and the page.
export function encodeWav(sampleRate: number, data: Int16Array): Uint8Array {
    const out = new Uint8Array(44 + data.length * 2);
    const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
    const ascii = (offset: number, text: string) => {
        for (let index = 0; index < text.length; index++) {
            view.setUint8(offset + index, text.codePointAt(index) ?? 0);
        }
    };
    ascii(0, "RIFF");
    view.setUint32(4, 36 + data.length * 2, true);
    ascii(8, "WAVE");
    ascii(12, "fmt ");
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true); // PCM
    view.setUint16(22, 2, true); // Stereo
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 4, true); // Byte rate
    view.setUint16(32, 4, true); // Block align
    view.setUint16(34, 16, true); // Bits per sample
    ascii(36, "data");
    view.setUint32(40, data.length * 2, true);
    out.set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength), 44);
    return out;
}
