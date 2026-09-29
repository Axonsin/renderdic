# Android Vulkan 句柄销毁防护

本次修改只作用于 Android 捕获层。游戏设置、游戏二进制、帧生成 SDK、公共
`WrappingPool` 语义和 RDC 格式均未修改。它防止 SDK 初始化失败后的错误清理进入驱动，
不会让失败的 SDK 初始化成功，也不增加 optical-flow 或扩展捕获支持。

## 实现

- `vk_handle_guard.h/.cpp`：Vulkan 专用注册表。非分派句柄采用 8 位标记、32 位代际、
  24 位槽位的不透明身份；1024 项一页，索引解析，不扫描 wrapper 池。代际耗尽永久退役槽位。
- `vk_resources.h`、`vk_manager.h/.cpp`：统一转换、注册、描述符复用、父子关联和注销。
  所属 Instance/Device/Pool 使用生命周期身份；在查明身份之前不解引用应用传入的地址。
- `wrappers`：单对象销毁、内存释放、Surface、整批释放、池重置/trim、QueryPool reset，
  以及名称、标签、private-data 的句柄转换。DebugReport、DebugUtils、PrivateDataSlot、
  DeferredOperation 和 ValidationCache 特殊对象同样纳入身份管理。
- `vk_core.cpp`：捕获结束和丢弃使用可信内部入口排空延迟释放。首次公开销毁即占用销毁权，
  后续请求被拒绝；内部排空允许 Pending 状态。注册表锁不跨驱动调用。

非法批量输入整批拒绝，包含重复项、错设备、错父池及旧代际。带结果的拒绝返回
`VK_ERROR_VALIDATION_FAILED_EXT`。实际驱动 free/reset 失败时撤销 claim，不提前修改账本。
DescriptorPool reset 成功使旧 DescriptorSet 不再有销毁资格，复用 record 时生成新身份；
CommandPool reset 保留 CommandBuffer 身份。借用的 swapchain image 由父对象释放。

## 模式及回退

`debug.rdoc.foreigndestroy` 在进程内首次初始化捕获管理器时读取并缓存，修改后须重启进程：

| 属性值 | 行为 |
| --- | --- |
| 未设置、`0`、任何其他值 | 严格模式，未知销毁句柄拒绝 |
| `1` | 仅恢复原单对象守卫中符合旧地址形态判断的未知裸句柄转发 |

兼容模式仍拒绝本层身份、旧代际、wrapper 池地址、错类型/归属和非法批量输入。
启用时输出一次风险提示。`[HANDLE-GUARD]` 日志按类型/原因仅打印前四次，
捕获结束、丢弃和管理器清理时打印累计计数及注册表大小。

```sh
adb shell setprop debug.rdoc.foreigndestroy 0
# 随后重启目标应用；默认不需要设置此属性。
```

兼容开关不是完整回滚。完整回退应替换回备份 SO，并重启应用。

## 边界

入口 Instance/Device 必须有效，批量数组内存必须可读。任意内存破坏、错误值恰好等于
另一个当前合法身份，不能仅凭销毁 API 判断。分派句柄遵守 loader 布局，仍然是 wrapper
指针，同地址复用无法彻底排除 ABA。正常资源使用要求应用遵守 Vulkan 的同步和生命周期规则。

普通 wrapper 注册失败不回退裸句柄：尽可能先清理本次新建驱动对象，再沿用 RenderDoc
现有致命 host-OOM 策略终止进程。借用对象不单独释放；禁止单独 free 的描述符集合依赖
父池/进程清理。这不是所有创建接口都可恢复的 OOM 改造。特殊扩展注册失败清理对象并返回
`VK_ERROR_OUT_OF_HOST_MEMORY`。注册表按使用峰值保留页，空闲槽位复用，不随销毁次数累积墓碑。

## 可重复测试

独立测试不链接到捕获 SO：

```sh
cmake -S renderdoc/driver/vulkan/handle_guard_tests -B build-guard -DCMAKE_BUILD_TYPE=Release
cmake --build build-guard
ctest --test-dir build-guard --output-on-failure
./build-guard/benchmark
```

Android 使用 NDK 的 `android.toolchain.cmake` 配置同一测试目录，设置
`ANDROID_ABI=arm64-v8a`、`ANDROID_PLATFORM=android-23`、`ANDROID_STL=c++_static`。
额外生成 `integration`，通过实际 SO 入口访问 Vulkan。SO 必须保留标准 basename，
避免已有 Android dlopen hook 把另一个名字当成外来模块重新装载自身：

```sh
adb push integration /data/local/tmp/rdoc_guard_integration
adb push libVkLayer_GLES_RenderDoc.so /data/local/tmp/libVkLayer_GLES_RenderDoc.so
adb shell chmod 755 /data/local/tmp/rdoc_guard_integration
adb shell 'LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/rdoc_guard_integration /data/local/tmp/libVkLayer_GLES_RenderDoc.so'
```

`vk_handle_guard_tests.cpp` 使用计数替身检查非法调用零次、合法调用一次、整批原子拒绝、
失败恢复、延迟释放、父对象注销、描述符代际变化、缩小代际耗尽、容量失败、并发抢占。
100,000 次复用检查槽位有界。benchmark 另做 1,000,000 次注册/claim/释放并断言槽位为 1。
integration 覆盖实际层的跨设备、错类型、旧身份、批量、池重置、特殊扩展及捕获结束/丢弃。

## 本次构建和验证记录

源码基线 `7e143d71b`，当前工作区修改；Android 使用 NDK r16b，arm64-v8a。
Linux 非 Android Vulkan `renderdoc` 完整构建通过；Windows 工程清单已更新，未执行 Windows 编译。
独立生命周期测试通过 WSL ASan/UBSan、Release CTest 和 Android ARM64 真机执行。

测试设备 HA29QMVJ，Android 15，Adreno 830；目标 `com.hottagames.yh.laohu`。
实际入口测试通过 debug-utils、debug-report、private-data、deferred-operation；设备不提供
ValidationCache 扩展，后者只完成实现/编译检查。BDA buffer 的重复销毁、捕获结束与丢弃通过。

最终 SO SHA256：
`705a14ed6cb43432c3252ceec6bfeb13b03f93a65b85d8929d9dd8673a775859`，29,094,528 字节。
对应符号文件 SHA256：
`5a7cf088da7ce18ed21ae26ee650950551801829e4b578de995e26801adb572d`。
二者 Build ID 相同：`da9683c71eaa550408db8e4c65e10c91ca1f44a4`。
原发布 SO SHA256：
`28348bfd2b092b51719f2d7dc4b55c0d34b397a9aa1a0e07663c431c7b613159`。

本机原始证据和产物位于 `D:/Projects/KSU_renderdochider/local/handle_guard/`：
`final.so`、`final.so.dbg`、`native_integration.log`、`native_lifecycle.log`、
`native_benchmark.log`、`long_run.json`、`*_perf.json`、`inject*_frame*.rdc` 和验证输出。
交付用标准名称位于该目录的 `delivery/libVkLayer_GLES_RenderDoc.so` 和 `.so.dbg`；
原 SO 备份为 `previous-28348bfd.so`，哈希清单为 `SHA256SUMS.txt`。

最终版本注入起点 2 的启动异常累计 19 次，严格守卫全部拒绝，无 legacy-forward：
PipelineLayout 1、Pipeline 2、DescriptorPool 8、DescriptorSetLayout 8。
与原场景数量一致；其中旧守卫可能转发的未知地址现已拒绝。

最终版本起点 2 连续监测 1,800 秒通过，31 次采样保持同一 PID 31336 且呈现时间戳前进。
时间为 2026-09-30 01:40:02 至 02:10:02（Asia/Shanghai）。
三次捕获的注册表槽位高水位均为 75,937，登记项从 49,956 到 51,073 随场景资源变化。
本次运行未出现崩溃、死锁或重复释放。百万次独立高频测试结束为 slots=1/live=0；
这支持槽位复用有界，但不等于证明所有场景没有资源泄漏。
进程总 PSS 从初始 3,666,488 KB 到结束 3,754,239 KB；其中包含游戏资源和截帧缓存，
不能把总 PSS 增量单独归因于注册表。五分钟后的样本在约 3.74–3.75 GB 范围。

| 最终版本 RDC | 字节 | 文件验证 |
| --- | ---: | --- |
| inject2_frame4701.rdc | 598,373,892 | OpenFile Success |
| inject2_frame33953.rdc | 587,293,165 | OpenFile Success |
| inject2_frame53793.rdc | 588,131,704 | OpenFile Success |
| final_inject1_frame3393.rdc | 578,935,308 | OpenFile Success |

注入起点 1 使用相同最终 SO 复核：PID 7738，成功进入场景并截帧，累计同样拒绝 19 次，
legacy-forward 为 0。较早的三份 `inject1_frame*.rdc` 来自中间版本，仅作为开发过程证据，
不计入最终版本的四次成功截帧。

独立 integration 的 native_capture.rdc 同时通过 OpenFile/OpenCapture，PC 成功读取动作和资源。

安静场景四段各 5 秒采样：原 SO 平均帧间隔 32.4225 ms、进程 CPU 143.2544 ticks/s；
最终 SO 32.4688 ms、140.6785 ticks/s。中位/p95 帧间隔均约 33.3338 ms。
没有观察到稳定超过噪声的性能退化；这是单设备固定场景的有限采样，不能据此保证所有游戏零开销。
微基准受 CPU 调频影响较大，应保留原始数据而不把单轮负差值解释为防护更快。

RDC 的 `OpenFile` 验证成功。PC 的 RTX 4070 SUPER 不支持捕获所需的
`VK_QCOM_render_pass_shader_resolve`，因此不能在该 PC 上完成回放；文件可打开不等同于完整回放通过。

验证结束已停止游戏、移除游戏 lib 目录中的临时捕获 SO，并恢复
`ro.force.debuggable=0`、`enable_gpu_debug_layers=0`；本次注入属性和兼容属性均清空。
本地 RDC、日志、源码及交付产物保留。现有工具目录中的原发布 SO 没有被替换。
