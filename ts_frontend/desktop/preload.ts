import { contextBridge, ipcRenderer } from "electron";
import type { Notice, Services } from "../contracts/services";
const call = (method: string, params: unknown = {}) =>
  ipcRenderer.invoke("replay", method, params);
const services: Services = {
  chooseCapture: () => ipcRenderer.invoke("chooseCapture"),
  openCapture: (path) => call("openCapture", { path }),
  closeCapture: () => call("closeCapture"),
  selectEvent: (eventId, generation) =>
    call("selectEvent", { eventId, generation }),
  getShader: (stage, generation, eventId) =>
    call("getShader", { stage, generation, eventId }),
  setTexture: (textureId, generation, eventId, fit, channels) =>
    call("setTexture", { textureId, generation, eventId, fit, channels }),
  viewport: (bounds) => call("viewport", bounds),
  setWindowTheme: (dark) => ipcRenderer.invoke("windowTheme", dark),
  restart: () => ipcRenderer.invoke("restart"),
  onNotice: (callback) => {
    const listener = (_e: unknown, notice: Notice) => callback(notice);
    ipcRenderer.on("notice", listener);
    return () => ipcRenderer.removeListener("notice", listener);
  },
};
contextBridge.exposeInMainWorld("renderdic", services);
if (process.argv.includes("--frontend-test"))
  contextBridge.exposeInMainWorld("frontendTest", {
    call: (method: string, params: unknown = {}) =>
      ipcRenderer.invoke("test:call", method, params),
    window: (action: string) => ipcRenderer.invoke("test:window", action),
  });
