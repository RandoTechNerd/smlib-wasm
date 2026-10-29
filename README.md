# smlib-wasm

The SMLib B-rep kernel, based on NVIDIA's open-source
[usd-brep](https://github.com/NVIDIA-Omniverse/usd-brep) (SMLib + SM_API,
v1.0.1, Apache-2.0), compiled to WebAssembly with Emscripten, so BREPcode can
build models on it in the browser as its desktop Alpha does natively.

The JS API is BREPcode's Node addon (`native/nvidia-kernel/src/addon.cc`),
function for function, so BREPcode's `adapter.cjs` runs on it unchanged:

```js
import loadSmlib from "./src/api.mjs";
const nk = await loadSmlib();          // { box, cylinder, boolean, tessellate, ... }
const h = nk.box(10, 20, 30);
nk.volume(h);                          // 6000
```

Not affiliated with or endorsed by NVIDIA.

## Files

| File | What it is |
|---|---|
| `fetch-upstream.mjs` | Fetches SMLib + SM_API (only) at the pinned commit into `upstream/` (or `--from DIR` copies a local checkout) |
| `CMakeLists.txt`, `build.mjs` | The Emscripten build: two static libraries and one ES module, `dist/smlib.mjs` + `dist/smlib.wasm` |
| `src/smlib_wasm.cc` | The embind bridge, a port of BREPcode's `addon.cc` |
| `src/api.mjs` | The JS face: loads the module and gives back the addon's functions |
| `test/smoke.mjs` | Every function once, no BREPcode needed |
| `test/use-wasm.mjs` | A Node `--import` hook that answers BREPcode's `require("brepcode_nvidia.node")` with the wasm kernel |
| `test/compare.mjs` | BREPcode's own `test/nvidia.js`, on the native addon and on wasm, with the differences printed |
| `test/bench.mjs`, `test/bench-one.mjs` | Size, load time and build time: wasm vs the native addon vs BREPcode's Rust wasm kernel |
| `test/memory.mjs` | Heap high-water marks on heavy models, and a leak check |

### What changed from upstream and from BREPcode

- `upstream/` (usd-brep's `source/SMLib` and `source/SM_API`): **not modified**.
  It compiles as it is with em++; the non-Windows, no-`SM_USE_THREADS` code
  paths keep SMLib's thread-local state in plain statics.
- `src/smlib_wasm.cc`: **ported** from BREPcode's `native/nvidia-kernel/src/addon.cc`.
  The Napi types became `emscripten::val`. Each function takes its arguments as
  one JS array, because embind checks argument counts and tessellate's 4th and
  5th arguments are optional. Errors are caught in C++ and returned instead of
  thrown across the wasm boundary. `init`, `lastError` and `heapBytes` were
  added. The kernel calls and their arguments are unchanged, including
  `twistExtrude` (arc angles normalised to [-360, 360]), tessellate's
  `dMaxEdgeLength`, and the boolean that retries the other way round.
- Everything else is new.

## Build

Needs the Emscripten SDK, CMake 3.20+, Ninja and Node 18+.

```bash
git clone https://github.com/emscripten-core/emsdk.git ../.toolchain/emsdk
```
```bash
../.toolchain/emsdk/emsdk install latest
```
```bash
../.toolchain/emsdk/emsdk activate latest
```
```bash
node fetch-upstream.mjs
```
```bash
node build.mjs
```

`build.mjs` finds the SDK at `$EMSDK` or `../.toolchain/emsdk` and sets up its
environment itself. About 2 minutes on 8 cores. Options: `--eh wasm|js`
(exception handling, default `wasm`), `--opt -O2`, `--dist DIR`.

Flags: wasm32, single threaded, C++17, `-O2`, RTTI on, `-fwasm-exceptions`,
`ALLOW_MEMORY_GROWTH` up to 4 GB (32 MB to start), 4 MB stack (Emscripten's
64 KB default is too small for SMLib), `MODULARIZE` + `EXPORT_ES6`, no
filesystem, environments web, worker and node.

## Test

```bash
node test/smoke.mjs
```
```bash
node test/compare.mjs
```
```bash
node test/bench.mjs
```
```bash
node test/memory.mjs
```

The last three use a BREPcode checkout with the native addon built (read
only): `$BREPCODE`, else `../BrepScript-wt/alpha-integration`.

## Results (Windows 11, 8 cores, Node 24, Emscripten latest, 2026-10-27)

### Correctness

BREPcode's `test/nvidia.js` (volumes, face names, bounds, mirrors, part tori,
twists, own-booleans, refusals): **105 of 105 pass on wasm, the same as on
native.** One output line differs: a placed, twisted off-centre circle comes
out at 3377.9 mm³ on wasm and 3377.8 on native (exact: 3392.9; the gap is
tessellation). Every benchmark model gives the same volume and the same
triangle count on wasm and native.

### Size

| File | Raw | gzip -9 | brotli 11 |
|---|---|---|---|
| `smlib.wasm` (wasm exceptions, the default) | 4.37 MB | 1.56 MB | **1.15 MB** |
| `smlib.wasm` (JS exceptions) | 5.88 MB | 1.83 MB | 1.34 MB |
| `smlib.mjs` (glue) | 0.03 MB | 0.01 MB | 0.01 MB |
| BREPcode's Rust kernel, `brep_kernel_bg.wasm` | 13.82 MB | 4.99 MB | 3.49 MB |
| Native addon, `brepcode_nvidia.node` (Windows) | 11.71 MB | 4.93 MB | 3.64 MB |

### Load and build times

Load (compile + instantiate, in Node): native 5 ms, **wasm 18 ms**, Rust 24 ms.

Build through BREPcode's `build()`: first build, then the median of 2 more.
SMLib runs with the `nvidia` engine (SMLib, falling back to Rust); Rust runs
with `split`.

| Model | Native | **Wasm** | Rust |
|---|---|---|---|
| Twisted square (90 degrees) | 188 / 131 ms | **250 / 95 ms** | 117 / 30 ms (mesh lane) |
| Chamfered box with a boss | 72 / 79 ms | **97 / 84 ms** | 202 / 42 ms |
| Chamfered union (`test/nvidia.js`'s revolve-cap chamfer) | 314 / 124 ms (JS fallback) | **318 / 140 ms** (JS fallback) | refused |
| Plate, 12 x 12 holes (144 booleans) | 7.4 / 7.5 s | **8.3 / 8.0 s** | 19.3 / 0.2 s |
| Mini Snapmaker U1 | 27.4 / 26.4 s | **27.6 / 29.5 s** | 28.2 / 31.3 s |
| Mini Snapmaker U1, chamfers left off | 697 / 721 ms | **738 / 809 ms** | 884 / 250 ms |

Wasm runs at 1.0x to 1.3x the native addon's time. With JS exceptions
(`--eh js`) it was twice as slow on the plate (16.8 s); wasm exceptions are
why the default is what it is.

Notes on the table:

- **The Snapmaker's 27 s is not kernel time.** Its two chamfers pick their own
  edges, and BREPcode's `compile()` (`childEdgeNames` in `src/dsl.js`) finds
  them by replaying the history so far on the JS kernel, which takes about
  20 s over the exact "snapmaker" lettering. With those two chamfers taken
  out, it builds in 0.7 to 0.8 s on every kernel. That's a BREPcode fix, not
  one for this kernel.
- The "chamfered union" falls back to the JS kernel on both SMLib builds alike
  (the Rust kernel refuses it), so it times the fallback chain, not SMLib.
- The Rust kernel's repeat builds of the plate take 0.2 s against 19 s the
  first time. Something caches between builds there (not investigated); the
  SMLib path has no such cache.

### Memory

`test/memory.mjs`. On wasm this is the heap's high-water mark: wasm memory
grows and never shrinks. On native, it's the process RSS.

| Model | Wasm time | Wasm heap | Native time | Native RSS |
|---|---|---|---|---|
| Plate 4 x 4, built 50 times | | 32 MB, flat (no leak) | | 134 to 175 MB |
| Plate 12 x 12 (144 booleans) | 7.6 s | 138 MB | 9.8 s | 235 MB |
| Same plate, tessellated to a 0.001 mm chord (149k triangles) | 12.0 s | 495 MB | 10.9 s | 227 MB |
| Plate 20 x 20 straight on the kernel (400 booleans) | 10.6 s | 495 MB | 8.2 s | 314 MB |
| 200 spheres unioned (488k triangles) | 78.4 s | **1232 MB** | 77.1 s | 1036 MB |
| 720 degree twist, 60 profiles | 0.6 s | | 0.4 s | |

The heaviest test peaks at 1.2 GB, under a third of wasm32's 4 GB cap.
Because the heap never shrinks, a browser worker that has built a heavy model
holds that memory until it is recycled. The 20 x 20 plate runs straight on
the kernel because BREPcode's DSL overflows the JS call stack on 400 nested
`difference()` calls before it reaches any kernel (that happens natively too).

## Using it in your own project

You need two files, `dist/smlib.mjs` (30 KB of glue) and `dist/smlib.wasm`
(4.4 MB, 1.2 MB brotli), plus `src/api.mjs` if you want the handle-based
API with thrown errors instead of raw embind calls. `dist/` is not committed;
build it as above or take the pair from the latest GitHub release.

### In Node

```bash
git clone https://github.com/RandoTechNerd/smlib-wasm.git
```
```bash
node fetch-upstream.mjs && node build.mjs && node test/smoke.mjs
```
```js
import loadSmlib from "./smlib-wasm/src/api.mjs";
const nk = await loadSmlib();
const box = nk.box(40, 30, 10);
const hole = nk.cylinder(4, 20);
nk.translate(hole, 20, 15, -5);
nk.boolean(3, box, hole);                 // 1 union, 2 intersect, 3 subtract; result stays in `box`
const mesh = nk.tessellate(box, 0.05, 8); // { positions, normals, indices, faceIds }
nk.free(hole); nk.free(box);
```

### In a browser

Load it in a Web Worker: the kernel is single threaded and a big boolean
blocks for seconds, so keep it off the page thread. Copy the two `dist/`
files next to your worker, or anywhere, and point `locateFile` at the wasm:

```js
// kernel.worker.js
import loadSmlib from "./api.mjs";
const nk = await loadSmlib({ locateFile: (f) => "/kernels/" + f }); // where smlib.wasm is served from
self.onmessage = ({ data }) => { /* build with nk, post the tessellation back */ };
```

Serve `smlib.wasm` as `application/wasm` with brotli or gzip on, and cache it
(it is immutable per release, so a long `Cache-Control` max-age is fine). It
needs no SharedArrayBuffer, no COOP/COEP headers and no filesystem, so it runs
on any static host. Memory starts at 32 MB and grows up to 4 GB; the heap
never shrinks, so recycle the worker after a very heavy model if you care.

### The API

Solids are integer handles. Every function has the same name and arguments as
BREPcode's native addon, `native/nvidia-kernel/src/addon.cc`, and each one is
commented in `src/smlib_wasm.cc`, which is the reference for argument
formats (sketch segments, the 4x4 placement matrix, fillet edge indices). The
list: `info`, `free`, `count`, `copy`, `box`, `cylinder`, `cone`, `sphere`,
`torus`, `torusArc`, `translate`, `rotate`, `scale`, `boolean`, `extrude`,
`twistExtrude`, `revolve`, `fillet`, `faces`, `matchFaces`, `edges`, `bbox`,
`volume`, `tessellate`, and `heapBytes`. `test/smoke.mjs` calls each one once
and is the quickest worked example.

What it does not do: read or write STEP or USD (that part of usd-brep is not
compiled in), shell, sweep along a path, or run on more than one thread. Pull
requests that add a kernel call to `src/smlib_wasm.cc` are welcome.

### Licence terms when you ship it

Apache-2.0 lets you use it in a commercial product. Keep the `LICENSE` and
`THIRD_PARTY_NOTICES.md` files with any copy of the wasm you distribute, and
do not use the NVIDIA name or logo to promote your product; "built on
NVIDIA's open-source usd-brep (SMLib)" is fine.

## Licence

Apache-2.0, see `LICENSE` and `THIRD_PARTY_NOTICES.md`.
