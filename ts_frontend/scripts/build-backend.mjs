import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { spawn, execFileSync } from "node:child_process";
import { root, frontend, build, msbuild } from "./paths.mjs";
fs.mkdirSync(build, { recursive: true });
const protectedFiles = [
  "x64/Development/qrenderdic.exe",
  "x64/Development/renderdic.dll",
  "x64/Development/pymodules/renderdoc.pyd",
  "x64/Release/qrenderdic.exe",
  "x64/Release/renderdic.dll",
  "x64/Release/pymodules/renderdoc.pyd",
];
const hash = (f) =>
  fs.existsSync(f)
    ? crypto.createHash("sha256").update(fs.readFileSync(f)).digest("hex")
    : null;
const before = Object.fromEntries(
  protectedFiles.map((p) => [p, hash(path.join(root, p))]),
);
fs.writeFileSync(
  path.join(build, "original-artifacts.json"),
  JSON.stringify(before, null, 2),
);
const out = path.join(build, "backend");
const glSource = fs.readFileSync(
  path.join(root, "renderdoc/driver/gl/gl_hooks.cpp"),
  "utf8",
);
const marker = "  // On Android the system GLES shim libraries";
const endMarker = "  // Last resort on Android:";
const start = glSource.indexOf(marker),
  end = glSource.indexOf(endMarker, start);
if (
  start < 0 ||
  end < start ||
  !glSource.slice(start, end).includes("dlopen(lib, RTLD_LAZY | RTLD_GLOBAL)")
)
  throw Error(
    "GL build adapter no longer matches upstream; review it before building.",
  );
fs.mkdirSync(path.join(out, "generated"), { recursive: true });
fs.writeFileSync(
  path.join(out, "generated/gl_hooks.win32.cpp"),
  glSource.slice(0, start) +
    "#if ENABLED(RDOC_ANDROID)\n" +
    glSource.slice(start, end) +
    "#endif\n\n" +
    glSource.slice(end),
);
fs.writeFileSync(
  path.join(out, "generated/source-adapter.json"),
  JSON.stringify(
    {
      source: "renderdoc/driver/gl/gl_hooks.cpp",
      sha256: hash(path.join(root, "renderdoc/driver/gl/gl_hooks.cpp")),
      change:
        "Guard Android GLES fallback with RDOC_ANDROID in isolated Windows build copy only",
    },
    null,
    2,
  ),
);
const props = path.join(frontend, "scripts/backend-output.props");
const common = [
  "/nologo",
  "/p:Configuration=Development",
  "/p:Platform=x64",
  `/p:SolutionDir=${root}\\`,
  `/p:FrontendBuildRoot=${out}`,
  `/p:ForceImportBeforeCppTargets=${props}`,
];
const projects = [
  "renderdoc/renderdoc.vcxproj",
  "qrenderdoc/Code/pyrenderdoc/pyrenderdoc_module.vcxproj",
  "renderdoc/driver/d3d12/renderdoc_d3d12.vcxproj",
];
for (const p of projects) {
  const result = JSON.parse(
    execFileSync(
      msbuild(),
      [path.join(root, p), ...common, "-getProperty:OutDir,IntDir"],
      { encoding: "utf8" },
    ),
  );
  for (const value of Object.values(result.Properties))
    if (!path.resolve(value).startsWith(out + path.sep))
      throw Error(`Unsafe output path: ${value}`);
  console.log("Verified isolated outputs:", p, result.Properties);
}
const log = fs.openSync(path.join(build, "backend-build.log"), "w");
console.log(
  "Compiling core + Python binding; log:",
  path.join(build, "backend-build.log"),
);
const child = spawn(
  msbuild(),
  [
    path.join(root, "qrenderdoc/Code/pyrenderdoc/pyrenderdoc_module.vcxproj"),
    ...common,
    "/t:Build",
    "/m:3",
    "/v:minimal",
  ],
  { cwd: root, stdio: ["ignore", log, log], windowsHide: true },
);
const code = await new Promise((resolve) => child.on("exit", resolve));
fs.closeSync(log);
for (const [p, h] of Object.entries(before))
  if (hash(path.join(root, p)) !== h)
    throw Error(`Original artifact changed: ${p}`);
if (code !== 0) {
  console.error(
    fs
      .readFileSync(path.join(build, "backend-build.log"), "utf8")
      .slice(-18000),
  );
  process.exit(code || 1);
}
console.log("Backend compiled; original artifacts unchanged.");
