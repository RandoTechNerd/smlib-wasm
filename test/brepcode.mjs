// Where BREPcode is (read only): $BREPCODE, else the desktop alpha worktree,
// which has the native addon built.
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const BREPCODE = path.resolve(process.env.BREPCODE || path.join(here, "..", "..", "BrepScript-wt", "alpha-integration"));
export const at = (p) => pathToFileURL(path.join(BREPCODE, p)).href;
