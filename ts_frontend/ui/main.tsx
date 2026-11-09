import React from "react";
import { createRoot } from "react-dom/client";
import { loader } from "@monaco-editor/react";
import * as monaco from "monaco-editor";
import EditorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";
import { App } from "./App";
import { ServiceProvider } from "./services";
import "./styles.css";
// Monaco is entirely local: no CDN, network request or capture contents leave this process.
globalThis.MonacoEnvironment = { getWorker: () => new EditorWorker() };
loader.config({ monaco });
createRoot(document.getElementById("root")!).render(
  <ServiceProvider services={window.renderdic}>
    <App />
  </ServiceProvider>,
);
