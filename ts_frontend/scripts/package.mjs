import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { execFileSync } from "node:child_process";
import { packager } from "@electron/packager";
import { frontend, root, build } from "./paths.mjs";
const stage = path.join(build, "package-source");
fs.mkdirSync(path.join(stage, ".build/desktop"), { recursive: true });
fs.cpSync(path.join(build, "desktop"), path.join(stage, ".build/desktop"), {
  recursive: true,
});
fs.cpSync(path.join(frontend, "ui-dist"), path.join(stage, "ui-dist"), {
  recursive: true,
});
const pkg = JSON.parse(
  fs.readFileSync(path.join(frontend, "package.json"), "utf8"),
);
fs.writeFileSync(
  path.join(stage, "package.json"),
  JSON.stringify(
    {
      name: "renderdic-ts",
      version: pkg.version,
      main: ".build/desktop/main.cjs",
      author: "RenderDic contributors",
      description: "Independent RenderDic analysis frontend",
    },
    null,
    2,
  ),
);
const runtime = path.join(build, "runtime");
const sha = (file) =>
  crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
const hashes = Object.fromEntries(
  fs
    .readdirSync(runtime)
    .filter((f) => /\.(exe|dll|pyd|zip|node)$/.test(f))
    .map((f) => [f, sha(path.join(runtime, f))]),
);
fs.writeFileSync(
  path.join(runtime, "build-manifest.json"),
  JSON.stringify(
    {
      commit: execFileSync("git", ["rev-parse", "HEAD"], {
        cwd: root,
        encoding: "utf8",
      }).trim(),
      builtAt: new Date().toISOString(),
      configuration: "Development|x64",
      node: process.version,
      electron: pkg.devDependencies.electron,
      python: "3.6.4",
      files: hashes,
      workerScript: sha(path.join(runtime, "bridge/worker.py")),
      lockfile: sha(path.join(frontend, "pnpm-lock.yaml")),
      buildAdaptations: [
        "Android-only GLES fallback guarded in generated Windows compilation copy",
        "Embedded vk_handle_guard_tests.cpp excluded due missing DoStringise<Reason>; driver implementation unchanged",
      ],
    },
    null,
    2,
  ),
);
fs.copyFileSync(
  path.join(root, "LICENSE.md"),
  path.join(runtime, "RenderDoc-LICENSE.md"),
);
fs.copyFileSync(
  path.join(root, "qrenderdoc/3rdparty/python/LICENSE"),
  path.join(runtime, "Python-LICENSE.txt"),
);
const outputs = await packager({
  dir: stage,
  out: path.join(frontend, "dist"),
  name: "RenderDicTS",
  executableName: "RenderDicTS",
  platform: "win32",
  arch: "x64",
  electronVersion: pkg.devDependencies.electron,
  asar: true,
  overwrite: true,
  prune: false,
});
fs.cpSync(runtime, path.join(outputs[0], "resources/runtime"), {
  recursive: true,
});
console.log("Portable application:", outputs[0]);
