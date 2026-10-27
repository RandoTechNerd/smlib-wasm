// The wasm kernel on its own, no BREPcode: every function once.
//   node test/smoke.mjs
import loadSmlib from "../src/api.mjs";

const nk = await loadSmlib();
let fail = 0;
const near = (label, got, want, tol = 1e-3) => {
  const ok = Math.abs(got - want) <= tol * Math.max(1, Math.abs(want));
  if (!ok) fail++;
  console.log(`${ok ? "  PASS" : "  FAIL"}  ${label}: ${got} (want ${want})`);
};
const PI = Math.PI;

console.log(nk.info());
const box = nk.box(10, 20, 30);
near("box volume", nk.volume(box), 6000);
near("box faces", nk.faces(box).length / 6, 6, 0);
near("box edges", nk.edges(box).length / 5, 12, 0);
const cyl = nk.cylinder(3, 40);
nk.translate(cyl, 5, 10, -5);
nk.boolean(3, box, cyl);
near("box minus cylinder", nk.volume(box), 6000 - PI * 9 * 30, 2e-3);
near("count after the boolean", nk.count(), 1, 0);
const c = nk.copy(box);
nk.scale(c, -1, 1, 1);
near("mirror keeps the volume", nk.volume(c), 6000 - PI * 9 * 30, 2e-3);
near("mirror bbox min x", nk.bbox(c)[0], -10, 1e-6);
const tor = nk.torusArc(10, 2, 90);
near("part torus", nk.volume(tor), 2 * PI * PI * 10 * 4 / 4, 5e-3);
const sq = [[0, -5, -5, 0, 5, -5, 0], [0, 5, -5, 0, 5, 5, 0], [0, 5, 5, 0, -5, 5, 0], [0, -5, 5, 0, -5, -5, 0]];
const ex = nk.extrude(sq, 0, 0, 1, 7);
near("extruded square", nk.volume(ex), 700);
const tw = nk.twistExtrude(sq, 20, 90, 1, 9, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
near("twisted square", nk.volume(tw), 2000, 5e-3);
const arcs = [[2, 8, 0, 0, 6, 0, 180], [2, 8, 0, 0, 6, 180, 360]];
const twc = nk.twistExtrude(arcs, 30, 270, 1, 12, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
near("twisted circle (arc angles past 360 normalised)", nk.volume(twc), PI * 36 * 30, 5e-3);
const rv = nk.revolve([[0, 0, 0, 0, 8, 0, 0], [0, 8, 0, 0, 8, 0, 6], [0, 8, 0, 6, 0, 0, 6]], 0, 0, 0, 0, 0, 1, 360);
near("revolved cup", nk.volume(rv), PI * 64 * 6, 5e-3);
const fb = nk.box(40, 30, 10);
const e = nk.edges(fb);
const vertical = [];
for (let i = 0; i < e.length / 5; i++) if (e[5 * i + 4] > 1 && e[5 * i + 4] < 9) vertical.push(i);
const before = nk.copy(fb);
nk.fillet(fb, vertical, 3, 1);
near("rounded box", nk.volume(fb), 12000 - 4 * (9 - PI * 9 / 4) * 10, 2e-3);
const m = nk.matchFaces(fb, [before], 1e-3);
near("fillet keeps 6 faces, adds 4 blends", [...m].filter((v, i) => i % 2 === 0 && v === -1).length, 4, 0);
const t = nk.tessellate(fb, 0.05, 8);
near("tessellate gives a face id per triangle", t.faceIds.length * 3, t.indices.length, 0);
const t2 = nk.tessellate(tw, 0.05, 8, 1);
console.log(`  tessellate with a max edge of 1: ${t2.indices.length / 3} triangles (without: ${nk.tessellate(tw, 0.05, 8).indices.length / 3})`);
let threw = "";
try { nk.boolean(1, 999, 998); } catch (err) { threw = err.message; }
console.log(`${threw ? "  PASS" : "  FAIL"}  a bad handle throws: ${threw}`);
if (!threw) fail++;
console.log(`heap ${(nk.heapBytes() / 1048576).toFixed(0)} MB`);
console.log(fail ? `\n${fail} failed` : "\nall passed");
process.exit(fail ? 1 : 0);
