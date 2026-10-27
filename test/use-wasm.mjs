// node --import ./test/use-wasm.mjs <script>
//
// Loads the wasm kernel, then answers every require() of BREPcode's addon
// (brepcode_nvidia.node) with it, so BREPcode's own adapter.cjs, engine and
// tests run on the wasm build without a line of theirs changed.
import Module from "node:module";
import loadSmlib from "../src/api.mjs";

const t0 = performance.now();
const nk = await loadSmlib();
globalThis.__smlibWasm = { nk, loadMs: performance.now() - t0 };

const load = Module._load;
Module._load = function (request, ...rest) {
  if (/brepcode_nvidia\.node$/.test(request)) return nk;
  return load.call(this, request, ...rest);
};
