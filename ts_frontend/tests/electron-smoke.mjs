import { _electron as electron } from "playwright";
import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import { createRequire } from "node:module";
import { frontend, root, build } from "../scripts/paths.mjs";
const require = createRequire(import.meta.url);
const packaged = process.argv.includes("--packaged");
const visualOnly = process.argv.includes("--visual-only");
const application = await electron.launch({
  executablePath: packaged
    ? path.join(frontend, "dist/RenderDicTS-win32-x64/RenderDicTS.exe")
    : require("electron"),
  args: packaged ? [] : [frontend],
  env: {
    ...process.env,
    RENDERDIC_TEST: "1",
    RENDERDIC_TEST_PROFILE: path.join(build, "test-profile"),
  },
  timeout: 60000,
});
const page = await application.firstWindow();
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
page.on("console", (m) => {
  if (m.type() === "error") console.error("Renderer:", m.text());
});
const call = (method, params = {}) =>
  page.evaluate(([m, p]) => window.frontendTest.call(m, p), [method, params]);
const stats = [];
async function waitNative(visible = true) {
  await page.waitForFunction(
    async (visible) => {
      const s = await window.frontendTest.call("inspect");
      return s.hwnd !== "0" && !!s.visible === visible && s.width > 100;
    },
    visible,
    { timeout: 30000 },
  );
  return call("inspect", { readback: visible });
}
async function openCapture(file) {
  const start = performance.now();
  await call("openUI", { path: file });
  await page
    .getByText("抓帧已加载", { exact: true })
    .waitFor({ timeout: 180000 });
  const native = await waitNative();
  assert.ok(native.parent);
  assert.ok(native.bytes > 0);
  assert.ok(native.uniqueBytes > 4);
  if (/d3d1[12]_direct/.test(file)) {
    await application.evaluate(({ BrowserWindow }) => {
      const win = BrowserWindow.getAllWindows()[0];
      win.show();
      win.focus();
    });
    await page.waitForFunction(
      async () =>
        ((await window.frontendTest.call("inspect")).screenPixel & 0xffffff) ===
        0x00ff00,
      null,
      { timeout: 10000 },
    );
  }
  const selection = page.locator("[role=treeitem][aria-selected=true]");
  assert.equal(await selection.count(), 1);
  stats.push({
    file,
    elapsedMs: Math.round(performance.now() - start),
    native,
  });
  console.log(
    "Surface visibility:",
    native.unobscured,
    native.topClass,
    native.screenPixel,
  );
  return native;
}
try {
  await page.getByRole("button", { name: "打开抓帧文件" }).waitFor();
  console.log("Main window rendered");
  const hello = await call("hello");
  console.log("Replay connected:", hello);
  const samples = path.join(root, "build/setthreadcontext_fixed_717b937b4");
  if (!visualOnly) {
    for (const file of [
      "d3d11_direct_frame41.rdc",
      "d3d12_direct_frame24.rdc",
      "gl_direct_frame3227.rdc",
    ]) {
      const info = await openCapture(path.join(samples, file));
      console.log(
        "Native output passed:",
        file,
        info.width,
        info.height,
        info.crc32,
      );
      await page.getByRole("button", { name: "Shader", exact: true }).click();
      await page.locator(".monaco-editor").waitFor({ timeout: 30000 });
      await page.waitForFunction(
        async () => !(await window.frontendTest.call("inspect")).visible,
      );
      await page.getByRole("button", { name: "纹理", exact: true }).click();
      await waitNative();
      await page.getByRole("button", { name: "外观设置" }).click();
      await page.getByRole("dialog").waitFor();
      await page.waitForFunction(
        async () => !(await window.frontendTest.call("inspect")).visible,
      );
      await page.getByRole("button", { name: "完成", exact: true }).click();
      await waitNative();
      await page.evaluate(() => window.frontendTest.window("maximize"));
      await waitNative();
      await page.evaluate(() => window.frontendTest.window("unmaximize"));
      await waitNative();
      await page.evaluate(() => window.frontendTest.window("minimize"));
      await page.waitForFunction(
        async () => !(await window.frontendTest.call("inspect")).visible,
      );
      await page.evaluate(() => window.frontendTest.window("restore"));
      await waitNative();
      await page.getByRole("button", { name: "关闭", exact: true }).click();
      await page.waitForFunction(
        async () => (await window.frontendTest.call("inspect")).hwnd === "0",
      );
    }
    for (let i = 0; i < 10; i++) {
      await openCapture(path.join(samples, "d3d11_direct_frame41.rdc"));
      await page.getByRole("button", { name: "关闭", exact: true }).click();
      await page.getByRole("button", { name: "打开抓帧文件" }).waitFor();
    }
    const big =
      "C:\\Users\\Admin\\Documents\\Codex\\2026-11-06\\zen\\outputs\\Seafarer_DX12_frame1873.rdc";
    if (fs.existsSync(big)) {
      await openCapture(big);
      const items = await page.locator("[role=treeitem] .event-name").count();
      if (items > 2) {
        await page.locator("[role=treeitem] .event-name").nth(1).click();
        await page.locator("[role=treeitem] .event-name").nth(2).click();
        await page.getByText("抓帧已加载", { exact: true }).waitFor();
      }
      await page.getByRole("button", { name: "关闭", exact: true }).click();
    }
    await openCapture(path.join(samples, "d3d11_direct_frame41.rdc"));
    await call("kill");
    await page.locator(".error[role=alert]").waitFor();
    await page.getByRole("button", { name: "重新连接", exact: true }).click();
    await page.getByText("抓帧已加载", { exact: true }).waitFor();
    await waitNative();
  } else {
    await openCapture(path.join(samples, "d3d11_direct_frame41.rdc"));
  }
  assert.deepEqual(errors, []);
  await page.getByRole("button", { name: "外观设置" }).click();
  await page
    .getByRole("combobox", { name: "主题", exact: true })
    .selectOption("dark");
  await page.getByRole("button", { name: "完成", exact: true }).click();
  await waitNative();
  await page.waitForFunction(
    async () =>
      ((await window.frontendTest.call("inspect")).screenPixel & 0xffffff) ===
      0x00ff00,
  );
  await call("inspect", {
    readback: true,
    png: path.join(build, "diagnostic-readback.png"),
  });
  const desktopShot = await application.evaluate(
    async ({ desktopCapturer, screen, BrowserWindow }) => {
      const win = BrowserWindow.getAllWindows()[0];
      win.show();
      win.focus();
      const display = screen.getDisplayMatching(win.getBounds());
      const sources = await desktopCapturer.getSources({
        types: ["screen"],
        thumbnailSize: {
          width: display.size.width * display.scaleFactor,
          height: display.size.height * display.scaleFactor,
        },
      });
      const source =
        sources.find((s) => s.display_id === String(display.id)) ?? sources[0];
      const rect = screen.dipToScreenRect(win, win.getBounds());
      return source.thumbnail
        .crop({
          x: rect.x - display.bounds.x * display.scaleFactor,
          y: rect.y - display.bounds.y * display.scaleFactor,
          width: rect.width,
          height: rect.height,
        })
        .toPNG()
        .toString("base64");
    },
  );
  fs.writeFileSync(
    path.join(build, "native-desktop.png"),
    Buffer.from(desktopShot, "base64"),
  );
  const reportFile = path.join(
    build,
    packaged && !visualOnly
      ? "packaged-smoke.json"
      : visualOnly
        ? "visual-smoke.json"
        : "electron-smoke.json",
  );
  fs.writeFileSync(
    reportFile,
    JSON.stringify(
      {
        hello,
        stats,
        lifecycleCycles: visualOnly ? 0 : 10,
        rendererErrors: errors,
        nativeDesktopScreenshot: "native-desktop.png",
      },
      null,
      2,
    ),
  );
  console.log("Electron integration passed; report:", reportFile);
  if (process.argv.includes("--keep-open")) {
    console.log("Keeping window open; send newline to finish.");
    await new Promise((resolve) => process.stdin.once("data", resolve));
  }
} finally {
  await application.close();
}
