import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { root, build } from "../scripts/paths.mjs";
import { connectWorker } from "./protocol.mjs";
const worker = await connectWorker();
const results = [];
try {
  const hello = await worker.call("hello");
  assert.equal(hello.python.startsWith("3.6."), true);
  console.log("Worker handshake:", hello);
  await assert.rejects(
    worker.call("openCapture", {
      path: path.join(build, "does-not-exist.rdc"),
    }),
  );
  for (const [api, file] of [
    ["D3D11", "d3d11_direct_frame41.rdc"],
    ["D3D12", "d3d12_direct_frame24.rdc"],
    ["OpenGL", "gl_direct_frame3227.rdc"],
  ]) {
    const opened = await worker.call("openCapture", {
      path: path.join(root, "build/setthreadcontext_fixed_717b937b4", file),
    });
    assert.ok(opened.events.length);
    assert.ok(opened.textures.length);
    assert.ok(opened.initial.eventId);
    assert.ok(opened.initial.stages.length);
    const shader = await worker.call("getShader", {
      stage: opened.initial.stages[0].stage,
      generation: opened.generation,
    });
    assert.ok(shader.text.length);
    await assert.rejects(
      worker.call("getShader", {
        stage: "VS",
        generation: opened.generation - 1,
      }),
    );
    const latest = opened.initial.eventId;
    await Promise.allSettled([
      worker.call("selectEvent", {
        eventId: opened.events[0].eventId,
        generation: opened.generation,
      }),
      worker.call("selectEvent", {
        eventId: latest,
        generation: opened.generation,
      }),
    ]);
    assert.equal((await worker.call("inspect")).eventId, latest);
    await assert.rejects(
      worker.call("getShader", {
        stage: "VS",
        generation: opened.generation,
        eventId: latest + 999,
      }),
      (error) => error.code === "SUPERSEDED",
    );
    results.push({
      api,
      events: opened.events.length,
      textures: opened.textures.length,
      initial: opened.initial,
      shaderLength: shader.text.length,
      loadMs: opened.elapsedMs,
    });
    await worker.call("closeCapture");
    console.log(api, "passed", results.at(-1).loadMs, "ms");
  }
  for (let i = 0; i < 10; i++) {
    await worker.call("openCapture", {
      path: path.join(
        root,
        "build/setthreadcontext_fixed_717b937b4/d3d11_direct_frame41.rdc",
      ),
    });
    await worker.call("closeCapture");
  }
  fs.writeFileSync(
    path.join(build, "worker-smoke.json"),
    JSON.stringify({ hello, results, lifecycleCycles: 10 }, null, 2),
  );
} finally {
  await worker.stop();
}
