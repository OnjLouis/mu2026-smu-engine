// Shared helpers for the emcc build scripts.
// Core sources, emcc discovery and incremental compilation live here.
// Build.ts and build-standalone.ts stay thin on top of it.
import child_process from "node:child_process";
import fs from "node:fs";
import { homedir } from "node:os";
import path from "node:path";

export const SOURCES = [
    "src/compat/compat.cpp",
    "src/sampling.cpp",
    "src/smartmedia.cpp",
    "src/card_fs.cpp",
    "src/m2a.cpp",
    "src/mame/sound/swp30.cpp",
    "src/mame/sound/swp30_jit.cpp",
    "src/mame/video/hd44780.cpp",
    "src/mame/machine/sci4.cpp",
    "src/mame/cpu/sh.cpp",
    "src/mame/cpu/sh2.cpp",
    "src/mame/cpu/sh2_jit.cpp",
    "src/compat/a64asm.cpp",
    "src/mame/cpu/sh7042.cpp",
    "src/mame/cpu/sh_adc.cpp",
    "src/mame/cpu/sh_bsc.cpp",
    "src/mame/cpu/sh_cmt.cpp",
    "src/mame/cpu/sh_dmac.cpp",
    "src/mame/cpu/sh_intc.cpp",
    "src/mame/cpu/sh_mtu.cpp",
    "src/mame/cpu/sh_port.cpp",
    "src/mame/cpu/sh_sci.cpp",
    "src/mu2000.cpp",
    "src/smf.cpp",
    "src/wasm/wasm_render.cpp"
];

// eslint-disable-next-line unicorn/consistent-boolean-name
async function fileExists(path: string) {
    try {
        await fs.promises.access(path);
        return true;
    } catch {
        return false;
    }
}

export async function findEmcc(repoRoot: string) {
    const siblingEmsdk = path.resolve(repoRoot, "..", "emsdk");
    const emccVariants = ["emcc", "emcc.bat", "emcc.cmd", "emcc.exe"];

    const candidates = [
        process.env.EMCC,
        process.env.EMSDK === undefined
            ? undefined
            : path.resolve(process.env.EMSDK, "upstream/emscripten/emcc"),
        process.env.EMSDK === undefined
            ? undefined
            : path.resolve(process.env.EMSDK, "upstream/emscripten/emcc.bat"),
        process.env.EMSDK === undefined
            ? undefined
            : path.resolve(process.env.EMSDK, "upstream/emscripten/emcc.cmd"),
        path.resolve(siblingEmsdk, "upstream/emscripten/emcc"),
        path.resolve(siblingEmsdk, "upstream/emscripten/emcc.bat"),
        path.resolve(siblingEmsdk, "upstream/emscripten/emcc.cmd"),
        "/usr/lib/emscripten/emcc",
        path.resolve(homedir(), "emsdk/upstream/emscripten/emcc"),
        path.resolve(homedir(), "emsdk/upstream/emscripten/emcc.bat"),
        ...emccVariants
    ];

    for (const candidate of candidates) {
        if (candidate === undefined) continue;
        if (["emcc", "emcc.bat", "emcc.cmd", "emcc.exe"].includes(candidate)) {
            return candidate;
        }
        if (await fileExists(candidate)) {
            return candidate;
        }
    }

    throw new Error(
        `Could not find emcc. Set EMSDK or EMCC to your Emscripten install (Windows examples: EMSDK=C:/path/to/emsdk, EMCC=C:/path/to/emsdk/upstream/emscripten/emcc.bat).`
    );
}

export function emxxFor(emcc: string) {
    // Em++ sits next to emcc in every layout: same file, cc -> ++.
    // (emcc, emcc.bat/.cmd/.exe, /usr/lib/emscripten/emcc, .../emcc.py)
    const parsed = path.parse(emcc);
    if (!parsed.name.endsWith("emcc")) {
        throw new Error(`cannot derive em++ from EMCC=${emcc}`);
    }
    return path.join(
        parsed.dir,
        `${parsed.name.slice(0, -4)}em++${parsed.ext}`
    );
}

function isStale(source: string, object: string) {
    if (!fs.existsSync(object)) return true;
    return fs.statSync(source).mtimeMs > fs.statSync(object).mtimeMs;
}

// Compile every source with the given flags, skipping fresh objects.
// Returns the object paths in source order.
export function compileObjects(
    emcc: string,
    flags: string[],
    objectDirectory: string,
    repoRoot: string
): string[] {
    fs.mkdirSync(objectDirectory, { recursive: true });
    const objects: string[] = [];
    for (const source of SOURCES) {
        const absolute = path.resolve(repoRoot, source);
        const object = path.resolve(
            objectDirectory,
            `${source.replaceAll("/", "_")}.o`
        );
        objects.push(object);
        if (!isStale(absolute, object)) continue;
        console.info(`CC: ${source}`);
        child_process.execFileSync(
            emcc,
            [...flags, "-c", absolute, "-o", object],
            {
                cwd: repoRoot,
                stdio: "inherit"
            }
        );
    }
    return objects;
}
