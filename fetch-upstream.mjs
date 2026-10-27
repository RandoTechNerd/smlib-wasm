// Fetches the kernel source (SMLib + SM_API only) of NVIDIA's open-source
// usd-brep at the pinned release into upstream/. Apache-2.0; see
// upstream/LICENSE.
//
//   node fetch-upstream.mjs               (sparse git fetch from GitHub)
//   node fetch-upstream.mjs --from DIR    (copy an existing checkout's LICENSE,
//                                          THIRD_PARTY_NOTICES.md and source/)
import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

export const REPO = "https://github.com/NVIDIA-Omniverse/usd-brep.git";
export const COMMIT = "3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5"; // v1.0.1

const dir = path.join(path.dirname(fileURLToPath(import.meta.url)), "upstream");
const from = process.argv.includes("--from") ? process.argv[process.argv.indexOf("--from") + 1] : null;

if (from) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  for (const p of ["LICENSE", "THIRD_PARTY_NOTICES.md"]) cpSync(path.join(from, p), path.join(dir, p));
  for (const lib of ["SMLib", "SM_API"]) cpSync(path.join(from, "source", lib), path.join(dir, "source", lib), { recursive: true });
  console.log(`copied SMLib + SM_API from ${from} (make sure it is usd-brep ${COMMIT.slice(0, 7)})`);
} else {
  const git = (...a) => execFileSync("git", a, { cwd: dir, stdio: "inherit", env: { ...process.env, MSYS_NO_PATHCONV: "1" } });
  const have = existsSync(path.join(dir, ".git")) &&
    execFileSync("git", ["rev-parse", "HEAD"], { cwd: dir }).toString().trim() === COMMIT &&
    existsSync(path.join(dir, "source", "SMLib", "src"));
  if (have) {
    console.log(`upstream already at ${COMMIT.slice(0, 7)}`);
  } else {
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir, { recursive: true });
    git("init", "-q");
    git("remote", "add", "origin", REPO);
    git("config", "core.sparseCheckout", "true");
    git("sparse-checkout", "set", "--no-cone", "/source/SMLib/", "/source/SM_API/", "/LICENSE", "/THIRD_PARTY_NOTICES.md");
    git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", COMMIT);
    git("checkout", "-q", "FETCH_HEAD");
    console.log(`fetched usd-brep ${COMMIT.slice(0, 7)}`);
  }
}
