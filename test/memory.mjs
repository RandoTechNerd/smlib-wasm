// How big the wasm heap gets on heavy models, and whether repeated builds
// leak. wasm32 memory tops out at 4 GB and never shrinks, so the heap's
// high-water mark is the number that matters.
//   node test/memory.mjs            (the wasm kernel, which it loads itself)
//   node test/memory.mjs --native   (the same models on BREPcode's native addon, for times)
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { createRequire } from "node:module";
import { BREPCODE, at } from "./brepcode.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const native = process.argv.includes("--native");
if (!native) await import(pathToFileURL(path.join(here, "use-wasm.mjs")).href);
const { build } = await import(at("index.js"));
const { runEditorCode } = await import(at("src/runcode.js"));
// the raw kernel functions: the wasm object, or the addon itself
const nk = native ? createRequire(import.meta.url)(path.join(BREPCODE, "native", "nvidia-kernel", "build", "Release", "brepcode_nvidia.node")) : globalThis.__smlibWasm.nk;
const MB = () => (native ? `rss ${(process.memoryUsage().rss / 1048576).toFixed(0)} MB` : `${(nk.heapBytes() / 1048576).toFixed(0)} MB`);

const plate = (n) => `let p = cube([${10 * n + 10}, ${10 * n + 10}, 4]);
  for (let i = 0; i < ${n}; i++) for (let j = 0; j < ${n}; j++) p = difference(p, translate([10 + 10 * i, 10 + 10 * j, -1], cylinder({ r: 3, h: 6 })));
  return p;`;
const one = async (label, src, opts = {}) => {
  const t0 = performance.now();
  try {
    const res = await build(runEditorCode(src).shape, { engine: "nvidia-split", ...opts });
    const tris = res.solids.reduce((n, s) => n + (s.toSTL(s.name).match(/facet normal/g) || []).length, 0);
    console.log(`${label.padEnd(44)} ${((performance.now() - t0) / 1000).toFixed(1).padStart(6)} s  ${String(tris).padStart(8)} tris  heap ${MB()}`);
  } catch (e) { console.log(`${label.padEnd(44)} failed: ${String(e.message || e).slice(0, 120)}  heap ${MB()}`); }
};

console.log(`start                                        heap ${MB()}`);
// the same build 50 times: the heap should level off, not climb
for (let k = 1; k <= 50; k++) {
  await build(runEditorCode(plate(4)).shape, { engine: "nvidia-split" });
  if (k === 1 || k === 10 || k === 50) console.log(`plate 4 x 4 holes, build ${String(k).padEnd(28)}        heap ${MB()}, ${nk.count()} solids alive`);
}
await one("plate 12 x 12 holes (144 booleans)", plate(12));
// straight on the kernel (BREPcode's DSL overflows the JS stack on 400
// nested difference() calls): an n x n plate, drawn at `chord`
const rawPlate = (n, chord) => {
  const t0 = performance.now();
  const p = nk.box(10 * n + 10, 10 * n + 10, 4);
  for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) {
    const c = nk.cylinder(3, 6);
    nk.translate(c, 10 + 10 * i, 10 + 10 * j, -1);
    nk.boolean(3, p, c);
  }
  const m = nk.tessellate(p, chord, chord < 0.01 ? 2 : 8);
  nk.free(p);
  console.log(`${`plate ${n} x ${n} on the kernel, ${chord} chord`.padEnd(44)} ${((performance.now() - t0) / 1000).toFixed(1).padStart(6)} s  ${String(m.indices.length / 3).padStart(8)} tris  heap ${MB()}`);
};
rawPlate(12, 0.001);
rawPlate(20, 0.05);
await one("200 spheres unioned", `let s = sphere({ r: 3 });
  for (let i = 1; i < 200; i++) s = union(s, translate([4 * (i % 20), 4 * Math.floor(i / 20), 0], sphere({ r: 3 })));
  return s;`);
await one("a 720 degree twist, 60 profiles", "return linearExtrude({ h: 80, twist: 720 }, polygon([[-10, -10], [10, -10], [10, 10], [-10, 10]]));");
console.log(`end                                          heap ${MB()} (the high-water mark), ${nk.count()} solids alive`);
process.exit(0); // BREPcode leaves a handle open
