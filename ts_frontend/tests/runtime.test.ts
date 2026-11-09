import { it, expect, vi, afterEach } from "vitest";
import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { ReplayClient } from "../desktop/client";
vi.mock("node:child_process", () => ({ spawn: vi.fn() }));
const build = path.resolve(".build");
let temporary: string | undefined;
afterEach(() => {
  if (temporary) {
    if (!temporary.startsWith(build + path.sep))
      throw Error("Unsafe test cleanup path");
    fs.rmSync(temporary, { recursive: true, force: true });
  }
  vi.clearAllMocks();
});
it("rejects an incomplete runtime before Windows can launch a broken executable", async () => {
  fs.mkdirSync(build, { recursive: true });
  temporary = fs.mkdtempSync(path.join(build, "missing-runtime-"));
  for (const name of [
    "RenderDicReplayWorker.exe",
    "renderdic.dll",
    "renderdoc.pyd",
    "python36.zip",
  ])
    fs.writeFileSync(path.join(temporary, name), "fixture");
  const client = new ReplayClient(temporary, path.join(temporary, "logs"));
  await expect(client.start()).rejects.toThrow("python36.dll");
  expect(spawn).not.toHaveBeenCalled();
});
