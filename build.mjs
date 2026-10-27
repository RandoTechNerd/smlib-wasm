// Builds dist/smlib.mjs + dist/smlib.wasm with Emscripten.
//
//   node build.mjs [--eh js|wasm] [--opt -O2] [--out build] [--dist dist]
//
// Finds the SDK at $EMSDK, else ../.toolchain/emsdk next to this folder, and
// sets up its environment itself (no emsdk_env needed). Needs CMake and Ninja.

import { execFileSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = (name, dflt) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : dflt;
};
const eh = arg("--eh", "js");
const opt = arg("--opt", "-O2");
const out = path.resolve(here, arg("--out", "build"));
const dist = path.resolve(here, arg("--dist", "dist"));

const emsdk = process.env.EMSDK || path.resolve(here, "..", ".toolchain", "emsdk");
const emscripten = path.join(emsdk, "upstream", "emscripten");
if (!existsSync(emscripten)) {
  console.error(`No Emscripten at ${emscripten}. Install the SDK there (emsdk install latest; emsdk activate latest) or set EMSDK.`);
  process.exit(1);
}
// the SDK's own Node and Python, if it has them
const bundled = (dir) => {
  const root = path.join(emsdk, dir);
  if (!existsSync(root)) return null;
  const v = readdirSync(root).sort().at(-1);
  return v ? path.join(root, v) : null;
};
const env = { ...process.env, EMSDK: emsdk, EM_CONFIG: path.join(emsdk, ".emscripten") };
const extra = [emscripten, path.join(emsdk, "upstream", "bin")];
const py = bundled("python");
if (py) { env.EMSDK_PYTHON = path.join(py, process.platform === "win32" ? "python.exe" : "bin/python3"); extra.push(py); }
const node = bundled("node");
if (node) extra.push(path.join(node, "bin"));
env.PATH = [...extra, process.env.PATH].join(path.delimiter);

const run = (cmd, args) => {
  console.log(`> ${cmd} ${args.join(" ")}`);
  // a shell on Windows, where cmake and ninja are .exe on PATH but emsdk tools are .bat
  execFileSync(cmd, args, { cwd: here, env, stdio: "inherit", shell: process.platform === "win32" });
};
const q = (s) => (process.platform === "win32" && /\s/.test(s) ? `"${s}"` : s);

const t0 = Date.now();
run("cmake", [
  "-S", q(here), "-B", q(out), "-G", "Ninja",
  `-DCMAKE_TOOLCHAIN_FILE=${q(path.join(emscripten, "cmake", "Modules", "Platform", "Emscripten.cmake"))}`,
  "-DCMAKE_BUILD_TYPE=Release",
  `-DSMLIB_EH=${eh}`,
  `-DSMLIB_OPT=${opt}`,
  `-DSMLIB_DIST=${q(dist)}`,
]);
run("cmake", ["--build", q(out), "--parallel"]);
console.log(`built ${path.relative(here, dist)}/smlib.mjs + smlib.wasm (exceptions: ${eh}, ${opt}) in ${((Date.now() - t0) / 1000).toFixed(0)} s`);
