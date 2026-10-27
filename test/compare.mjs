// Runs BREPcode's own NVIDIA kernel test (test/nvidia.js) twice, on the
// native addon and on this wasm build, and prints every line that differs.
//   node test/compare.mjs [--full]
import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { BREPCODE } from "./brepcode.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const test = path.join(BREPCODE, "test", "nvidia.js");
const run = (pre) => {
  const t0 = Date.now();
  const r = spawnSync(process.execPath, [...pre, test], { cwd: BREPCODE, encoding: "utf8", env: { ...process.env, BREPCODE_REQUIRE_NVIDIA: "1" } });
  return { out: (r.stdout + r.stderr).split(/\r?\n/).filter((l) => l.trim()), code: r.status, s: (Date.now() - t0) / 1000 };
};
const native = run([]);
const wasm = run(["--import", path.join(here, "use-wasm.mjs")]);
const count = (lines, w) => lines.filter((l) => l.trim().startsWith(w)).length;
for (const [name, r] of [["native", native], ["wasm", wasm]]) {
  console.log(`${name.padEnd(6)}  ${count(r.out, "PASS")} pass, ${count(r.out, "FAIL")} fail, exit ${r.code}, ${r.s.toFixed(1)} s`);
}
const n = Math.max(native.out.length, wasm.out.length);
let diffs = 0;
for (let i = 0; i < n; i++) {
  if (native.out[i] === wasm.out[i]) continue;
  diffs++;
  console.log(`\n  native: ${native.out[i] ?? "(none)"}\n  wasm:   ${wasm.out[i] ?? "(none)"}`);
}
console.log(diffs ? `\n${diffs} lines differ` : "\nidentical output");
if (process.argv.includes("--full")) console.log("\n--- wasm output ---\n" + wasm.out.join("\n"));
process.exit(count(wasm.out, "FAIL") || wasm.code ? 1 : 0);
