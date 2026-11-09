import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import Editor from "@monaco-editor/react";
import { useVirtualizer } from "@tanstack/react-virtual";
import * as Dialog from "@radix-ui/react-dialog";
import {
  Activity,
  ChevronDown,
  ChevronRight,
  Code2,
  FolderOpen,
  Image,
  Layers,
  LoaderCircle,
  Search,
  Settings2,
  X,
  RefreshCw,
  Check,
  Copy,
  Monitor,
} from "lucide-react";
import type { Capture, Selection, Shader } from "../contracts/services";
import { Revision, visibleEvents } from "../contracts/model";
import { useServices } from "./services";
import { useAppearance } from "./store";

export function App() {
  const api = useServices();
  const appearance = useAppearance();
  const [capture, setCapture] = useState<Capture | null>(null);
  const [selection, setSelection] = useState<Selection | null>(null);
  const [shader, setShader] = useState<Shader | null>(null);
  const [stage, setStage] = useState("VS");
  const [tab, setTab] = useState("texture");
  const [query, setQuery] = useState("");
  const [collapsed, setCollapsed] = useState(new Set<number>());
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState("");
  const [settings, setSettings] = useState(false);
  const [textureId, setTextureId] = useState("");
  const [fit, setFit] = useState(true);
  const [channels, setChannels] = useState([true, true, true, false]);
  const [systemDark, setSystemDark] = useState(
    matchMedia("(prefers-color-scheme:dark)").matches,
  );
  const revision = useRef(new Revision());
  const textureRevision = useRef(new Revision());
  const lastPath = useRef<string | null>(null);
  const currentCapture = useRef(capture);
  currentCapture.current = capture;
  const currentSelection = useRef(selection);
  currentSelection.current = selection;
  const eventsRef = useRef<HTMLDivElement>(null);
  const viewportRef = useRef<HTMLDivElement>(null);
  const dark =
    appearance.theme === "system" ? systemDark : appearance.theme === "dark";
  const rows = useMemo(
    () => visibleEvents(capture?.events ?? [], query, collapsed),
    [capture, query, collapsed],
  );
  const virtual = useVirtualizer({
    count: rows.length,
    getScrollElement: () => eventsRef.current,
    estimateSize: () => 34,
    overscan: 12,
  });
  useEffect(() => {
    const index = rows.findIndex((row) => row.eventId === selection?.eventId);
    if (index >= 0) virtual.scrollToIndex(index, { align: "center" });
  }, [rows, selection?.eventId, virtual]);
  const selectedTexture = capture?.textures.find((t) => t.id === textureId);
  const report = useCallback((reason: unknown) => {
    if ((reason as { code?: string })?.code !== "SUPERSEDED")
      setError(String(reason).replace(/^Error: /, ""));
  }, []);
  const applySelection = useCallback((result: Selection) => {
    setSelection(result);
    setTextureId(result.textureId ?? "");
    setShader(null);
    setStage((previous) =>
      result.stages.some((s) => s.stage === previous)
        ? previous
        : (result.stages[0]?.stage ?? "VS"),
    );
    setFit(true);
    setChannels([true, true, true, false]);
  }, []);
  const open = useCallback(
    async (path: string) => {
      const ticket = revision.current.next();
      lastPath.current = path;
      setBusy(true);
      setProgress(0);
      setError("");
      setCapture(null);
      setSelection(null);
      setShader(null);
      setTextureId("");
      try {
        const result = await api.openCapture(path);
        if (revision.current.isCurrent(ticket)) {
          setCapture(result);
          applySelection(result.initial);
          setCollapsed(new Set());
          setQuery("");
        }
      } catch (e) {
        if (revision.current.isCurrent(ticket)) report(e);
      } finally {
        if (revision.current.isCurrent(ticket)) setBusy(false);
      }
    },
    [api, applySelection, report],
  );
  const choose = useCallback(async () => {
    try {
      const path = await api.chooseCapture();
      if (path) await open(path);
    } catch (e) {
      report(e);
    }
  }, [api, open, report]);
  async function close() {
    revision.current.next();
    setCapture(null);
    setSelection(null);
    setShader(null);
    setTextureId("");
    setBusy(false);
    try {
      await api.closeCapture();
    } catch (e) {
      report(e);
    }
  }
  async function select(eventId: number) {
    if (!capture) return;
    const ticket = revision.current.next();
    setBusy(true);
    setError("");
    setShader(null);
    try {
      const result = await api.selectEvent(eventId, capture.generation);
      if (revision.current.isCurrent(ticket)) applySelection(result);
    } catch (e) {
      if (revision.current.isCurrent(ticket)) report(e);
    } finally {
      if (revision.current.isCurrent(ticket)) setBusy(false);
    }
  }
  async function updateTexture(
    id = textureId,
    newFit = fit,
    newChannels = channels,
  ) {
    if (!capture || !id || !selection) return;
    const generation = capture.generation;
    const event = selection.eventId;
    const ticket = textureRevision.current.next();
    const current = () =>
      currentCapture.current?.generation === generation &&
      currentSelection.current?.eventId === event &&
      textureRevision.current.isCurrent(ticket);
    try {
      await api.setTexture(id, generation, event, newFit, newChannels);
      if (current()) {
        setTextureId(id);
        setFit(newFit);
        setChannels(newChannels);
      }
    } catch (e) {
      if (current()) report(e);
    }
  }
  useEffect(
    () =>
      api.onNotice((notice) => {
        if (notice.event === "progress") setProgress(notice.value ?? 0);
        if (notice.event === "openRequested") void choose();
        if (notice.event === "openPath" && notice.message)
          void open(notice.message);
        if (notice.event === "crashed") {
          revision.current.next();
          setCapture(null);
          setSelection(null);
          setShader(null);
          setBusy(false);
          setError(notice.message ?? "回放进程已退出");
        }
      }),
    [api, choose, open],
  );
  useEffect(() => {
    const media = matchMedia("(prefers-color-scheme:dark)");
    const listener = () => setSystemDark(media.matches);
    media.addEventListener("change", listener);
    return () => media.removeEventListener("change", listener);
  }, []);
  useEffect(() => {
    document.documentElement.dataset.theme = dark ? "dark" : "light";
    document.documentElement.style.setProperty(
      "--ui-font-size",
      appearance.fontSize + "px",
    );
    void api.setWindowTheme(dark);
  }, [api, dark, appearance.fontSize]);
  useEffect(() => {
    if (
      tab !== "shader" ||
      !capture ||
      !selection?.stages.some((s) => s.stage === stage)
    )
      return;
    let stale = false;
    setShader(null);
    api
      .getShader(stage, capture.generation, selection.eventId)
      .then((result) => {
        if (
          !stale &&
          result.eventId === selection.eventId &&
          result.generation === capture.generation
        )
          setShader(result);
      })
      .catch((e) => {
        if (!stale) report(e);
      });
    return () => {
      stale = true;
    };
  }, [api, tab, capture, selection, stage, report]);
  useLayoutEffect(() => {
    const element = viewportRef.current;
    if (!element) return;
    let frame = 0;
    const update = () => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(() => {
        const r = element.getBoundingClientRect();
        void api
          .viewport({
            x: r.x,
            y: r.y,
            width: r.width,
            height: r.height,
            scale: devicePixelRatio,
            visible:
              !!capture &&
              !!textureId &&
              tab === "texture" &&
              !settings &&
              r.width > 0 &&
              r.height > 0,
          })
          .catch(report);
      });
    };
    const observer = new ResizeObserver(update);
    observer.observe(element);
    window.addEventListener("resize", update);
    update();
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      window.removeEventListener("resize", update);
    };
  }, [
    api,
    capture,
    textureId,
    tab,
    settings,
    appearance.left,
    appearance.right,
    report,
  ]);
  function drag(side: "left" | "right", event: React.PointerEvent) {
    const initial = event.clientX;
    const width = appearance[side];
    event.currentTarget.setPointerCapture(event.pointerId);
    const target = event.currentTarget;
    const move = (e: Event) => {
      const x = (e as PointerEvent).clientX;
      appearance.set({
        [side]: Math.max(
          220,
          Math.min(500, width + (side === "left" ? x - initial : initial - x)),
        ),
      });
    };
    const end = () => {
      target.removeEventListener("pointermove", move);
      target.removeEventListener("pointerup", end);
      target.removeEventListener("pointercancel", end);
    };
    target.addEventListener("pointermove", move);
    target.addEventListener("pointerup", end);
    target.addEventListener("pointercancel", end);
  }
  async function reconnect() {
    setError("");
    setBusy(true);
    try {
      await api.restart();
      if (lastPath.current) await open(lastPath.current);
    } catch (e) {
      report(e);
    } finally {
      setBusy(false);
    }
  }
  return (
    <div className="app">
      <header className="titlebar">
        <span className="logo">
          <Activity size={19} />
        </span>
        <strong>RenderDic</strong>
        <span className="edition">WORKSPACE</span>
        <span className="title-path" title={capture?.path}>
          {capture ? capture.path.split(/[\\/]/).at(-1) : "图形帧分析工作台"}
        </span>
      </header>
      <nav className="toolbar">
        <button
          className="primary"
          onClick={() => void choose()}
          disabled={busy}
        >
          <FolderOpen size={16} />
          打开抓帧 <kbd>Ctrl O</kbd>
        </button>
        <button disabled={!capture || busy} onClick={() => void close()}>
          <X size={15} />
          关闭
        </button>
        <div className="spacer" />
        <span className="api-tag">{capture?.api ?? "LOCAL REPLAY"}</span>
        <button aria-label="外观设置" onClick={() => setSettings(true)}>
          <Settings2 size={17} />
          <span>外观</span>
        </button>
      </nav>
      {error && (
        <div className="error" role="alert">
          <span>{error}</span>
          <button onClick={() => void reconnect()}>
            <RefreshCw size={14} />
            重新连接
          </button>
          <button aria-label="关闭错误提示" onClick={() => setError("")}>
            <X size={14} />
          </button>
        </div>
      )}
      <main
        className="workspace"
        style={{
          gridTemplateColumns: `${appearance.left}px 5px minmax(240px,1fr) 5px ${appearance.right}px`,
        }}
      >
        <section className="panel events">
          <div className="panel-heading">
            <Layers size={15} />
            <strong>事件浏览器</strong>
            <span className="counter">{capture?.events.length ?? 0}</span>
          </div>
          <label className="search">
            <Search size={15} />
            <input
              aria-label="搜索事件"
              placeholder="搜索事件、编号…"
              value={query}
              onChange={(e) => setQuery(e.target.value)}
            />
          </label>
          <div className="event-column">
            <span>EID</span>
            <span>事件</span>
          </div>
          <div
            className="event-scroll"
            ref={eventsRef}
            role="tree"
            aria-label="事件树"
          >
            <div
              style={{ height: virtual.getTotalSize(), position: "relative" }}
            >
              {virtual.getVirtualItems().map((item) => {
                const row = rows[item.index];
                return (
                  <div
                    key={row.eventId}
                    className={`event-row ${row.eventId === selection?.eventId ? "selected" : ""}`}
                    role="treeitem"
                    aria-level={row.depth + 1}
                    aria-selected={row.eventId === selection?.eventId}
                    aria-expanded={
                      row.children ? !collapsed.has(row.eventId) : undefined
                    }
                    style={{
                      height: item.size,
                      transform: `translateY(${item.start}px)`,
                    }}
                  >
                    <span className="eid">{row.eventId}</span>
                    <button
                      className="event-name"
                      title={row.name}
                      onClick={() => void select(row.eventId)}
                      style={{ paddingLeft: row.depth * 12 }}
                    >
                      {row.children ? (
                        <span
                          className="chevron"
                          onClick={(e) => {
                            e.stopPropagation();
                            setCollapsed((previous) => {
                              const next = new Set(previous);
                              if (next.has(row.eventId))
                                next.delete(row.eventId);
                              else next.add(row.eventId);
                              return next;
                            });
                          }}
                        >
                          {collapsed.has(row.eventId) ? (
                            <ChevronRight size={13} />
                          ) : (
                            <ChevronDown size={13} />
                          )}
                        </span>
                      ) : (
                        <span
                          className={`event-dot ${row.draw ? "draw" : ""}`}
                        />
                      )}
                      <span>{row.name}</span>
                    </button>
                  </div>
                );
              })}
            </div>
            {!capture && (
              <div className="side-empty">打开抓帧后查看事件序列</div>
            )}
          </div>
        </section>
        <div
          role="separator"
          aria-label="调整事件栏宽度"
          aria-orientation="vertical"
          className="resize-handle"
          onPointerDown={(e) => drag("left", e)}
        />
        <section className="panel center">
          <div className="tabbar">
            <button
              className={tab === "texture" ? "active" : ""}
              onClick={() => setTab("texture")}
            >
              <Image size={15} />
              纹理
            </button>
            <button
              className={tab === "shader" ? "active" : ""}
              onClick={() => setTab("shader")}
            >
              <Code2 size={15} />
              Shader
            </button>
            <div className="spacer" />
            {selection && (
              <span className="muted">EID {selection.eventId}</span>
            )}
          </div>
          <div className="content-toolbar">
            {tab === "texture" ? (
              <>
                <select
                  aria-label="选择纹理"
                  disabled={!capture}
                  value={textureId}
                  onChange={(e) => void updateTexture(e.target.value)}
                >
                  <option value="">选择纹理资源</option>
                  {capture?.textures.map((t) => (
                    <option value={t.id} key={t.id}>
                      {t.name} · {t.width} × {t.height}
                    </option>
                  ))}
                </select>
                <button
                  className={fit ? "pressed" : ""}
                  disabled={!textureId}
                  onClick={() => void updateTexture(textureId, !fit)}
                >
                  {fit ? "适应窗口" : "100%"}
                </button>
                {["R", "G", "B", "A"].map((label, index) => (
                  <button
                    key={label}
                    className={`channel ${channels[index] ? "pressed" : ""}`}
                    disabled={!textureId}
                    aria-label={`${label} 通道`}
                    aria-pressed={channels[index]}
                    onClick={() =>
                      void updateTexture(
                        textureId,
                        fit,
                        channels.map((v, i) => (i === index ? !v : v)),
                      )
                    }
                  >
                    {label}
                  </button>
                ))}
              </>
            ) : (
              <>
                <select
                  aria-label="Shader 阶段"
                  value={stage}
                  onChange={(e) => setStage(e.target.value)}
                >
                  {selection?.stages.map((s) => (
                    <option key={s.stage} value={s.stage}>
                      {s.stage} · {s.name}
                    </option>
                  ))}
                </select>
                <span className="muted">
                  {shader?.kind === "source"
                    ? "源代码"
                    : shader?.kind === "disassembly"
                      ? "反汇编"
                      : ""}
                </span>
                <button
                  disabled={!shader?.text}
                  aria-label="复制 Shader"
                  onClick={() =>
                    void navigator.clipboard.writeText(shader?.text ?? "")
                  }
                >
                  <Copy size={14} />
                </button>
              </>
            )}
          </div>
          <div className="viewer">
            <div
              ref={viewportRef}
              className="native-viewport"
              data-testid="viewport"
              style={{ visibility: tab === "texture" ? "visible" : "hidden" }}
            />
            {tab === "shader" && (
              <div className="shader-view">
                {shader?.text ? (
                  <Editor
                    height="100%"
                    language={shader.kind === "source" ? "cpp" : "plaintext"}
                    theme={dark ? "vs-dark" : "light"}
                    value={shader.text}
                    options={{
                      readOnly: true,
                      fontSize: appearance.codeSize,
                      fontFamily: "Consolas, Microsoft YaHei UI, monospace",
                      lineHeight: Math.round(appearance.codeSize * 1.6),
                      minimap: { enabled: false },
                      scrollBeyondLastLine: false,
                      automaticLayout: true,
                      wordWrap: "off",
                      renderLineHighlight: "none",
                      padding: { top: 16 },
                      contextmenu: false,
                    }}
                  />
                ) : (
                  <div className="empty">
                    <Code2 size={30} />
                    <h2>
                      {capture
                        ? "当前事件没有可显示的 Shader"
                        : "Shader 查看器"}
                    </h2>
                    <p>选择绘制事件后查看阶段源码或反汇编</p>
                  </div>
                )}
              </div>
            )}
            {tab === "texture" && capture && !textureId && (
              <div className="empty">
                <Image size={30} />
                <h2>当前事件没有颜色输出</h2>
                <p>可以从上方资源列表选择其他纹理</p>
              </div>
            )}
            {tab === "texture" && !capture && (
              <div className="empty">
                <div className="hero-icon">
                  <Layers size={38} />
                </div>
                <h1>从一帧开始，理解每一次绘制</h1>
                <p>打开本地 RDC，浏览事件、检查纹理与着色器。</p>
                <button
                  className="primary large"
                  disabled={busy}
                  onClick={() => void choose()}
                >
                  <FolderOpen size={17} />
                  打开抓帧文件
                </button>
                <div className="welcome-tags">
                  <span>D3D11</span>
                  <span>D3D12</span>
                  <span>OpenGL</span>
                  <span>本地回放</span>
                </div>
              </div>
            )}
            {busy && !capture && (
              <div className="loading">
                <LoaderCircle className="spin" size={25} />
                <strong>正在加载抓帧</strong>
                <span>{progress < 1 ? "初始化回放…" : "读取资源…"}</span>
                <progress />
              </div>
            )}
          </div>
          <div className="viewer-footer">
            <span>
              {tab === "texture"
                ? selectedTexture
                  ? `${selectedTexture.width} × ${selectedTexture.height}  ·  ${selectedTexture.format}`
                  : "未选择纹理"
                : shader
                  ? `${shader.text.split("\n").length} 行`
                  : "只读视图"}
            </span>
            <div className="spacer" />
            <span>
              {selectedTexture && tab === "texture" ? selectedTexture.id : ""}
            </span>
          </div>
        </section>
        <div
          role="separator"
          aria-label="调整管线栏宽度"
          aria-orientation="vertical"
          className="resize-handle"
          onPointerDown={(e) => drag("right", e)}
        />
        <aside className="panel pipeline">
          <div className="panel-heading">
            <Activity size={15} />
            <strong>管线概览</strong>
          </div>
          {selection ? (
            <div className="pipeline-content">
              <div className="eyebrow">当前事件</div>
              <h2 title={selection.name}>{selection.name}</h2>
              <div className="facts">
                <span>事件编号</span>
                <code>{selection.eventId}</code>
                <span>索引 / 顶点数</span>
                <code>{selection.numIndices}</code>
                <span>实例数</span>
                <code>{selection.numInstances}</code>
              </div>
              <div className="section-title">
                着色器阶段 <span>{selection.stages.length}</span>
              </div>
              {selection.stages.map((s) => (
                <button
                  className="stage-card"
                  key={s.stage}
                  onClick={() => {
                    setStage(s.stage);
                    setTab("shader");
                  }}
                >
                  <span className="stage-tag">{s.stage}</span>
                  <span title={s.name}>{s.name}</span>
                  <ChevronRight size={14} />
                </button>
              ))}
              {!selection.stages.length && (
                <p className="muted">当前事件没有绑定 Shader</p>
              )}
              <div className="section-title">
                颜色输出 <span>{selection.outputs.length}</span>
              </div>
              {selection.outputs.map((t, i) => (
                <button
                  className="resource-card"
                  key={`${i}-${t.id}`}
                  onClick={() => {
                    setTab("texture");
                    void updateTexture(t.id);
                  }}
                >
                  <div>
                    <Image size={15} />
                    <span title={t.name}>{t.name}</span>
                  </div>
                  <small>
                    {t.width} × {t.height} · {t.format}
                  </small>
                </button>
              ))}
              {!selection.outputs.length && (
                <p className="muted">没有颜色输出</p>
              )}
              {selection.depth && (
                <>
                  <div className="section-title">深度输出</div>
                  <button
                    className="resource-card"
                    onClick={() => {
                      setTab("texture");
                      void updateTexture(selection.depth!.id);
                    }}
                  >
                    <div>
                      <Layers size={15} />
                      <span title={selection.depth.name}>
                        {selection.depth.name}
                      </span>
                    </div>
                    <small>{selection.depth.format}</small>
                  </button>
                </>
              )}
            </div>
          ) : (
            <div className="side-empty">
              <Monitor size={25} />
              <p>
                选择一个事件
                <br />
                查看绑定状态与输出资源
              </p>
            </div>
          )}
        </aside>
      </main>
      <footer className="statusbar">
        <span className={`status-dot ${busy ? "busy" : ""}`} />
        <span data-testid="status">
          {busy ? "正在处理…" : capture ? "抓帧已加载" : "准备就绪"}
        </span>
        <div className="spacer" />
        {capture && <span>加载 {(capture.elapsedMs / 1000).toFixed(2)}s</span>}
        {selection && <span>事件 {selection.elapsedMs}ms</span>}
        <span>本地工作区</span>
      </footer>
      <Dialog.Root open={settings} onOpenChange={setSettings}>
        <Dialog.Portal>
          <Dialog.Overlay className="dialog-overlay" />
          <Dialog.Content className="dialog">
            <Dialog.Title>外观设置</Dialog.Title>
            <Dialog.Description>调整工作台的阅读体验</Dialog.Description>
            <label>
              主题
              <select
                aria-label="主题"
                value={appearance.theme}
                onChange={(e) =>
                  appearance.set({
                    theme: e.target.value as typeof appearance.theme,
                  })
                }
              >
                <option value="system">跟随系统</option>
                <option value="dark">深色</option>
                <option value="light">浅色</option>
              </select>
            </label>
            <label>
              界面字号 <span>{appearance.fontSize}px</span>
              <input
                aria-label="界面字号"
                type="range"
                min={12}
                max={18}
                value={appearance.fontSize}
                onChange={(e) => appearance.set({ fontSize: +e.target.value })}
              />
            </label>
            <label>
              代码字号 <span>{appearance.codeSize}px</span>
              <input
                aria-label="代码字号"
                type="range"
                min={12}
                max={22}
                value={appearance.codeSize}
                onChange={(e) => appearance.set({ codeSize: +e.target.value })}
              />
            </label>
            <Dialog.Close asChild>
              <button className="primary">
                <Check size={15} />
                完成
              </button>
            </Dialog.Close>
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>
    </div>
  );
}
