// One kernel, in this process; prints one JSON line (test/bench.mjs drives it).
//   node [--import ./test/use-wasm.mjs] test/bench-one.mjs native|wasm|rust
import { readFileSync } from "node:fs";
import path from "node:path";
import { BREPCODE, at } from "./brepcode.mjs";

const kernel = process.argv[2];
const dsl = await import(at("src/dsl.js"));
const { build } = await import(at("index.js"));
const loaders = await import(at("src/engine-node.js"));

const names = Object.keys(dsl).filter((k) => typeof dsl[k] === "function" && k !== "compile");
const shape = (src) => new Function(...names, `"use strict";\n${src}`)(...names.map((n) => dsl[n]));

export const MODELS = {
  "twisted square": "return linearExtrude({ h: 40, twist: 90 }, polygon([[-10, -10], [10, -10], [10, 10], [-10, 10]]));",
  "chamfered union": `return chamfer({ d: 1, edges: "top" }, union(cube([20, 20, 10]), translate([10, 10, 0], cube([20, 20, 10]))));`,
  "plate, 12 x 12 holes": `let p = cube([130, 130, 4]);
    for (let i = 0; i < 12; i++) for (let j = 0; j < 12; j++) p = difference(p, translate([10 + 10 * i, 10 + 10 * j, -1], cylinder({ r: 3, h: 6 })));
    return p;`,
  "mini Snapmaker U1": readFileSync(path.join(BREPCODE, "examples", "snapmaker-u1.js"), "utf8"),
};

function volumeOf(res) {
  let vol = 0, tris = 0;
  for (const s of res.solids) {
    const v = [...s.toSTL(s.name).matchAll(/vertex\s+(\S+)\s+(\S+)\s+(\S+)/g)].map((m) => [+m[1], +m[2], +m[3]]);
    tris += v.length / 3;
    for (let i = 0; i + 2 < v.length; i += 3) {
      const [a, b, c] = [v[i], v[i + 1], v[i + 2]];
      vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
    }
  }
  return { vol, tris };
}

const out = { kernel, models: {} };
let t0 = performance.now();
if (kernel === "rust") { await loaders.loadRustKernel(); out.loadMs = performance.now() - t0; }
else { loaders.loadNvidiaKernel(); out.loadMs = kernel === "wasm" ? globalThis.__smlibWasm.loadMs : performance.now() - t0; }
// nvidia: SMLib first, then the Rust kernel for what it refuses (the alpha's
// default); split: the Rust kernel (and manifold for mesh steps), no fallback
const engine = process.env.ENGINE || (kernel === "rust" ? "split" : "nvidia");
const nk = globalThis.__smlibWasm?.nk;
const runs = +(process.env.RUNS || 3);
for (const [label, src] of Object.entries(MODELS)) {
  const r = { ms: [] };
  try {
    for (let k = 0; k <= runs; k++) {
      t0 = performance.now();
      const res = await build(shape(src), { engine });
      r.ms.push(performance.now() - t0);
      if (k === 0) Object.assign(r, volumeOf(res), { built: res.kernel || res.engine, engine: res.engine, fallback: res.fallback || undefined });
    }
  } catch (e) { r.error = String(e.message || e).slice(0, 200); }
  r.first = r.ms[0];
  r.median = r.ms.slice(1).sort((a, b) => a - b)[Math.floor((r.ms.length - 1) / 2)];
  if (nk) r.heapMB = nk.heapBytes() / 1048576;
  out.models[label] = r;
}
out.rssMB = process.memoryUsage().rss / 1048576;
console.log(JSON.stringify(out));
