import { app, BrowserWindow, ipcMain, dialog } from "electron";
import path from "node:path";
import { ReplayClient } from "./client";
import type { Viewport } from "../contracts/services";
app.setPath(
  "userData",
  process.env.RENDERDIC_TEST === "1" && process.env.RENDERDIC_TEST_PROFILE
    ? process.env.RENDERDIC_TEST_PROFILE
    : path.join(app.getPath("appData"), "RenderDicTS"),
);
app.setAppLogsPath(path.join(app.getPath("userData"), "logs"));
app.setAppUserModelId("org.renderdic.tsfrontend");
let window: BrowserWindow;
let client: ReplayClient;
let started: Promise<any>;
let viewport: Viewport = {
  x: 0,
  y: 0,
  width: 1,
  height: 1,
  scale: 1,
  visible: false,
};
let parent = "";
let surface: string | null = null;
let nativeView: {
  create: (parent: string) => string;
  destroy: () => void;
  bounds: (rect: string) => void;
};
let closing = false;
function nativeBounds() {
  const visible =
    viewport.visible && !window.isMinimized() && window.isVisible();
  nativeView.bounds(
    JSON.stringify([
      viewport.x,
      viewport.y,
      viewport.width,
      viewport.height,
      viewport.scale,
      visible ? 1 : 0,
    ]),
  );
  return client.call("viewport", { ...viewport, parent, surface, visible });
}
async function startClient() {
  client = new ReplayClient(
    app.isPackaged
      ? path.join(process.resourcesPath, "runtime")
      : path.resolve(__dirname, "../runtime"),
    app.getPath("logs"),
  );
  client.on("notice", (n) => {
    if (!window.isDestroyed()) window.webContents.send("notice", n);
  });
  started = client.start();
  await started;
  await nativeBounds();
}
void app.whenReady().then(async () => {
  nativeView = require(
    path.join(
      app.isPackaged
        ? path.join(process.resourcesPath, "runtime")
        : path.resolve(__dirname, "../runtime"),
      "viewport.node",
    ),
  );
  window = new BrowserWindow({
    width: 1480,
    height: 940,
    minWidth: 1024,
    minHeight: 700,
    title: "RenderDic TS",
    backgroundColor: "#111315",
    titleBarStyle: "hidden",
    titleBarOverlay: { color: "#111315", symbolColor: "#dddddd", height: 42 },
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      additionalArguments:
        process.env.RENDERDIC_TEST === "1" ? ["--frontend-test"] : [],
    },
  });
  parent = window.getNativeWindowHandle().readBigUInt64LE().toString();
  window.webContents.setZoomFactor(1);
  window.webContents.setVisualZoomLevelLimits(1, 1);
  window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
  window.webContents.on("will-navigate", (event) => event.preventDefault());
  window.webContents.on("before-input-event", (event, input) => {
    if (input.control && ["+", "-", "=", "0"].includes(input.key))
      event.preventDefault();
    if (input.control && input.key.toLowerCase() === "o") {
      event.preventDefault();
      window.webContents.send("notice", { event: "openRequested" });
    }
  });
  ipcMain.handle("chooseCapture", async () => {
    await started;
    await client.call("viewport", { ...viewport, visible: false });
    try {
      const result = await dialog.showOpenDialog(window, {
        filters: [{ name: "RenderDoc capture", extensions: ["rdc"] }],
        properties: ["openFile"],
      });
      return result.canceled ? null : result.filePaths[0];
    } finally {
      await nativeBounds();
    }
  });
  ipcMain.handle("replay", async (_event, method: string, params: any) => {
    if (
      ![
        "openCapture",
        "closeCapture",
        "selectEvent",
        "getShader",
        "setTexture",
        "viewport",
      ].includes(method)
    )
      throw Error("Unsupported frontend operation");
    await started;
    if (method === "viewport") {
      if (
        !params ||
        !["x", "y", "width", "height"].every(
          (k) => Number.isFinite(params[k]) && Math.abs(params[k]) <= 32768,
        )
      )
        throw Error("Invalid viewport bounds");
      viewport = {
        x: params.x,
        y: params.y,
        width: Math.max(0, params.width),
        height: Math.max(0, params.height),
        scale: Number.isFinite(params.scale)
          ? Math.max(0.5, Math.min(4, params.scale))
          : 1,
        visible: !!params.visible,
      };
      return nativeBounds();
    }
    if (method === "openCapture") {
      await client.call("closeCapture");
      nativeView.destroy();
      surface = nativeView.create(parent);
      await nativeBounds();
      try {
        return await client.call(method, params);
      } catch (error) {
        nativeView.destroy();
        surface = null;
        throw error;
      }
    }
    if (method === "closeCapture") {
      const result = await client.call(method, params);
      nativeView.destroy();
      surface = null;
      return result;
    }
    return client.call(method, params);
  });
  ipcMain.handle("restart", async () => {
    await client.stop();
    nativeView.destroy();
    surface = null;
    await startClient();
  });
  ipcMain.handle("windowTheme", (_event, dark: boolean) =>
    window.setTitleBarOverlay({
      color: dark ? "#111315" : "#edf0f2",
      symbolColor: dark ? "#dddddd" : "#252b33",
    }),
  );
  if (process.env.RENDERDIC_TEST === "1") {
    ipcMain.handle("test:call", async (_e, method, params) => {
      await started;
      if (method === "kill") {
        client.killForTest();
        return;
      }
      if (method === "openUI") {
        window.webContents.send("notice", {
          event: "openPath",
          message: params.path,
        });
        return;
      }
      return client.call(method, params);
    });
    ipcMain.handle("test:window", async (_e, action) => {
      if (action === "minimize") window.minimize();
      if (action === "restore") window.restore();
      if (action === "maximize") window.maximize();
      if (action === "unmaximize") window.unmaximize();
      return window.getBounds();
    });
  }
  const syncBounds = () => {
    if (client && started)
      void started.then(() => nativeBounds()).catch(() => {});
  };
  window.on("minimize", syncBounds);
  window.on("restore", syncBounds);
  window.on("show", syncBounds);
  window.on("hide", syncBounds);
  window.on("resize", syncBounds);
  window.on("move", syncBounds);
  window.on("close", (event) => {
    if (!closing) {
      event.preventDefault();
      closing = true;
      void client?.stop().finally(() => window.destroy());
    }
  });
  void startClient().catch((error) =>
    window.webContents.send("notice", {
      event: "crashed",
      message: String(error),
    }),
  );
  const url = process.env.RENDERDIC_DEV_URL;
  if (url) await window.loadURL(url);
  else await window.loadFile(path.join(__dirname, "../../ui-dist/index.html"));
});
app.on("window-all-closed", () => app.quit());
