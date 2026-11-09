import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
export const frontend = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
);
export const root = path.resolve(frontend, "..");
export const build = path.join(frontend, ".build");
export function visualStudio() {
  return execFileSync(
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
    [
      "-latest",
      "-products",
      "*",
      "-requires",
      "Microsoft.Component.MSBuild",
      "-property",
      "installationPath",
    ],
    { encoding: "utf8" },
  ).trim();
}
export function msbuild() {
  return path.join(visualStudio(), "MSBuild/Current/Bin/MSBuild.exe");
}
