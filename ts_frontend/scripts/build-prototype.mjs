import { execFileSync } from "node:child_process";
import { frontend } from "./paths.mjs";
const node = process.execPath;
for (const script of ["build-backend.mjs", "build-worker.mjs"])
  execFileSync(node, [`scripts/${script}`], {
    cwd: frontend,
    stdio: "inherit",
  });
execFileSync(node, ["node_modules/typescript/bin/tsc", "--noEmit"], {
  cwd: frontend,
  stdio: "inherit",
});
execFileSync(node, ["node_modules/vite/bin/vite.js", "build"], {
  cwd: frontend,
  stdio: "inherit",
});
for (const script of ["build-desktop.mjs", "package.mjs"])
  execFileSync(node, [`scripts/${script}`], {
    cwd: frontend,
    stdio: "inherit",
  });
