import { spawn, execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { frontend, build } from "./paths.mjs";
const require = createRequire(import.meta.url);
if (!fs.existsSync(path.join(build, "runtime/renderdoc.pyd")))
  throw Error(
    "Run pnpm build:prototype once to prepare the isolated backend and worker.",
  );
execFileSync(process.execPath, ["scripts/build-desktop.mjs"], {
  cwd: frontend,
  stdio: "inherit",
});
const { createServer } = await import("vite");
const server = await createServer({
  root: frontend,
  server: { host: "127.0.0.1", port: 5173, strictPort: true },
});
await server.listen();
const child = spawn(require("electron"), [frontend], {
  cwd: frontend,
  env: { ...process.env, RENDERDIC_DEV_URL: "http://127.0.0.1:5173" },
  stdio: "inherit",
  windowsHide: true,
});
child.on("exit", async () => {
  await server.close();
  process.exit();
});
process.once("SIGINT", () => child.kill());
