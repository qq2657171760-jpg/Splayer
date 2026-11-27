import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const viteCli = path.join(root, "node_modules", "vite", "bin", "vite.js");
if (!existsSync(viteCli)) {
  throw new Error("Vite CLI not found. Run pnpm install before building the desktop app.");
}

const result = spawnSync(process.execPath, [
  viteCli,
  "build",
  "--config",
  path.join(root, "vite.config.ts"),
], {
  cwd: root,
  env: {
    ...process.env,
    BASE_PATH: "./",
    PORT: "5173",
  },
  stdio: "inherit",
  windowsHide: true,
});

if (result.error) throw result.error;
if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
console.log("[build-desktop] Desktop renderer bundle created at dist/public.");
