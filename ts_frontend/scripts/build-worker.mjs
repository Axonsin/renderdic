import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { root, frontend, build, visualStudio } from "./paths.mjs";
fs.mkdirSync(path.join(build, "native"), { recursive: true });
const vs = visualStudio();
const tool = fs.readdirSync(path.join(vs, "VC/Tools/MSVC")).sort().at(-1);
const msvc = path.join(vs, "VC/Tools/MSVC", tool);
const sdkRoot = "C:\\Program Files (x86)\\Windows Kits\\10";
const sdk = fs
  .readdirSync(path.join(sdkRoot, "Include"))
  .filter((v) => v.startsWith("10."))
  .sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
  .at(-1);
const python = path.join(root, "qrenderdoc/3rdparty/python");
const include = [
  path.join(python, "include"),
  path.join(msvc, "include"),
  ...["ucrt", "shared", "um"].map((d) => path.join(sdkRoot, "Include", sdk, d)),
];
const lib = [
  path.join(python, "x64"),
  path.join(msvc, "lib/x64"),
  ...["ucrt", "um"].map((d) => path.join(sdkRoot, "Lib", sdk, d, "x64")),
];
execFileSync(
  path.join(msvc, "bin/Hostx64/x64/cl.exe"),
  [
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/MD",
    "/W4",
    "/O2",
    ...include.map((p) => "/I" + p),
    path.join(frontend, "native/worker.cpp"),
    "/Fe:RenderDicReplayWorker.exe",
    "/link",
    ...lib.map((p) => "/LIBPATH:" + p),
    "python36.lib",
    "user32.lib",
    "gdi32.lib",
    "/SUBSYSTEM:CONSOLE",
  ],
  { cwd: path.join(build, "native"), stdio: "inherit", windowsHide: true },
);
const runtime = path.join(build, "runtime");
fs.mkdirSync(runtime, { recursive: true });
execFileSync(
  path.join(msvc, "bin/Hostx64/x64/cl.exe"),
  [
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/MD",
    "/W4",
    "/O2",
    "/LD",
    ...include.map((p) => "/I" + p),
    path.join(frontend, "native/viewport.cpp"),
    "/Fe:viewport.node",
    "/link",
    ...lib.map((p) => "/LIBPATH:" + p),
    "user32.lib",
    "gdi32.lib",
  ],
  { cwd: path.join(build, "native"), stdio: "inherit", windowsHide: true },
);
fs.copyFileSync(
  path.join(build, "native/viewport.node"),
  path.join(runtime, "viewport.node"),
);
const redist = path.join(vs, "VC/Redist/MSVC", tool, "x64/Microsoft.VC145.CRT");
if (fs.existsSync(redist))
  for (const file of fs.readdirSync(redist).filter((f) => f.endsWith(".dll")))
    fs.copyFileSync(path.join(redist, file), path.join(runtime, file));
for (const file of ["python36.dll", "_ctypes.pyd"])
  fs.copyFileSync(path.join(python, "x64", file), path.join(runtime, file));
fs.copyFileSync(
  path.join(python, "python36.zip"),
  path.join(runtime, "python36.zip"),
);
fs.copyFileSync(
  path.join(build, "native/RenderDicReplayWorker.exe"),
  path.join(runtime, "RenderDicReplayWorker.exe"),
);
for (const file of ["renderdic.dll", "dbghelp.dll", "symsrv.dll"])
  fs.copyFileSync(
    path.join(build, "backend/bin", file),
    path.join(runtime, file),
  );
fs.copyFileSync(
  path.join(build, "backend/bin/pymodules/renderdoc.pyd"),
  path.join(runtime, "renderdoc.pyd"),
);
fs.cpSync(path.join(frontend, "bridge"), path.join(runtime, "bridge"), {
  recursive: true,
});
console.log("Isolated worker runtime:", runtime);
