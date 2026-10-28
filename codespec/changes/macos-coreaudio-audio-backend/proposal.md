# CHG-045 macOS 音频设备后端：CoreAudio 补上第四足

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-045 |
| 提出日期 | 2026-10-27 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | **新增库内部后端**（非公共 API）：`src/aurora/media/audio_coreaudio.h` + `src/aurora/media/audio_coreaudio.cpp`（pimpl 隔离，CoreAudio / AudioToolbox 头不外泄）、`src/aurora/media/audio.cpp`（工厂择路加 Apple 分支）、`cmake/AuroraBackends.cmake`（新开关 `AURORA_ENABLE_AUDIO_COREAUDIO` + `-framework AudioToolbox -framework CoreAudio`）、`include/aurora/debug/feature_flags.h` 与 `src/aurora/debug/feature_flags.cpp`（**公共结构体 `FeatureFlags` 新增一枚 bool 字段 `enable_audio_coreaudio`**，源码兼容：全字段带默认初值 + 指定初始化器口径；`to_json()` 键序随之多一键）、`tests/unit/utest_audio_coreaudio.cpp`（新文件）、`tools/verify/coreaudio_audio_live_probe.cpp` + `cmake/AuroraVerify.cmake`（新探针）、`codespec/specification/03-layout-render.md` §9、`codespec/BUILD_OPTIONS.md` §4 与 §附录命令清单、`codespec/SPECIFICATIONS.md` §8 文档地图行（「三端设备层」→「四端」）；`aurora_api.json` 需重生（字段级新增）；**不改任何既有签名、不改 vtable、不改错误码** |

## 动机

1. **macOS 是四平台 Surface 里唯一「音频恒静默」的一足。** `create_default_device_backend()` 的分支只有 `AURORA_PLATFORM_WASM → WebAudio` / `AURORA_ENABLE_AUDIO_ALSA → ALSA` / `否则 → Wasapi`，而 `audio_wasapi.cpp` 在非 Windows 编译为 disabled 桩（`start()` 恒 false）。于是 macOS 上即便 `-DAURORA_ENABLE_AUDIO=ON`，`AudioContext` 也只能进静默模式——图能跑、时钟不推进、录音乐器入口恒报 `AudioDeviceUnavailable`。规格里「Windows WASAPI / Linux ALSA / 浏览器 Web Audio」三足对称，Apple 平台**根本没有对位后端**，属结构性缺口而非缺配置。
2. **这不是「未接线的可选能力」，而是已交付能力的平台缺口。** 音频图 API 恒编译并对外承诺（`feature_flags().audio` 可查、`03-layout-render.md` §9 全篇描述），设备层的缺失使用户拿到的是一台「能建图、永不出声」的机器，与 GPU 通道「后端缺位则回退软件光栅」的回退语义不同——软件光栅有真产物，静默模式没有。
3. **顺延不等于不存在。** CHANGELOG 里把 CoreAudio 记为「后续增量顺延」，规格 §9 与 `SPECIFICATIONS.md` §8 文档地图均只写「三端设备层」；文档口径本身没错（当时确实没有第四端），但顺延项长期无人认领会让 macOS 用户反复撞上同一条静默路径。本提案把它转为可验收批次。
4. **另一条更省事的路被否决**：让 macOS 继续落 WASAPI 桩、只在文档里加一句「Apple 无内置设备后端」。否决理由：① 与本仓既有的「后端对称设计」取向冲突（拖拽 / 剪贴板 / 菜单栏 / 最近文档四座桥都补齐了 macOS 腿，唯独音频留洞）；② 用户自定义 `AudioDeviceBackend` 注入虽是合法出口，但框架层不提供对位实现时，每个 macOS 应用都要自行重写同一份 CoreAudio 胶水。

## 变更内容

1. **采纳形态：CoreAudio `AudioQueue`（AudioToolbox）而非 AudioUnit HAL。** 三条实测/设计理由：
   - `AudioQueue` 自带客户端格式到设备格式的采样率与声道转换，图侧契约可恒为 48000/2 float32 交错——与 ALSA 靠插件层（plug/dmix/pulse）吸收差异、WASAPI 靠 `AUTOCONVERTPCM` 吸收差异**同一口径**；AudioUnit 需自行协商 `AudioChannelLayout` 与 `kAudioUnitProperty_MaximumFramesPerSlice`，并把交错 stereo 拆成非交错双 buffer。
   - `AudioQueue` 自带队列线程，`AudioQueueStop(q, true)` / `AudioQueueDispose(q, true)` 为**同步**语义，直接满足本接口「`stop()` 须等待设备线程退出后返回」的契约；AudioUnit 得自持一个渲染线程或依赖 CoreAudio 实时线程 + 手工 in-flight 计数。
   - 代价如实申报：端到端延迟高于 WASAPI 的 event-driven 路径（3 块 × 20ms 水位，约 40–60ms，与 Web Audio 后端的 60–150ms 同数量级），低延迟场景（监听回环）不在本期目标内。换 AudioUnit 属独立优化批次，不在本批。
2. **渲染端 `CoreAudioDeviceBackend`**：`format()` 恒 `{48000, 2}`；`start()` 建队列 → 分配 3 块 × 20ms 缓冲 → 预灌静音块 → `AudioQueueStart`；**任一步失败即回收并返回 false**（AudioContext 按既有契约静默降级，与 ALSA/WASAPI 同口径）。回调契约：`RenderFn(out, frames)` 直写队列 buffer 的 `mAudioData`，回调末尾 `AudioQueueEnqueueBuffer` 归还。**本机实测的失败形态**：无输出设备的机器上 `AudioQueueNewOutput` 仍返回 noErr，失败落在 `AudioQueueStart` = `kAudioQueueErr_InvalidDevice`(-66680)——故判据必须取 `start` 的返回码，不能取队列创建。
3. **重路由（默认输出设备变更）**：`AudioObjectAddPropertyListener` 挂 `kAudioHardwarePropertyDefaultOutputDevice`，通知只置位不重建（回调不在 CoreAudio 通知线程里做设备级操作）；实际重建由后端自持的控制线程执行——等待 200ms 节拍，置位后 `Dispose` 旧队列、按同一客户端格式建新队列并 `Start`，失败按 200ms 退避重试，期间无回调 ⇒ 图时钟冻结（对齐 ALSA 断连重开与 WASAPI 重路由语义）。控制线程同时以 200ms 轮询 `kAudioQueueProperty_IsRunning` 兜底（设备被拔出且默认设备未变时事件不到）。
4. **采集端 `CoreAudioCaptureBackend`**：先读默认输入设备的 `kAudioDevicePropertyNominalSampleRate` 与 `kAudioDevicePropertyStreamConfiguration`（Input scope 声道数），按**原生率/声道**建队列，回调携带实际 rate/channels——与 ALSA 的「顺位协商 + 回调携实际值」同语义（不做输入侧重采样：CoreAudio 输入队列不保证率转换，猜格式失败率高于读设备真值）。`failed()` 中段失败观察口与 `running` = stop 握手位纪律逐字继承 ALSA/WASAPI 审计结论。
5. **麦克风权限纪律（本条是安全边界，不是可选优化）**：macOS 上未声明 `NSMicrophoneUsageDescription` 的进程访问麦克风会被系统**直接终止**（不是返回错误）。故 `CoreAudioCaptureBackend::start()` 在建队列前先用 CoreFoundation 读 main bundle 的该 Info.plist 键，缺键即返回 false 并在诊断串里点名该键名。效果：① 裸可执行文件（CTest 单测、无壳探针）永不会因采集用例被 TCC 杀掉；② 有壳应用按其自身声明走系统正常授权流。不主动 `requestAccessForMediaType:`——那要么阻塞调用线程（违 `start() -> bool` 同步契约），要么引入 AVFoundation 依赖，本期两条都不取，如实申报为边界。
6. **工厂择路改为四分**：`AURORA_PLATFORM_WASM → WebAudio`；`AURORA_ENABLE_AUDIO_COREAUDIO → CoreAudio`；`AURORA_ENABLE_AUDIO_ALSA → ALSA`；其余 → WASAPI（真实实现 / disabled 桩）。Apple 上 ALSA 开关被 CMake 判 FATAL，故 CoreAudio 分支在 Apple 上无歧义；把 CoreAudio 排在 ALSA 之前只为「宏集合可被人工组合」时不出现 Apple 构建落 WASAPI 桩的反直觉形态。
7. **新开关 `AURORA_ENABLE_AUDIO_COREAUDIO`**：`AURORA_ENABLE_AUDIO=ON` 时在 Apple 默认 `ON`、其余平台 `OFF`；非 Apple 开启即 FATAL（与三枚同族开关逐字对称），经 `aurora_define_feature(... EXPORT)` PUBLIC 传播，链接 `-framework AudioToolbox -framework CoreAudio`。零运行期绑定需求：两枚 framework 属系统 SDK，不需要 dlopen 逃逸（与 ALSA 的零构建依赖取向不同，理由：ALSA 的动因是 Linux CI 无 dev 包，Apple 无此问题）。
8. **`FeatureFlags` 新增 `enable_audio_coreaudio`**：与三枚同族字段并列、`to_json()` 键 `AURORA_ENABLE_AUDIO_COREAUDIO`。这是本批唯一的公共 API 面变化（新增字段，非改签名），`aurora_api.json` 重生。
9. **不做清单**：
   - **不做 AudioUnit / 低延迟路径**：见第 1 条，属独立优化批次。
   - **不做输入侧采样率重采样与多流（aggregated）设备**：只取默认输入设备原生格式。
   - **不做主动权限申请与 AVFoundation 依赖**：见第 5 条。
   - **不做输出设备枚举与选定非默认设备**：三端既有后端一律只认「默认设备」，本批不打破该对称。
   - **不在本机做真机出声验收**：本虚拟机为 VMware Fusion guest，CoreAudio `device_count = 0`（实测 `kAudioHardwarePropertyDevices` 尺寸 0、默认输出/输入 id 均为 0），无虚拟声卡可驱动。探针在本机的合法形态是**退出码 2 报 ENV-UNAVAILABLE**（与 ALSA 探针在无 ALSA 栈的 WSL 上同口径），实际出声验收须在带音频设备的真机上补做，本批如实标注为未验证面。

## 验收判据

1. **无设备环境的静默降级实测（本机可验）**：`-DAURORA_ENABLE_AUDIO=ON -DAURORA_ENABLE_AUDIO_COREAUDIO=ON` 构建后，`AudioContext` 在本虚拟机上 `silent() == true`、`device_state() == Silent`，且构造/析构全程无崩溃——即 CoreAudio 代码路径在 `kAudioQueueErr_InvalidDevice` 上正确返回 false，而不是把失败压成「线程已起但不出声」的假活态。
2. **单测（环境无关不变量，`utest_audio_coreaudio`）**：格式契约恒 48000/2；未 `start` 即 `stop` 安全且幂等；`start → stop → stop → start → stop` 全生命周期无崩溃（含无线程时的析构可归属性）；采集 `start()` 返回 false 时 `failed()` 必为 false（启动期失败 ≠ 中段失败）；重开不带旧败。判据在「有真机 / 无设备 / 宏关闭」三种环境下均须成立（与 `utest_audio_alsa` 同纪律）。
3. **探针（`aurora_verify_coreaudio_audio`，不进 CTest）**：自动段结构与 ALSA/WASAPI 探针对齐——激活 / 格式契约 / 图时钟推进 ≥0.2s / 缓冲源出声通路 / 推流排空零溢出 / suspend-resume 时钟冻结与续推 / 采集观察口干净启停不误报；无输出设备时以退出码 2 报 ENV-UNAVAILABLE 属合法降级。**本机实测须落在退出码 2**（无设备），且不得崩溃——这条是本机唯一能自证的段。`--interactive` 出声人工段（扫频 + 双源混音）在无设备机器上如实标注为**未执行**。
4. **门禁**：`check_platform_macros`（新分支只用规范化宏）、`check_code_doc_sync` 与 `check_codespec_xref`（开关与后端段落文档同批）、`check_doc_comments`（新库内部头按 §13 形态注释）、`check_api_schema_sync`（`FeatureFlags` 新字段先 `--target aurora_api_json`）、`check_naming_conventions`、`check_no_hardcoded_paths`、`format-check`、`lint`（改动文件）、全量 `ctest` 绿。
5. **跨环境不回归**：Linux（ALSA 路径）、Windows（WASAPI 路径）、wasm（WebAudio 路径）的音频构建与用例零变化——本批改工厂择路与 feature flags 两处共享代码，须以三端各自的 configure 与用例证明未把 Apple 分支漏进别端。

## 回写落点

- `codespec/specification/03-layout-render.md` §9：新增「CoreAudio 后端」段（与 WASAPI / ALSA / Web Audio 三段并列，含 `AudioQueue` 选型理由、重路由与 IsRunning 兜底、采集原生格式协商、麦克风 Info.plist 纪律、延迟口径）；「设备层」段的后端计数与 `create_default_device_backend()` 择路描述由三分改为四分；如实标注本机无设备的未验证面。
- `codespec/BUILD_OPTIONS.md`：§2 路由表行、§4 开关表新增 `AURORA_ENABLE_AUDIO_COREAUDIO` 行（默认值、前置依赖、非 Apple FATAL、链接两枚 framework）、附录命令行清单补一行。
- `codespec/SPECIFICATIONS.md` §8 文档地图：layout-render 规格行的「WASAPI / ALSA / Web Audio 三端设备层」改为四端并补 CoreAudio。
- `codespec/manual-test/`：不新增手工用例——音频出声历来走探针 `--interactive` 人工段（`wasapi` / `alsa` 两路无 `TC-*` 条目），本批与之同口径，人工段在文档里点名为「须在有音频设备的真机执行」。

## 落码次序

新增库内部 TU + 一枚新开关 + 工厂一处分支 + `FeatureFlags` 一字段，与其它批次无文件重叠（`src/aurora/media/audio.cpp` 与 `include/aurora/debug/feature_flags.h`、`src/aurora/debug/feature_flags.cpp` 近期无兄弟提案触及）。次序：提案（本文件）→ `src/aurora/media/audio_coreaudio.h` 与 `src/aurora/media/audio_coreaudio.cpp` → CMake 开关与链接 → 工厂择路 → `FeatureFlags` 新字段与 `to_json()` 键（同步 `tests/unit/utest_feature_flags.cpp` 的宏键表）→ 单测 → 探针与 `cmake/AuroraVerify.cmake` → 文档回写 + 根 CHANGELOG 条目 → macOS 全量 ctest + 门禁 → 音频开关矩阵冒烟复核。

## 实施记录与偏离（落码期回写）

1. **`aurora_api.json` 无字段级新增**（与「影响面」里「需重生（字段级新增）」的表述偏离）：本仓 API schema 生成器只登记函数签名与属性键，`FeatureFlags` 的标量字段不进 schema（`grep FeatureFlags aurora_api.json` 只有 `feature_flags() -> FeatureFlags` 一行）。故本批唯一的公共 API 面变化是**结构体多一枚带默认初值的 bool 字段**，源码兼容、schema 零漂移；重跑生成器后 `aurora_api.json` 的变化全部来自上一批次的 `codespec/errors.toml` 文案回写，与本批无关。
2. **判据 1 由「SKIP」升级为「硬 PASS」**：无设备机器上 `AudioContext` 的降级形态本身可断言，探针在第 1 段直接验 `silent()` 与 `device_state()==Silent`（`no crash, no fake-active`），只有其后的出声段才真需要设备。本机实测输出：三条 PASS（重启判定不抖 / 采集拒启不置 `failed()` 且 TCC 未触 / 静默降级不误报 Active）+ 退出码 2，与「变更内容」第 9 条申报的合法形态一致。
3. **麦克风闸门按设计生效且可证**：裸可执行探针无 Info.plist ⇒ 拒启并点名 `NSMicrophoneUsageDescription`；`CoreAudioCaptureBackend::failed()` 保持 false（启动期失败 ≠ 中段失败）。副作用是探针的采集腿在无壳机器上只能给出这一条硬判据，干净启停与重开判据留在有壳应用或真机腿。
4. **`-66680` 的失败口径写进代码与文档两处**：`AudioQueueNewOutput` 在无设备机器上仍返回 `noErr`，失败落 `AudioQueueStart`。若按「队列建成即活」实现，会得到「线程已起却永不出声」的假活态——这条是本批最有价值的一条平台事实，已落 `src/aurora/media/audio_coreaudio.cpp` 的 WARN 诊断（`status=-66680; no usable device?`）、规格 §9 与本文件第 2 条。
5. **验收判据兑现情况**：判据 1、2、3 已在本机兑现（探针如上；`utest_audio_coreaudio` 6 例随全量 ctest 全绿）；判据 4 的门禁部分兑现——macOS 全量 `ctest` 449/449 绿、`format-check` 1227/1227 绿、`check_codespec_xref` / `check_change_proposals` / `check_code_doc_sync` / `check_doc_comments` / `check_platform_macros` / `check_api_schema_sync` 全绿，**`lint`（clang-tidy）在本机未跑**：这台机器只有 Apple clang-format 21，CommandLineTools 不带 clang-tidy，且 CI 钉的是 apt.llvm.org 的 llvm-22 快照（版本不同会给出不同结论，故不以本机凑出来的 tidy 结果充数），该门禁留待 CI 腿。**判据 3 的出声与热切换人工段、判据 1 的「有设备真机」形态未验证**（VMware Fusion guest `device_count == 0`，从 guest 侧无法打开 host 的虚拟声卡选项），须在带音频设备的 macOS 上补跑 `aurora_verify_coreaudio_audio --interactive`。
6. **本机格式化器版本与 CI 不同源，暴露一处真实漂移**：本机 `clang-format` 不在 PATH 上，导致 configure 早先静默跳过 `generate_error_codes` 的排版步，`include/aurora/core/error_codes.gen.h` 曾以「未折行」形态落盘。经显式指到 CommandLineTools 的 clang-format 后重新生成 + `format-check` 全绿，同时清掉了本分支早前几处手写换行（`src/aurora/window/macos_surface.cpp`、`include/aurora/window/platform.h`、`src/aurora/app/os_hotkey.cpp`）——三条均在 Apple 21 下与 HEAD 口径一致，属回归仓库风格而非改语义。附带一条环境事实值得记：CMake 侧的 clang-format 探测失败只 WARN 不 FATAL，「生成物未排版」这种形态在本地完全静默，CI 才会红灯。
7. **判据 5（跨环境不回归）以开关矩阵代替真端交叉构建**：这台 macOS 上没有 Linux / Windows / Emscripten 工具链，无法真做三端构建。改为证明「共享代码的分支可达性不受平台影响」：`AURORA_ENABLE_AUDIO=OFF`（零设备后端宏）与 `AURORA_ENABLE_AUDIO=ON / AURORA_ENABLE_AUDIO_COREAUDIO=OFF`（Apple 上回落 WASAPI disabled 桩，即本批之前的 macOS 形态）两种组合在本机各建一份独立构建目录、库目标与测试运行器全部构建通过，音频用例各 5 例（`utest_audio` / `utest_audio_alsa` / `utest_audio_coreaudio` / `utest_audio_sink_bridge` / `utest_audio_webaudio`）**两向 100% 通过**；`flags.make` 里 `AURORA_ENABLE_AUDIO_COREAUDIO` 的命中数在 ON / OFF / 前置开关 OFF 三份配置中分别为 3 / 0 / 0，证明新分支的可达性只由这两枚开关决定、与平台无关。加 `check_platform_macros` 对新分支的静态核对；三端各自的真构建由 CI 矩阵兜底，本批不留「已验证」的说法。
8. **本批复跑开关矩阵时暴露一处与本批无关的构建面缺陷，另立一批处置**：矩阵的三份配置都必须显式传 `-DAURORA_BACKEND_MACOS=ON` 才能构建，因为 Apple 上该开关的默认值 `OFF` 是一台构建不过的机器——本批陆续落地的 macOS 应用域实现（通知、托盘、最近文档、菜单栏桥、剪贴板）按平台宏而非后端宏门控，关掉后端后它们仍以纯 C++ 编译并缺框架；叠加另一处时序坑（开关的 `option()` 声明在 `add_library(aurora)` 之后才 `include()` 的模块里，顶层 `enable_language(OBJCXX)` 的前置判定读不到默认值，于是**不带 `-D` 的裸 configure 在 generate 阶段即失败**，而 CI 的 macOS 作业因显式传 flag 恰好从未暴露它）。该缺陷属后端矩阵修正、与音频语义无涉，故不并入本批：另立一批处置（默认值按平台取 + Apple 上关闭走 configure 期显式拒绝 + `option()` 声明上移），本批只在此登记「矩阵配置需显式 ON」这一实测前提。
9. **「无设备」这台虚拟机上试过两条零安装出路，都不成立，故出声段确证留在真机**：为了拿到 `AudioQueueStart` 成功路径的第一手证据，试过 **① 打开虚拟声卡**——本虚拟机是 VMware Fusion guest，声卡是**宿主机侧的虚拟机硬件配置**，guest 内没有可改的开关；共享文件夹（`/Volumes/VMware Shared Folders`）里只能看到 guest 自身根目录的映射，拿不到宿主侧的 `.vmx`。**② 造一枚 CoreAudio 聚合设备**——`AudioHardwareCreateAggregateDevice` 在 `device_count == 0` 的机器上**返回 `noErr` 并给出设备 id**，但它不进硬件设备清单（`kAudioHardwarePropertyDevices` 仍为 0 枚），把 `kAudioHardwarePropertyDefaultOutputDevice` 指过去虽然**写入返回 `noErr`、回读却是 `kAudioObjectUnknown`**（系统不采纳），`AudioQueueStart` 照旧 -66680。装第三方虚拟音频驱动那条路也不通：这台 guest 出网失败（对 `https://github.com` 的连接 12s 超时、0 字节，与同期 `git fetch` 的 `SSL_ERROR_SYSCALL` 同源），拿不到安装包。**结论不变且理由更硬**：成功路径（出声、时钟推进、suspend/resume、热切换）的验证前提是机器上有一枚可驱动的输出设备，本虚拟机不具备，须在带音频设备的 macOS 上补跑 `aurora_verify_coreaudio_audio --interactive`；本批不留「出声已验证」的说法。两处实验设备与临时文件均已销毁 / 删除，虚拟机状态未改（含麦克风权限：`TCC.db` 修改时间仍早于本会话，采集腿按设计在闸门处即拒、一次都没触达 TCC）。
