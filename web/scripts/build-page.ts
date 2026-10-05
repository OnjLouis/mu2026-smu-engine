// Page bundles become minified dist/ files with sourcemaps.
// The wasm binary is embedded as base64 (see build-standalone.ts).
// No separate .wasm fetch is needed in dist/.
// Index.html, style.css and the page sources are copied next to them.
// Public/ holds the page sources.
// Run from web/: npx tsx scripts/build-page.ts (or: npm run build:page)
import esbuild from "esbuild";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const distributionDirectory = path.resolve(web, "dist");

fs.mkdirSync(distributionDirectory, { recursive: true });

for (const page of ["render-app", "live-app", "play-app"]) {
    await esbuild.build({
        entryPoints: [path.resolve(web, "src", "browser", `${page}.ts`)],
        bundle: true,
        minify: true,
        sourcemap: true,
        format: "esm",
        platform: "browser",
        target: "es2022",
        outfile: path.resolve(
            distributionDirectory,
            `${page.replace("-app", "")}.js`
        ),
        logLevel: "info"
    });
}

await esbuild.build({
    entryPoints: [path.resolve(web, "src", "browser", "processor.ts")],
    bundle: true,
    minify: true,
    sourcemap: true,
    format: "esm",
    platform: "browser",
    target: "es2022",
    outfile: path.resolve(distributionDirectory, "smu-processor.js"),
    logLevel: "info"
});

for (const file of [
    "index.html",
    "render.html",
    "live.html",
    "play.html",
    "style.css"
]) {
    fs.copyFileSync(
        path.resolve(web, "public", file),
        path.resolve(distributionDirectory, file)
    );
}
// The hero screenshot lives with the docs; copy it into dist/ so the
// Deployed pages stay self-contained (web/ holds no binary copy).
fs.copyFileSync(
    path.resolve(web, "..", "doc", "mu_screenshot.png"),
    path.resolve(distributionDirectory, "mu_screenshot.png")
);
console.info("page: dist/ with render, live, play, processor bundles");
