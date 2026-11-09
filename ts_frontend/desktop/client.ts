import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { EventEmitter } from "node:events";
import { createInterface } from "node:readline";
export class ReplayClient extends EventEmitter {
  private process?: ChildProcessWithoutNullStreams;
  private pending = new Map<
    number,
    {
      resolve: (v: any) => void;
      reject: (e: Error) => void;
      timer: NodeJS.Timeout;
    }
  >();
  private serial = 0;
  constructor(
    private runtime: string,
    private logs: string,
  ) {
    super();
  }
  async start() {
    for (const file of [
      "RenderDicReplayWorker.exe",
      "renderdic.dll",
      "renderdoc.pyd",
      "python36.dll",
      "python36.zip",
    ]) {
      if (!fs.existsSync(path.join(this.runtime, file)))
        throw Error(`缺少回放运行库：${file}。请重新构建或恢复完整便携目录。`);
    }
    fs.mkdirSync(this.logs, { recursive: true });
    const child = spawn(
      path.join(this.runtime, "RenderDicReplayWorker.exe"),
      ["--parent-pid", String(process.pid)],
      {
        cwd: this.runtime,
        windowsHide: true,
        stdio: "pipe",
        env: { ...process.env, RENDERDIC_FRONTEND_LOG_DIR: this.logs },
      },
    );
    this.process = child;
    const log = fs.createWriteStream(path.join(this.logs, "replay.log"), {
      flags: "a",
    });
    child.stderr.pipe(log);
    const ready = new Promise<void>((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(Error("回放进程启动超时，请检查运行库。")),
        30000,
      );
      child.once("error", (e) => {
        clearTimeout(timeout);
        reject(e);
      });
      child.once("exit", (code) => {
        clearTimeout(timeout);
        reject(Error(`回放进程已退出 (${code})`));
      });
      createInterface({ input: child.stdout }).on("line", (line) => {
        try {
          const msg = JSON.parse(line);
          if (msg.event === "ready") {
            clearTimeout(timeout);
            resolve();
          }
          if (msg.id !== undefined) {
            const entry = this.pending.get(msg.id);
            if (!entry) return;
            this.pending.delete(msg.id);
            clearTimeout(entry.timer);
            if (msg.error)
              entry.reject(
                Object.assign(Error(msg.error.message), {
                  code: msg.error.code,
                }),
              );
            else entry.resolve(msg.result);
          } else this.emit("notice", msg);
        } catch {
          log.write(`Invalid protocol line: ${line.slice(0, 500)}\n`);
        }
      });
    });
    child.once("exit", (code) => {
      for (const p of this.pending.values()) {
        clearTimeout(p.timer);
        p.reject(Error(`回放进程已退出 (${code})`));
      }
      this.pending.clear();
      log.end();
      if (this.process === child) {
        this.process = undefined;
        this.emit("notice", {
          event: "crashed",
          message: "回放进程已退出。请重新连接后打开抓帧。",
        });
      }
    });
    await ready;
    const hello = await this.call("hello");
    if (hello.protocol !== 1 || !hello.python.startsWith("3.6.")) {
      await this.stop();
      throw Error("回放运行库版本不匹配，请重新构建。");
    }
    return hello;
  }
  call(method: string, params: unknown = {}): Promise<any> {
    const child = this.process;
    if (!child || child.killed)
      return Promise.reject(Error("回放进程未连接。"));
    const id = ++this.serial;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => {
          this.pending.delete(id);
          reject(Error("回放请求超时，可重新连接工作进程。"));
        },
        method === "openCapture" ? 180000 : 30000,
      );
      this.pending.set(id, { resolve, reject, timer });
      child.stdin.write(
        JSON.stringify({ id, method, params }) + "\n",
        (error) => {
          if (error) {
            clearTimeout(timer);
            this.pending.delete(id);
            reject(error);
          }
        },
      );
    });
  }
  async stop() {
    const child = this.process;
    if (!child) return;
    this.process = undefined;
    await Promise.race([
      this.callOnClosing(child),
      new Promise<void>((resolve) => setTimeout(resolve, 3000)),
    ]);
    if (child.exitCode === null) child.kill();
  }
  private async callOnClosing(child: ChildProcessWithoutNullStreams) {
    await new Promise<void>((resolve) => {
      child.once("exit", () => resolve());
      child.stdin.end(JSON.stringify({ id: 0, method: "shutdown" }) + "\n");
    });
  }
  killForTest() {
    this.process?.kill();
  }
}
