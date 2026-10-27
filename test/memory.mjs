// How big the wasm heap gets on heavy models, and whether repeated builds
// leak. wasm32 memory tops out at 4 GB and never shrinks, so the heap's
// high-water mark is the number that matters.
//   node test/memory.mjs   (run from anywhere; loads the wasm kernel itself)
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { at } from "./brepcode.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
await import(pathToFileURL(path.join(here, "use-wasm.mjs")).href);
const { nk } = globalThis.__smlibWasm;
const { build } = await import(at("index.js"));
const { runEditorCode } = await import(at("src/runcode.js"));
const MB = () => `${(nk.heapBytes() / 1048576).toFixed(0)} MB`;

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
{
  // straight on the kernel: the same plate, drawn at a 0.001 mm chord
  const t0 = performance.now();
  const p = nk.box(130, 130, 4);
  for (let i = 0; i < 12; i++) for (let j = 0; j < 12; j++) {
    const c = nk.cylinder(3, 6);
    nk.translate(c, 10 + 10 * i, 10 + 10 * j, -1);
    nk.boolean(3, p, c);
  }
  const m = nk.tessellate(p, 0.001, 2);
  nk.free(p);
  console.log(`${"the same plate, tessellated to a 0.001 chord".padEnd(44)} ${((performance.now() - t0) / 1000).toFixed(1).padStart(6)} s  ${String(m.indices.length / 3).padStart(8)} tris  heap ${MB()}`);
}
await one("plate 20 x 20 holes (400 booleans)", plate(20));
await one("200 spheres unioned", `let s = sphere({ r: 3 });
  for (let i = 1; i < 200; i++) s = union(s, translate([4 * (i % 20), 4 * Math.floor(i / 20), 0], sphere({ r: 3 })));
  return s;`);
await one("a 720 degree twist, 60 profiles", "return linearExtrude({ h: 80, twist: 720 }, polygon([[-10, -10], [10, -10], [10, 10], [-10, 10]]));");
console.log(`end                                          heap ${MB()} (the high-water mark), ${nk.count()} solids alive`);
process.exit(0); // BREPcode leaves a handle open
