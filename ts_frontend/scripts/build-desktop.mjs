import { build as bundle } from "esbuild";
import { frontend } from "./paths.mjs";
await bundle({
  absWorkingDir: frontend,
  entryPoints: ["desktop/main.ts", "desktop/preload.ts"],
  bundle: true,
  platform: "node",
  format: "cjs",
  outdir: ".build/desktop",
  outExtension: { ".js": ".cjs" },
  external: ["electron"],
  sourcemap: true,
});
