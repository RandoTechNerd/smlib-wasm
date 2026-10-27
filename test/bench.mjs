// Size, load time and build time: the wasm build against the native addon
// and against BREPcode's Rust wasm kernel.
//   node test/bench.mjs [--json]      (RUNS=3 timed builds after a first one)
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { brotliCompressSync, gzipSync, constants } from "node:zlib";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { BREPCODE } from "./brepcode.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const MB = (n) => `${(n / 1048576).toFixed(2)} MB`;
const sizes = (file) => {
  const b = readFileSync(file);
  return { raw: b.length, gzip: gzipSync(b, { level: 9 }).length, brotli: brotliCompressSync(b, { params: { [constants.BROTLI_PARAM_QUALITY]: 11 } }).length };
};
const files = {
  "smlib.wasm (this)": path.join(here, "..", "dist", "smlib.wasm"),
  "smlib.mjs (this, glue)": path.join(here, "..", "dist", "smlib.mjs"),
  "brep_kernel_bg.wasm (Rust)": path.join(BREPCODE, "vendor", "brep-kernel-rust", "brep_kernel_bg.wasm"),
  "brepcode_nvidia.node (native)": path.join(BREPCODE, "native", "nvidia-kernel", "build", "Release", "brepcode_nvidia.node"),
};
console.log("\nsize                            raw        gzip -9    brotli 11");
for (const [k, f] of Object.entries(files)) {
  try { const s = sizes(f); console.log(`${k.padEnd(30)}  ${MB(s.raw).padStart(9)}  ${MB(s.gzip).padStart(9)}  ${MB(s.brotli).padStart(9)}`); }
  catch { console.log(`${k.padEnd(30)}  (missing)`); }
}

const one = (kernel) => {
  const pre = kernel === "wasm" ? ["--import", path.join(here, "use-wasm.mjs")] : [];
  const r = spawnSync(process.execPath, [...pre, path.join(here, "bench-one.mjs"), kernel], { cwd: BREPCODE, encoding: "utf8", maxBuffer: 1 << 26 });
  const line = r.stdout.trim().split(/\r?\n/).at(-1);
  try { return JSON.parse(line); } catch { return { kernel, error: (r.stdout + r.stderr).slice(-800) }; }
};
const results = ["native", "wasm", "rust"].map(one);
console.log("\nload: " + results.map((r) => `${r.kernel} ${r.loadMs?.toFixed(0)} ms`).join(", "));
const labels = Object.keys(results.find((r) => r.models)?.models || {});
for (const r of results) if (r.error) console.log(`\n${r.kernel} failed:\n${r.error}`);
for (const label of labels) {
  console.log(`\n${label}`);
  for (const r of results) {
    const m = r.models?.[label];
    if (!m) continue;
    if (m.error) { console.log(`  ${r.kernel.padEnd(7)} error: ${m.error}`); continue; }
    console.log(`  ${r.kernel.padEnd(7)} first ${m.first.toFixed(0).padStart(6)} ms  median ${m.median.toFixed(0).padStart(6)} ms  ` +
      `volume ${m.vol.toFixed(1).padStart(10)}  ${String(m.tris).padStart(6)} tris  by ${m.built}/${m.engine}` +
      `${m.fallback ? ` (fell back: ${String(m.fallback).slice(0, 80)})` : ""}${m.heapMB ? `  heap ${m.heapMB.toFixed(0)} MB` : ""}`);
  }
}
console.log("\nprocess RSS at the end: " + results.map((r) => `${r.kernel} ${r.rssMB?.toFixed(0)} MB`).join(", "));
if (process.argv.includes("--json")) console.log(JSON.stringify(results));
