import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import path from "node:path";
import fs from "node:fs";
import { build } from "../scripts/paths.mjs";
export async function connectWorker(runtime = path.join(build, "runtime")) {
  const child = spawn(
    path.join(runtime, "RenderDicReplayWorker.exe"),
    ["--parent-pid", String(process.pid)],
    {
      cwd: runtime,
      windowsHide: true,
      stdio: "pipe",
      env: {
        ...process.env,
        RENDERDIC_FRONTEND_LOG_DIR: path.join(build, "worker-core-logs"),
      },
    },
  );
  const pending = new Map();
  let id = 0;
  const log = fs.createWriteStream(path.join(build, "worker-smoke.log"), {
    flags: "a",
  });
  child.stderr.pipe(log);
  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(Error("Worker startup timed out")),
      30000,
    );
    child.once("error", reject);
    child.once("exit", (code) => {
      clearTimeout(timer);
      reject(Error(`Worker exited: ${code}`));
      for (const p of pending.values())
        p.reject(Error(`Worker exited: ${code}`));
      pending.clear();
      log.end();
    });
    createInterface({ input: child.stdout }).on("line", (line) => {
      const msg = JSON.parse(line);
      if (msg.event === "ready") {
        clearTimeout(timer);
        resolve();
      }
      const p = pending.get(msg.id);
      if (p) {
        pending.delete(msg.id);
        clearTimeout(p.timer);
        if (msg.error)
          p.reject(
            Object.assign(Error(msg.error.message), { code: msg.error.code }),
          );
        else p.resolve(msg.result);
      }
    });
  });
  await ready;
  const call = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const n = ++id;
      const timer = setTimeout(() => {
        pending.delete(n);
        reject(Error(`Timeout: ${method}`));
      }, 180000);
      pending.set(n, { resolve, reject, timer });
      child.stdin.write(JSON.stringify({ id: n, method, params }) + "\n");
    });
  return {
    child,
    call,
    stop: async () => {
      await call("shutdown");
      child.stdin.end();
    },
  };
}
