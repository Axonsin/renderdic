# 原型验证记录

日期：2026-11-09。分支：`feature/ts_frontend`，核心基线 `6ff05d6b2d837f57d2e4073d54e8d06bc074863b`。新增内容均位于 `ts_frontend`。

## 环境与交付

- Windows 11 x64，RTX 5080；MSVC v145 / 14.51.36231，Windows SDK 10.0.28000.0。
- Node 24.18.0，pnpm 12.18.0；Electron 44.7.0，React 19.2.4，TypeScript 6.0.2。
- 自定义 RenderDoc 1.46；standalone Python 绑定与核心均重新编译，运行时 Python 3.6.4。
- 便携入口：`../dist/RenderDicTS-win32-x64/RenderDicTS.exe`。完整目录为运行依赖，不是单文件 exe。
- 构建清单：便携目录中的 `resources/runtime/build-manifest.json`，包含核心提交、依赖锁文件哈希及二进制哈希。

## 已通过

| 检查 | 结果与证据 |
|---|---|
| 类型与单元检查 | `pnpm test`：2 个测试文件、5 个测试通过；覆盖事件树过滤/折叠、过时响应和缺失 Python DLL 时阻止进程启动 |
| 真实回放 | D3D11、D3D12、OpenGL 的事件树、管线和 Shader 获取通过；数据分别含 5/5/3 个事件和 3/4/4 个纹理 |
| 并发一致性 | 连续选择事件后，最终控制器停留在最新事件；旧抓帧 generation 和旧事件请求被拒绝 |
| 进程生命周期 | standalone 连续开关 10 次通过；便携版另有连续开关 10 次通过，最终无回放工作进程残留 |
| 原生呈现 | 真实 GPU swapchain 渲染到原生子 HWND；测试三角形屏幕中心像素为绿色，桌面合成截图可见纹理，显示不依赖读回 PNG |
| 窗口状态 | 便携版在三种 API 下的页签切换、外观弹窗、最小化/恢复、最大化/还原均通过 |
| DPI 与分栏 | Chromium device scale factor 1/1.25/1.5 下，原生尺寸与 CSS 尺寸乘 DPR 相符，拖动分栏后重新对齐；明暗主题与 16px 界面字号切换通过 |
| 恢复 | 无效文件打开失败可报告；主动终止回放工作进程后，界面报错，重新连接并恢复打开抓帧成功 |
| 实际桌面 | computer-use 观察到便携版中文工作台和真实原生三角形；文件选择器打开、取消和画面恢复通过 |
| 隔离性 | 根目录既有源码无 Git diff；旧 Development/Release 的 exe/DLL/PYD 由构建前后 SHA-256 比较保护；产物哈希另有最终复核 |

便携版完整集成测试未记录 renderer pageerror。截图见 `../artifacts/native-desktop.png`；原始测试 JSON 在 `../.build`，包括 `worker-smoke.json`、`dpi-smoke.json`、`packaged-smoke.json`。`visual-smoke.json` 是最终打包后的额外短流程，不覆盖完整集成测试记录。

## 本机耗时样本

以下为一次便携版测试中，从发送打开请求到页面及原生输出可用的测量，包含 IPC、回放、页面更新和诊断等待，不是纯 GPU 回放基准。

| 文件 | 耗时 |
|---|---:|
| D3D11 `d3d11_direct_frame41.rdc` | 923ms |
| D3D12 `d3d12_direct_frame24.rdc` | 909ms |
| OpenGL `gl_direct_frame3227.rdc` | 1398ms |
| Seafarer D3D12，74,065,349 字节 | 3958ms |

此前同机运行的 Seafarer 测量为 5469ms，结果会受缓存、窗口更新和机器负载影响。加载过程位于独立工作进程，界面与测试脚本可继续执行。

## 实现调整及保留项

1. **原生容器调整：** 直接把跨进程子 HWND 挂到 Chromium 下时，窗口句柄和 GPU 读回均正常，但实际呈现被合成层覆盖。最终由 Electron 主线程创建主窗口拥有的无边框、非激活附属容器，再将回放子 HWND 放入其中；无单独任务栏入口，不能自由拖动，位置和显隐由工作台管理。它仍是原生 GPU 显示，并非离屏图片方案。
2. **基线编译适配：** Android GLES fallback 在生成的 Windows 编译副本中加平台保护；仅排除存在链接问题的 `vk_handle_guard_tests.cpp` 嵌入式测试。不修改原始文件、公共接口或驱动实现。详情见 README。
3. **加载进度：** 使用无进度回调的 standalone 打开方式规避 Python 3.6 GIL 等待；界面展示阶段和不定进度动画。
4. **配置：** UI、日志及测试 profile 独立。核心按现有规则读取 `renderdic.conf`，不写入或迁移旧配置。

尚未完成：真实 Windows 系统 DPI 改动与跨屏移动、Vulkan 本地抓帧、远程/Android、Mesh，以及旧 GUI 的重新运行验收。没有兼容的“不支持本地回放”样例进行实测，该错误分支仅已实现；未运行整套 RenderDoc 核心单元测试。文件选择器“输入路径并确认”未纳入最终人工验收，文件路径打开已由便携版集成测试覆盖。

该交付是可运行的最小原型，完整功能迁移和上述实机验收仍属于后续开发。
