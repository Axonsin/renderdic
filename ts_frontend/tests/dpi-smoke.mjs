import { _electron as electron } from "playwright";
import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import { createRequire } from "node:module";
import { frontend, root, build } from "../scripts/paths.mjs";
const require = createRequire(import.meta.url);
const results = [];
for (const scale of [1, 1.25, 1.5]) {
  const application = await electron.launch({
    executablePath: require("electron"),
    args: [frontend, `--force-device-scale-factor=${scale}`],
    env: {
      ...process.env,
      RENDERDIC_TEST: "1",
      RENDERDIC_TEST_PROFILE: path.join(build, "dpi-profile"),
    },
  });
  try {
    const page = await application.firstWindow();
    await page.getByRole("button", { name: "打开抓帧文件" }).waitFor();
    await page.evaluate(() =>
      localStorage.removeItem("renderdic-ts-appearance-v1"),
    );
    await page.reload();
    await page.evaluate(
      (file) => window.frontendTest.call("openUI", { path: file }),
      path.join(
        root,
        "build/setthreadcontext_fixed_717b937b4/d3d12_direct_frame24.rdc",
      ),
    );
    await page.getByText("抓帧已加载", { exact: true }).waitFor();
    await page.waitForFunction(
      async () => !!(await window.frontendTest.call("inspect")).visible,
    );
    const measure = async () =>
      page.evaluate(async () => {
        const r = document
          .querySelector("[data-testid=viewport]")
          .getBoundingClientRect();
        const native = await window.frontendTest.call("inspect", {
          readback: true,
        });
        return {
          css: { x: r.x, y: r.y, width: r.width, height: r.height },
          dpr: devicePixelRatio,
          native,
        };
      });
    const before = await measure();
    assert.ok(
      Math.abs(before.native.width - before.css.width * before.dpr) <= 2,
    );
    assert.ok(
      Math.abs(before.native.height - before.css.height * before.dpr) <= 2,
    );
    const handle = page.getByRole("separator", { name: "调整事件栏宽度" });
    const bounds = await handle.boundingBox();
    await page.mouse.move(bounds.x + 2, bounds.y + 80);
    await page.mouse.down();
    await page.mouse.move(bounds.x + 52, bounds.y + 80, { steps: 8 });
    await page.mouse.up();
    await page.waitForTimeout(150);
    const after = await measure();
    assert.ok(after.css.width < before.css.width - 20);
    assert.ok(Math.abs(after.native.width - after.css.width * after.dpr) <= 2);
    for (const theme of ["dark", "light"]) {
      await page.getByRole("button", { name: "外观设置" }).click();
      await page
        .getByRole("combobox", { name: "主题", exact: true })
        .selectOption(theme);
      await page
        .getByRole("slider", { name: "界面字号", exact: true })
        .fill("16");
      await page.getByRole("button", { name: "完成", exact: true }).click();
      await page.waitForFunction(
        async () => !!(await window.frontendTest.call("inspect")).visible,
      );
      assert.equal(
        await page.locator("html").getAttribute("data-theme"),
        theme,
      );
    }
    results.push({ scale, before, after });
    console.log(
      "DPI and resizing passed:",
      scale,
      before.dpr,
      after.native.width,
    );
  } finally {
    await application.close();
  }
}
fs.writeFileSync(
  path.join(build, "dpi-smoke.json"),
  JSON.stringify(
    {
      method:
        "Chromium forced device scale factor (OS display settings unchanged)",
      results,
    },
    null,
    2,
  ),
);
