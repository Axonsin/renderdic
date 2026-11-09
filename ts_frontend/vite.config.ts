import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwind from "@tailwindcss/vite";
export default defineConfig({
  root: ".",
  base: "./",
  plugins: [react(), tailwind()],
  build: { outDir: "ui-dist", emptyOutDir: true },
  server: { host: "127.0.0.1" },
  worker: { format: "es" },
});
