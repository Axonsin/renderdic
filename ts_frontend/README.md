# RenderDic TS prototype

独立的 Windows x64 分析前端，参考 ZCode 的服务分层与视觉设计。旧 Qt/PySide UI、公共接口及原有项目文件不变。所有新增实现与构建适配均位于本目录。

## 启动

便携程序：`dist/RenderDicTS-win32-x64/RenderDicTS.exe`。保留整个目录，不能只复制 exe；不需要系统 Python、Node 或 Qt。

当前支持本地 RDC、可搜索事件树、管线摘要、Shader 源码/反汇编、原生 GPU Texture、RGBA、适应窗口/原始比例、明暗主题和独立字号。`Ctrl+O` 打开抓帧。尚不包含 Mesh、远程设备、抓帧和 Shader 调试。

## 开发与构建

环境：Windows 11 x64，Node 24、pnpm 12、Visual Studio 18 C++ v145、Windows SDK，以及仓库已有的 Python 3.6、SWIG 和第三方依赖。

在本目录运行：

```powershell
pnpm install --ignore-scripts
node node_modules/electron/install.js
pnpm build:prototype
pnpm dev
```

`build:prototype` 重新编译核心及 standalone Python 模块、编译回放启动器/原生窗口模块、编译界面并生成便携目录。只构建 Development|x64，不构建旧 GUI。首次编译较慢；之后增量编译。核心编译日志在 `.build/backend-build.log`。

`dev` 使用本机 Vite，React/CSS 热更新。修改 desktop 代码需重启 `dev`；修改 native/bridge 代码需关闭新前端，再执行 `pnpm build:worker`。桌面进程必须退出后才能替换 Windows 正在加载的 DLL。

## 目录职责

| 目录 | 职责 |
|---|---|
| `contracts` | 浏览器可用的服务类型、事件树转换、异步版本检查 |
| `ui` | React、Zustand 外观设置、虚拟事件树、Monaco、主题 |
| `desktop` | Electron 装配、有限 preload 接口、进程代理和窗口生命周期 |
| `bridge` | Python 3.6 单线程回放、资源投影、请求/响应/通知 |
| `native` | 嵌入式 Python 启动器、稳定 Node-API 原生窗口模块、原生窗口诊断 |
| `scripts` | 输出隔离、编译、便携打包及版本/哈希清单 |
| `tests` | 单元、真实回放、Electron 与 DPI 测试 |
| `.build`, `dist`, `ui-dist` | 被局部忽略的生成物 |

## 原生显示的实际结构

```text
Electron 主窗口
  ├─ React 工作台
  └─ 主窗口拥有的无边框附属容器（非激活、无任务栏入口）
       └─ 原生子 HWND ← 工作进程中的 RenderDoc GPU swapchain
```

普通 Chromium 子窗口路径在本机出现了“GPU 数据正确、桌面画面被合成层覆盖”的情况。因此原型采用 owned container 建立独立 DWM 合成目标，原生子 HWND 仍被约束在预览区域，位置、尺寸、页签、弹窗和最小化均由主窗口控制。这是相对于最初“直接挂在 Electron HWND 下”的实现调整；不是离屏图片显示，也不是可自由拖动的独立预览工具窗。

窗口在 Electron 主线程通过 `viewport.node` 创建和销毁，RenderDoc 控制器及输出由独立 Python 进程的同一线程管理。关闭时先释放回放输出，再销毁原生窗口；每次打开创建新窗口，避免 D3D12 的窗口复用限制。工作进程监测父进程退出，避免残留。

界面保持浏览器缩放 100%，通过字号变量调整文字。原生桥统一将 CSS 坐标按 devicePixelRatio 转换为物理像素，工作线程匹配窗口 DPI awareness。当前可见纹理以最多约 10Hz 刷新，用于恢复窗口遮挡后的呈现；后续可改为更精细的失效通知。

## 通信和状态

Renderer 只调用注入的服务；preload 不暴露通用 IPC 或 Node。Electron 用独立 stdio 与工作进程通信，消息具有请求编号和明确错误；句柄与资源 ID 使用字符串，不跨进程传递 SWIG 对象。`generation` 隔离不同抓帧，事件检查与 UI revision 防止过时结果覆盖新选择。

工作进程仅 import `renderdoc`，不使用 `qrenderdoc` 或旧 GUI 进程。保留 Python 3.6 ABI，不升级原 UI 的运行环境。运行库来自同次隔离构建，便携目录内有源码提交、依赖锁文件及二进制 SHA-256 清单。

standalone Python 3.6 的跨线程加载进度回调在本机触发过 GIL 等待；当前使用官方支持的无回调打开方式，UI 显示加载阶段，不承诺精确百分比。

## 基线的两项构建适配

1. `gl_hooks.cpp` 的 Android GLES fallback 缺少平台保护，Windows 无法编译。构建脚本只在 `.build/backend/generated` 中生成带 `RDOC_ANDROID` 保护的副本，并只替换本次 Windows 编译输入；原文件不改。
2. 基线的 `vk_handle_guard_tests.cpp` 缺少 `DoStringise<Reason>`，导致 Development 链接失败。独立构建只排除这个嵌入式 Catch 测试翻译单元，不排除 Vulkan 驱动代码。这里没有声称原核心单元测试全部通过。

上述适配在 `backend-output.props` 中集中管理。合并上游后若这些问题已修复，应移除对应适配。生成副本带源文件哈希，匹配逻辑变化会停止构建，要求重新检查。

不修改根解决方案：通过 MSBuild 的 `ForceImportBeforeCppTargets` 重定向每个项目的 OutDir/IntDir，原源码路径和 ProjectReference 保留。构建前校验输出路径，构建后比对旧 exe/DLL/PYD 哈希。

## 验证

```powershell
pnpm test
pnpm test:smoke
pnpm test:e2e
node tests/dpi-smoke.mjs
node tests/electron-smoke.mjs --packaged
```

Electron 测试会短暂显示/聚焦原型窗口。DPI 测试使用 Chromium 的 1/1.25/1.5 device scale factor，不修改 Windows 显示设置；真实系统 DPI 与跨屏移动仍需额外实机验收。

原生验证除 HWND、尺寸及回放读回检查外，还检查测试三角形在真实屏幕中心的绿色像素，并生成桌面合成截图；不能仅凭 DOM 截图或 GPU 读回判断呈现成功。诊断 PNG 仅用于检查，不参与前端显示。

测试 JSON、日志与截图位于 `.build`，人工复核结论见 `docs/VALIDATION.md`。RDC 使用本机现有文件，未复制进源码仓库。测试模式的诊断接口只在显式 `RENDERDIC_TEST=1` 启动时开放。

## 配置与后续开发

界面外观与日志位于独立的 `%APPDATA%/RenderDicTS`；自动测试使用 `.build` 下的独立 profile。核心仍按照原实现读取现有 `renderdic.conf`，新前端不调用保存核心配置的 API；不会迁移或覆写旧 UI 设置。

下一阶段优先做真实系统 DPI/多屏、原生容器焦点和遮挡压力测试，再扩展像素拾取、Mip/Slice、Mesh 及远程回放。完整停靠布局、性能优化和复杂 Shader 调试不属于本次原型。

## 参考

借鉴 [ZCode](https://github.com/zai-org/ZCode) 的 UI/服务分离、依赖注入及设计变量规则，未整体复制其 Agent、账号、远程服务或工作区业务代码。RenderDoc 与 Python 的许可证随便携运行库分发，Electron/Chromium 许可随 Electron 包分发；React、Monaco、Radix 等依赖版本由本目录锁文件固定。
