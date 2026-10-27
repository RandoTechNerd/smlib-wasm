// One kernel, in this process; prints one JSON line (test/bench.mjs drives it).
//   node [--import ./test/use-wasm.mjs] test/bench-one.mjs native|wasm|rust
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { BREPCODE, at } from "./brepcode.mjs";

const kernel = process.argv[2];
const { build } = await import(at("index.js"));
const loaders = await import(at("src/engine-node.js"));

// editor code run as the editor runs it (a const may shadow a DSL word), with
// text() as exact glyph outlines from the editor's own font, as in the app
const { runEditorCode } = await import(at("src/runcode.js"));
const dsl = await import(at("src/dsl.js"));
const { exactText } = await import(at("src/textexact.js"));
const ot = createRequire(import.meta.url)(path.join(BREPCODE, "node_modules", "opentype.js", "dist", "opentype.js"));
const fontBuf = readFileSync(path.join(BREPCODE, "viewer", "fonts", "Roboto-Bold.woff"));
const font = ot.parse(fontBuf.buffer.slice(fontBuf.byteOffset, fontBuf.byteOffset + fontBuf.byteLength));
const vocab = { ...dsl, text: (o) => exactText(font, String(o.text), o) };
const shape = (src) => runEditorCode(src, vocab).shape;

export const MODELS = {
  "twisted square": "return linearExtrude({ h: 40, twist: 90 }, polygon([[-10, -10], [10, -10], [10, 10], [-10, 10]]));",
  // BREPcode's test/nvidia.js: a union, then a chamfer on an edge picked by name
  "chamfered union": `return chamfer({ d: 1, edges: ["R4:L100_END|R4:L100_START[0]"] }, union(cube(20), translate([30, 0, 0], revolve({ angle: 270 }, polygon([[0, 0], [8, 0], [8, 10], [0, 10]])))));`,
  "chamfered box with a boss": `return chamfer({ d: 1, edges: "top" }, union(cube([20, 20, 10]), translate([10, 10, 0], cylinder({ r: 4, h: 16 }))));`,
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
process.exit(0); // BREPcode leaves a handle open; without this the run never ends
