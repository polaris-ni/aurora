# window 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`window/` —— 窗口与后端层（24 个公共头），在实测依赖分层中位于 L4。

| 分组 | 头 |
|:---|:---|
| 窗口抽象 | `window.h` `window_state.h` `window_chrome.h` `window_host.h` `window_bus.h` |
| 表面抽象 | `surface.h` `native_surfaces.h` `platform.h` |
| 后端实现 | `win32_host.h` `win32_surface.h` `glfw_surface.h` `d3d11_surface.h` `x11_surface.h` `wayland_surface.h` `macos_surface.h` `wasm_surface.h` `wgpu_win32_surface.h` `wgpu_x11_surface.h` `wgpu_wayland_surface.h` |
| 辅助 | `frame_pacing.h` `cursor_map.h` `title_bar_geometry.h` `title_bar_style.h` `wasm_aria.h` |

本模块的人工增益集中在**只有真实窗口环境才能验证**的行为：多窗口生命周期、窗口角色与模态拦截、跨显示器移动、真实退出路径。

### 1.2 执行载体

复用既有示例程序与真机探针，不新增载体：

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_multi_window.cpp` | `demo_multi_window` | 多窗口创建与关闭、窗口角色与模态、跨显示器移动、跨窗广播、退出路径 |
| `examples/demos/demo_custom_surface.cpp` | `demo_custom_surface` | 自定义 `Surface` 注入、无窗口（Headless）路径与产物 |
| `examples/demos/demo_csd_gallery.cpp` | `demo_csd_gallery` | CSD（客户端自绘装饰）全景：五档策略预览窗；`ClientSide` host 自绘交互与 `Borderless`/`Frameless` 控件级 TitleBar 的真机点击走查（TC-WINDOW-016） |
| `tools/verify/glfw_dpi_live_probe.cpp` | `aurora_verify_glfw_dpi` | **GLFW 后端**的 DPI 缩放换算自洽与缩放变化上报（探针，非 demo） |
| `tools/verify/x11_csd_live_probe.cpp` | `aurora_verify_x11_csd` | **X11 后端**的客户端自绘装饰（CSD）离屏像素级判据：`ClientSide` 撤 WM 口沿后标题栏带真绘出、`content_inset` 上报栏高、`Auto`/`Frameless` 零入侵口径（探针，非 demo） |
| `tools/verify/win32_csd_live_probe.cpp` | `aurora_verify_win32_csd` | **Win32 后端**的客户端自绘装饰（CSD）离屏像素级判据：`WM_NCCALCSIZE` 吞框（最大化拉回工作区）、五档策略的 `content_inset` / 像素 / `WM_NCHITTEST` 热区回归、按钮释放触发与拖离取消、降级归零、热更（探针，非 demo） |

**第三个载体的构建口径与前两个不同，须单独构建目录**（探针受两个开关门控，二者在本模块的默认
`build/` 里都不满足，故前两个载体的构建步骤不能覆盖它）：

```
cmake -S . -B build-verify-glfw -G Ninja -DAURORA_BACKEND_GLFW=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
cmake --build build-verify-glfw --target aurora_verify_glfw_dpi
```

**X11 CSD 探针的构建口径更苛刻**：像素判据直读帧缓冲（`data()`），须 `AURORA_ENABLE_DEBUG=ON`
（Release 构建下该口被裁掉、恒返空指针），另需 `AURORA_BACKEND_X11=ON` 与
`AURORA_BUILD_VERIFY_TOOLS=ON`，三个开关在 GLFW 载体的构建目录里同样不满足，故独立成目录：

```
cmake -S . -B build-verify-x11 -G Ninja -DAURORA_BACKEND_X11=ON -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
cmake --build build-verify-x11 --target aurora_verify_x11_csd
```

**Win32 CSD 探针的构建口径同 X11**（像素判据直读帧缓冲，须 `AURORA_ENABLE_DEBUG=ON`，另需
`AURORA_BACKEND_WIN32=ON` 与 `AURORA_BUILD_VERIFY_TOOLS=ON`；本机默认 `build/` 的前两个开关
已满足，仅须追加 `-DAURORA_BUILD_VERIFY_TOOLS=ON` 重 configure）：

```
cmake -S . -B build-verify-win32 -G Ninja -DAURORA_BACKEND_WIN32=ON -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
cmake --build build-verify-win32 --target aurora_verify_win32_csd
```

探针的判据段（`content_inset` / 标题栏带像素 / 零入侵）为**自动段**，退出码语义同上；悬停 / 按压 /
拖拽 / 双击的**人工目视段**由用例步骤承载（见 TC-WINDOW-014），不受探针退出码覆盖。

探针**全部输出走 stdout**（经 `AURORA_LOG_RAW`），**stderr 恒为空**——判定读 stdout，勿按
「stderr 应有 N 行」立判据。退出码：`0` 全过；`1` 有断言失败；`2` 环境不可用；`3` 人工段待执行
（自动段已过但真跨屏未做，**此时是「待验收」不是「通过」**）。

**关于 `demo_custom_surface` 的已知现象（判定时不得记为缺陷）**：该载体含两条路径。

- 方案 A（自定义 `Surface` 经 `Application` 接入后调用 `render_to_png`）：因自定义 `Surface` 的尺寸在无头路径下未被设定，PNG 写出以 `writePNG: invalid dimensions` 结束。该结果以 `INF` 级日志输出，进程不失败。
- 方案 B（`HeadlessOptions` 经工厂组装 `Headless` 窗口后交给流式 `App`）：正常产出 PNG。

方案 A 的失败**属于载体的构造局限，不是 `Surface` 契约的缺陷**——它恰好可用来核对「错误路径的日志级别与不中止语义」。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja） |
| 构建 | 库已构建；两个载体为按需目标，不在默认构建内 |
| 终端 | 支持 UTF-8 输出 |
| 分离输出 | 诊断日志写 stderr；判定前须重定向 |
| 指针与键盘 | 真实鼠标与键盘 |
| 显示器 | **TC-WINDOW-008 / 009 / 013 需要 ≥2 台且缩放不同**；单显示器环境下该三条记 `SKIP` |

**本机实际启用的后端（实测自本机构建目录的 CMakeCache 缓存值，构建产物不入库）**：

| 后端开关 | 取值 |
|:---|:---|
| `AURORA_BACKEND_WIN32` | ON |
| `AURORA_BACKEND_HEADLESS` | 恒编入（无开关） |
| `AURORA_BACKEND_GLFW` / `D3D11` / `GPU_GL` / `GPU_WGPU` | OFF |
| `AURORA_BACKEND_X11` / `WAYLAND` / `MACOS` / `WASM` | OFF |

因此 **D3D11、wgpu、Wayland、macOS、WASM 五个后端的人工验证在本机不可执行**，本模块不为它们
编写用例（其窗口表现差异属跨平台真机验收的范畴）。TC-WINDOW-001 的方案 B 覆盖 Headless 路径，
其余用例覆盖 Win32 路径。

**X11 后端的例外**（口径同 GLFW）：本机有可达的 X server（`DISPLAY=:0`，XWayland 会话），只需另建
一个开启 `AURORA_BACKEND_X11` **且 `AURORA_ENABLE_DEBUG=ON`** 的构建目录（口径见 §1.2 的
X11 CSD 探针段），故 TC-WINDOW-014 覆盖 X11 的自绘装饰自动判据。

**Win32 后端的例外**：它是本机原生后端（`AURORA_BACKEND_WIN32=ON`），自绘装饰已落地（见
`07-environment-modifier.md` §4.1 三后端映射表）；探针只须 `AURORA_BUILD_VERIFY_TOOLS=ON`
（口径见 §1.2 的 Win32 CSD 探针段），故 TC-WINDOW-015 覆盖 Win32 的自绘装饰自动判据。

**GLFW 后端的例外**：它是**本机可执行**的——只需另建一个开启 `AURORA_BACKEND_GLFW` 的构建目录
（口径见 §1.2），不依赖本节表格所列的 `build/` 配置。故 TC-WINDOW-013 覆盖 GLFW 的 DPI 换算与
缩放上报；其跨屏段与其他后端一样受单显示器限制（见该用例前置条件）。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `window`，即 `TC-WINDOW-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
用例在文档中**按编号升序声明**——该顺序即默认执行顺序，也是依赖列表排序的依据（见 §1.5）。

### 1.5 用例字段清单与格式规范

每条用例由**六个固定字段**组成，排布于同一张 Markdown 表格内。**字段顺序固定、字段名一字不差、每字段独占一行、单元格内不换行**（需要多条内容时用 `<br>` 分隔）：

| 序号 | 字段 | 格式 | 正则约束 | 说明 |
|:--:|:---|:---|:---|:---|
| 1 | 用例编号 | 单行 | `^TC-[A-Z0-9]+-\d{3}$` | 模块内唯一且不复用 |
| 2 | 测试目的 | 单行 | 自由文本 | 一句话，指向**单一**验证目标 |
| 3 | 前置条件 | 单行 | 自由文本 | 只描述**环境状态**；「必须先执行某用例」属依赖用例字段，不在此重复 |
| 4 | 依赖用例 | 单行 | `^无$` 或 `^TC-[A-Z0-9]+-\d{3}(, TC-[A-Z0-9]+-\d{3})*$` | 见下方规范 |
| 5 | 操作步骤 | 条目列表 | 每条目以 `^\d+\. ` 开头，条目间以 `<br>` 分隔 | 自 1 起连续编号 |
| 6 | 预期结果 | 条目列表 | 同操作步骤；编号是步骤号的**子集** | 见 §1.6 |

**依赖用例字段规范（四条硬性要求）：**

1. 无依赖时**必须**写 `无`，不得留空、不得写 `-`、`N/A`、`none` 等变体。
2. 有依赖时写**用例编号全称**（含模块段），不写省略形式；跨模块依赖同样写全称。
3. 多项依赖以**半角逗号加一个空格**（`, `）分隔；逗号前后不得出现空格以外的字符。
4. 多项依赖的**排列顺序即依赖先后**——先决者在前。因用例按编号升序声明，等价于按被依赖用例的**编号升序**排列。

**模块级约定**：本模块两个载体的构建步骤统一置于 `TC-WINDOW-001` 的步骤 1（该步骤一次构建两个目标）；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。

**为什么必须统一格式**：本文件由 `tools/check/check_manual_test_format.py` 解析守护（CTest 用例 `check_manual_test_format`，校验字段名与顺序、取值域、依赖拓扑、步骤-预期同号映射与执行记录表格式）。上述约定使每个字段可用**单条正则**校验，无需自然语言推断。

### 1.6 操作步骤与预期结果编号规则

**预期结果与操作步骤同号一一对应**：`预期结果 N` 即「步骤 N 的对应结果」。

1. 预期结果的序号**等于**其对应步骤的序号，两者严格相等——不得因省略而重排、压缩或重新连续编号。
2. 纯执行步骤**不产生预期结果**，其序号在预期结果列表中直接跳过。
3. 故预期结果的序号**不要求连续**，条数也无需等于步骤数。
4. 步骤列表中，纯执行步骤在文本末尾标注「（纯执行，无预期结果）」，供与预期结果列表逐号对照。

示例：某用例有步骤 1、2、3、4，其中步骤 3 为纯执行步骤，则该用例的预期结果共 3 条，编号为 1、2、4。

**该规则解决的问题**：执行者按步骤顺序推进时，编号本身即是映射关系，无需自行推断「这条预期结果对应哪一步」；执行记录可借步骤号精确落到失败点。

### 1.7 判定与记录

| 结果 | 含义 |
|:---|:---|
| PASS | 全部非空预期结果均满足 |
| FAIL | 任一非空预期结果不满足；须在记录表登记该预期结果对应的**步骤号**、实际现象与缺陷编号 |
| BLOCKED | 前置条件无法满足（如载体构建失败），未开始执行 |
| SKIP | 主动判定不适用于本环境（如被测平台与用例限定平台不符） |

输出中含**时间戳、线程 id、行号**的字段属可变部分，判定时只核对格式与是否出现，不比对具体值。

**执行记录表格式**（列顺序固定）：

| 列 | 格式 | 允许值 / 正则 |
|:---|:---|:---|
| 用例编号 | 单行 | 与 §2 用例编号一致 |
| 执行日期 | 单行 | `^\d{4}-\d{2}-\d{2}$` |
| 执行人 | 单行 | 自由文本 |
| 结果 | 单行 | `PASS` / `FAIL` / `BLOCKED` / `SKIP` |
| 失败步骤号 | 单行 | 留空（未失败或未执行），或 `^\d+(, \d+)*$`（多个以半角逗号加一个空格分隔） |
| 实际现象 | 单行 | 自由文本 |
| 缺陷编号 | 单行 | 留空或缺陷单号 |
| 备注 | 单行 | 自由文本 |

未执行的取值一律**留空单元格**，不写 `-`、`TBD`、`N/A` 等占位符。

### 1.8 覆盖取舍

本模块按「人工判定是否优于程序判定」取舍，不追求对 24 个头的全覆盖。

**纳入人工测试的子域**：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 多窗口生命周期 | 新建、关闭、计数同步涉及跨窗口的可见状态，需真实窗口 |
| 窗口角色与模态 | 「模态期间主窗输入被屏蔽、关闭后自动恢复」是端到端输入链路不变量，需真实输入设备 |
| 跨窗通知 | 一条广播是否送达**所有**窗口，需多窗口同时可见 |
| 跨显示器移动 | 窗口出现在另一台显示器上，无头环境无法验证 |
| 表面契约与无头产物 | `Surface` 抽象能否被外部实现、无头路径能否产出 PNG，需真实文件产物 |
| 退出路径 | 「主动退出」与「关闭主窗」两条结束路径是否都能干净退出 |
| 关闭请求闸门（真机） | 「OS 关闭钮的请求何时置位」「否决后窗口是否真的不销毁」「窗口管理器是否重发关闭请求」三项只有真实窗口管理器参与才有答案；无头底座的程序化置位（`set_should_close`）代替不了消息路径，逐后端的清位落点也只有真机能验 |
| 窗口几何态恢复（真机） | 「落盘的几何态重启后是否回到那一档」「`Minimized` 是否如政策变 `Normal`」「各后端 `set_window_mode` 的真实落点与 WRN 留痕」只有真实 OS 窗口环境才有答案；无头 seam 只记录目标态并同步上报（`utest_window_geometry` 已覆盖快照 / State 两层与降级口径），代替不了 WM 与 compositor 的实际定档 |

**不纳入人工测试的头及其理由**：

| 头 | 理由 | 已有覆盖 |
|:---|:---|:---|
| `window.h` `window_state.h` | 选项结构、角色枚举与状态转换为纯数据与确定性逻辑 | `utest_window` `utest_window_state` |
| `window_chrome.h` `title_bar_geometry.h` `title_bar_style.h` | 装饰元素的几何与样式为数值计算，像素级正确性由 golden 比对 | `utest_window_chrome` `utest_title_bar_geometry` `utest_title_bar_style` `utest_title_bar_painter` |
| `window_host.h` | 宿主状态机为确定性逻辑；其**多实例可见行为**由 TC-WINDOW-003 至 TC-WINDOW-006 覆盖 | `utest_window_shell` `utest_window_lifecycle` `utest_window_geometry` |
| `win32_host.h` `win32_surface.h` | Win32 后端实现的内部逻辑；**接线正确性**由全部多窗口用例覆盖 | `utest_win32_host` `utest_win32_surface` |
| `frame_pacing.h` | 帧节奏为数值策略 | `utest_frame_pacing` |
| `cursor_map.h` | 映射表为静态查表 | `utest_cursor_map` |
| `window_bus.h` | 消息总线传递为确定性逻辑；其**跨窗送达**由 TC-WINDOW-007 覆盖 | `utest_window_bus` |
| `native_surfaces.h` `platform.h` | 平台判定与工厂分派 | `utest_native_surfaces` |
| `glfw_surface.h` `d3d11_surface.h` `x11_surface.h` `wayland_surface.h` `macos_surface.h` `wasm_surface.h` `wgpu_win32_surface.h` `wgpu_x11_surface.h` `wgpu_wayland_surface.h` | 本机构建中对应后端开关均为 OFF，**不可执行**；其真机验证属跨平台验收范畴 | `utest_glfw_surface` `utest_d3d11_surface` `utest_x11_surface` `utest_wayland_surface` `utest_macos_surface` `utest_wasm_surface` |
| `surface.h` | 抽象契约本身无运行时行为；其**外部实现可行性**由 TC-WINDOW-001 覆盖 | — |
| `wasm_aria.h` | 仅 WASM 后端使用，本机构建不可用 | — |

## 2 用例清单

### 2.1 载体基线

#### TC-WINDOW-001 无窗口载体的构建与运行基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-001 |
| 测试目的 | 确认载体可构建、不挂起事件循环地运行完毕，且无头路径能产出真实 PNG 产物 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；本机工具链可用 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建载体：`cmake --build build --target demo_multi_window demo_custom_surface`（纯执行，无预期结果）<br>2. 删除既有产物：`rm -f build/demo_custom_surface_*.png`（纯执行，无预期结果）<br>3. 运行 `./build/demo_custom_surface.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>4. 记录进程退出码<br>5. 检查 `build/` 下产出的 PNG 文件<br>6. 查看 `err.txt` 的内容、行数与级别 |
| 预期结果 | 4. 退出码为 0（该载体不进入事件循环，运行后自行结束；与多窗口载体不同，不会挂起等待关窗）<br>5. `build/demo_custom_surface_headless.png` 存在且体积非零（实测约 1.9 MB）<br>6. 恰 1 行，级别为 `INF`，消息为 `[demo_custom_surface] custom Surface PNG: writePNG: invalid dimensions`。<br>**判定要点**：该行是方案 A（自定义 `Surface` + `render_to_png`）的已知局限——自定义 `Surface` 在无头路径下未设定尺寸，故 PNG 写出失败；它被记录为 `INF` 而非 `ERR`，进程也未中止，这正是「降级不中止」在窗口层的体现（对照 `core` 模块的 TC-CORE-007）。方案 B 的产物即步骤 5 的文件 |

#### TC-WINDOW-002 多窗口载体启动基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-002 |
| 测试目的 | 确认多窗口载体启动后主窗正确显示，且初始窗口计数为 1 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；当前目录可写 |
| 依赖用例 | TC-WINDOW-001 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题与客户区尺寸<br>3. 查看 `err.txt` 的内容与行数<br>4. 读取窗口内计数文本 |
| 预期结果 | 2. 标题为 `Aurora multi-window (main)`，客户区约为 520×380<br>3. 恰 1 行，级别为 `INF`，消息含 `[multi_window] main window shown (close it to exit)`<br>4. 显示 `windows = 1` |

### 2.2 多窗口生命周期

#### TC-WINDOW-003 新建辅助窗口并同步窗口计数

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-003 |
| 测试目的 | 验证新建窗口会产出独立窗口实体，且主窗的窗口计数随之增长 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用；当前目录可写 |
| 依赖用例 | TC-WINDOW-002 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在主窗点击 `New auxiliary window` 按钮一次（纯执行，无预期结果）<br>3. 查看 `err.txt` 新增行的消息与新增窗口的标题，并读取主窗计数文本<br>4. 再次点击该按钮（纯执行，无预期结果）<br>5. 再次查看 `err.txt` 新增行、窗口标题与主窗计数 |
| 预期结果 | 3. 新增 1 行，级别 `INF`，消息形如 `[multi_window] opened window id= <N> total= 2`（`<N>` 为窗口 id，非零）；出现标题为 `Auxiliary #1` 的新窗口；主窗计数更新为 `windows = 2`<br>5. 新增行形如 `total= 3`（`id` 与上一次不同）；出现标题为 `Auxiliary #2` 的第二扇窗口；主窗计数更新为 `windows = 3`。**计数同步发生在帧末**，若读取过早可能仍显示旧值，等待约一帧后重读即可 |

#### TC-WINDOW-004 关闭单个辅助窗口不影响其他窗口

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-004 |
| 测试目的 | 验证每扇辅助窗口拥有独立生命周期：关闭其一不影响主窗与其余窗口继续渲染 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002, TC-WINDOW-003 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe`，并新建两扇辅助窗口（纯执行，无预期结果）<br>2. 在 `Auxiliary #1` 窗口内点击 `Close this window`（纯执行，无预期结果）<br>3. 观察主窗与 `Auxiliary #2` 窗口的存活与响应情况<br>4. 读取主窗计数文本 |
| 预期结果 | 3. `Auxiliary #1` 窗口消失；主窗与 `Auxiliary #2` 仍正常显示，且 `Auxiliary #2` 内的按钮仍可点击（未被连带冻结或关闭）<br>4. 约一帧后更新为 `windows = 2` |

#### TC-WINDOW-005 主窗按钮关闭最近打开的辅助窗口

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-005 |
| 测试目的 | 验证按窗口 id 程序化关闭的是最近打开的那一扇，而非任意一扇 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002, TC-WINDOW-003 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe`，依次新建两扇辅助窗口（`Auxiliary #1`、`Auxiliary #2`）（纯执行，无预期结果）<br>2. 在主窗点击 `Close last auxiliary window` 按钮（纯执行，无预期结果）<br>3. 观察哪一扇辅助窗口消失<br>4. 读取主窗计数文本 |
| 预期结果 | 3. 消失的是 `Auxiliary #2`（最近打开者），`Auxiliary #1` 保留并仍可交互<br>4. 约一帧后更新为 `windows = 2` |

### 2.3 窗口角色与模态

#### TC-WINDOW-006 模态对话框屏蔽拥有者输入并在关闭后恢复

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-006 |
| 测试目的 | 验证模态窗口打开期间拥有者窗口的输入被屏蔽，且关闭模态后无需额外操作即自动恢复 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在主窗点击 `Open modal dialog (blocks main window)` 按钮（纯执行，无预期结果）<br>3. 查看新窗口的标题与尺寸，并读取 `err.txt` 的行数<br>4. 在主窗内点击 `New auxiliary window` 按钮（纯执行，无预期结果）<br>5. 检查主窗计数文本与 `err.txt` 是否变化，并观察两窗的激活外观<br>6. 在模态窗口内点击 `Close modal` 按钮（纯执行，无预期结果）<br>7. 再次在主窗内点击 `New auxiliary window` 按钮（纯执行，无预期结果）<br>8. 检查主窗计数文本与 `err.txt` |
| 预期结果 | 3. 出现标题为 `Modal dialog`、客户区约 360×200 的窗口；`err.txt` 行数**不因打开模态而增加**（打开模态不产生日志）<br>5. 主窗计数文本**不因该点击而增加**：打开模态后计数即已变为 `windows = 2`（模态窗自身计入 `window_count()`），点击后仍为 `windows = 2`；`err.txt` **无新增行**（主窗输入确被屏蔽，按钮回调未执行）；模态窗口为激活外观、主窗为非激活外观<br>8. 主窗计数文本较关闭模态后增加 1（实测 `windows = 2`，即主窗加新建的辅助窗口，比打开模态前的 1 多 1）、`err.txt` 新增 1 行 `[multi_window] opened window id=`（输入已自动恢复，无需点击主窗激活） |

### 2.4 跨窗通知

#### TC-WINDOW-007 跨窗广播送达所有窗口

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-007 |
| 测试目的 | 验证经窗口总线发布的一次通知能被**所有**存活窗口收到，而非仅发送者或单扇窗口 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002, TC-WINDOW-003 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe`，并新建两扇辅助窗口（纯执行，无预期结果）<br>2. 在主窗点击 `Broadcast to all windows (bus)` 按钮一次（纯执行，无预期结果）<br>3. 同时查看两扇辅助窗口的回显区文本<br>4. 再点击该按钮两次（纯执行，无预期结果）<br>5. 再次查看两扇辅助窗口的回显区文本 |
| 预期结果 | 3. **两扇**辅助窗口的回显区**同时**更新为 `broadcast #1`（不止一扇）<br>5. 两扇窗口均更新为 `broadcast #3`（序号递增，与点击次数一致） |

### 2.5 显示器

#### TC-WINDOW-008 主窗移动到下一台显示器

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-008 |
| 测试目的 | 验证窗口可在显示器之间迁移，且迁移后尺寸与内容保持不变 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；**本机连接 ≥2 台显示器**；鼠标可用 |
| 依赖用例 | TC-WINDOW-002 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`，记录主窗所在显示器（纯执行，无预期结果）<br>2. 在主窗点击 `Move main window to next display` 按钮（纯执行，无预期结果）<br>3. 查看 `err.txt` 新增行的消息<br>4. 观察主窗当前的显示器位置、尺寸与内容完整度 |
| 预期结果 | 3. 新增 1 行，级别 `INF`，消息形如 `[multi_window] moved main window to display <显示器名称>`（若本机实际只有一台显示器则输出 `single display; nothing to move to`——此时本用例应为 `SKIP` 而非 `FAIL`）<br>4. 主窗出现在下一台显示器上；客户区尺寸与内容与迁移前一致，无缩放错乱、无内容丢失 |

#### TC-WINDOW-009 跨不同 DPI 显示器后内容缩放正确

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-009 |
| 测试目的 | 验证窗口迁移到 DPI 不同的显示器后，内容按新显示器的缩放比例重排，不出现模糊或错位 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；**本机连接 ≥2 台显示器且两者 DPI 缩放比例不同**；鼠标可用 |
| 依赖用例 | TC-WINDOW-002, TC-WINDOW-008 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe`，记录主窗在原始显示器上的文字清晰度与按钮尺寸（纯执行，无预期结果）<br>2. 点击 `Move main window to next display` 迁移到 DPI 不同的显示器（纯执行，无预期结果）<br>3. 对比迁移前后的文字清晰度与控件尺寸<br>4. 在迁移后的窗口内点击 `New auxiliary window`，观察新建窗口的尺寸与内容 |
| 预期结果 | 3. 文字在新显示器上清晰可辨（不出现位图放大导致的模糊）；按钮与文字的相对尺寸比例与迁移前一致<br>4. 新建的辅助窗口尺寸与内容正常，未沿用旧显示器的缩放参数 |

### 2.6 退出路径

#### TC-WINDOW-010 主动退出结束帧循环

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-010 |
| 测试目的 | 验证程序化退出路径能结束应用的帧循环并干净收尾 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`，并新建一扇辅助窗口（纯执行，无预期结果）<br>2. 在主窗点击 `Quit app (Application::quit)` 按钮（纯执行，无预期结果）<br>3. 记录进程退出码<br>4. 查看 `err.txt` 全文 |
| 预期结果 | 3. 退出码为 0；**所有窗口（含仍打开的辅助窗口）随之关闭**，进程不残留<br>4. 无 `ERR` 或 `FTL` 级日志 |

#### TC-WINDOW-011 关闭主窗结束进程

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-011 |
| 测试目的 | 验证关闭主窗这一默认退出路径能结束帧循环并干净收尾 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 点击主窗标题栏的关闭按钮（纯执行，无预期结果）<br>3. 记录进程退出码并确认窗口全部消失<br>4. 查看 `err.txt` 全文 |
| 预期结果 | 3. 退出码为 0；进程列表中不再存在该进程，无窗口残留<br>4. 无 `ERR` 或 `FTL` 级日志 |

#### TC-WINDOW-012 主窗关闭后辅助窗口存活（LastWindowClosed 语义）

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-012 |
| 测试目的 | 验证默认退出策略下关闭主窗**不**连带关闭其他窗口，进程存活到最后一扇窗口关闭为止 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WINDOW-002, TC-WINDOW-003 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`，并新建一扇辅助窗口（纯执行，无预期结果）<br>2. 关闭主窗的标题栏关闭按钮（纯执行，无预期结果）<br>3. 记录进程存活状态与存活窗口清单，并查看 `err.txt` 新增行<br>4. 关闭剩余的辅助窗口（纯执行，无预期结果）<br>5. 记录进程退出码与窗口清单<br>6. 查看 `err.txt` 全文 |
| 预期结果 | 3. 进程**仍存活**，只剩标题为 `Auxiliary #1` 的窗口；`err.txt` 新增 1 行 `INF`，消息形如 `[app] window host closed and reaped: id= <N>`（`<N>` 为主窗 id）<br>5. 退出码为 0，无窗口残留<br>6. 无 `ERR` 或 `FTL` 级日志 |

**关于 TC-WINDOW-011 与 TC-WINDOW-012 的分工**：`ExitPolicy::LastWindowClosed`（`window.h` 的默认策略，语义见 `specification/06-app-platform.md` §2.4）下，「关闭主窗即退出」只在主窗是**最后一扇**窗口时成立；仍有辅助窗口时进程必须存活。两例合起来覆盖该边界，任何单独一例都不足。

### 2.7 GLFW 后端 DPI 换算与缩放上报

#### TC-WINDOW-013 GLFW 后端缩放换算自洽且跨屏后上报新缩放

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-013 |
| 测试目的 | 验证 GLFW 后端把窗口尺寸与帧缓冲的物理像素 / 逻辑 dp 两套口径保持自洽，且跨不同 DPI 显示器后把新缩放如实上报给上层 |
| 前置条件 | 已按 §1.2 建好 `build-verify-glfw` 并构建出 `aurora_verify_glfw_dpi`；**跨屏段另需本机连接 ≥2 台缩放不同的显示器**（单显示器时该段记 `SKIP`，自动段仍须执行）；本机需有活动桌面会话（锁屏时窗口无法接收注入消息） |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `./build-verify-glfw/aurora_verify_glfw_dpi.exe 1> out.txt 2> err.txt`，记录退出码与 `out.txt` 中的 `scale_factor()` 读数（纯执行，无预期结果）<br>2. 查看 `out.txt` 中标记 `(b1)` 至 `(b4)` 的四行断言结果<br>3. 查看 `out.txt` 中标记 `(c1)`、`(e1)` 至 `(e3)`、`(e5)`、`(e6)` 的六行断言结果<br>4. 查看 `err.txt` 的行数<br>5. 加上 `--interactive` 重跑，在探针窗口出现后将其**拖到缩放比不同的显示器**，待控制台提示后按回车（纯执行，无预期结果）<br>6. 查看退出码与 `out.txt` 中标记 `(d1)` 至 `(d4)` 的断言结果 |
| 预期结果 | 2. 四行均为 `PASS`：`size()` 等于请求的 480x320 逻辑 dp、`framebuffer_size()` 等于 480x320 乘 `scale_factor()`、且 `逻辑尺寸 x scale` 与帧缓冲逐位相符（`(b4)` 证明帧缓冲未退化为逻辑尺寸）<br>3. 六行均为 `PASS`：`scale_factor()` 未因构造期校正而误报（`count=0`）；注入 `WM_DPICHANGED` 后 handler 被调用且收到的值等于注入 DPI/96 并与 `scale_factor()` 一致；**再注入同一 DPI 时 handler 不再被调用**（`(e5)` 的计数与前一步相同）且已采纳的缩放保持不变（`(e6)`）<br>4. `err.txt` 为 0 字节（探针输出全走 stdout）<br>6. `(d1)` 至 `(d4)` 均为 `PASS`（handler 收到新缩放、该值与 `scale_factor()` 一致、跨屏后一帧的帧缓冲按新缩放重算），退出码为 0 |

**本例与 TC-WINDOW-008 / 009 的分工**：那两条走 `demo_multi_window` 的 Win32 宿主路径、以**人眼**判定内容清晰度；本条走 GLFW 后端的探针、以**断言**判定数值自洽与上报时序。两者不可互相替代——Win32 路径正确不代表 GLFW 正确：GLFW 3.3 起窗口尺寸用的是屏幕坐标（物理像素），而 aurora 的窗口模型是逻辑 dp，此前该后端正是漏了这层换算（把 `glfwGetWindowSize` 当 dp 用），100% DPI 下两种单位解读重合故在 CI 上完全不显形。

**为什么 `(e5)` 这条不能省**：GLFW 自身**不做**「值未变则跳过」的去重（`win32_window.c` 收 `WM_DPICHANGED` 时
无条件调 `_glfwInputWindowContentScale`，后者只要注册了回调就无条件派发），所以「再注入一次相同 DPI」是
驱动 Aurora 侧去重分支（`on_content_scale` 的 `if (next == self->scale) return;`）的**唯一**手段。少了它，
该分支在自动段永远走不到——删掉去重后探针仍全绿，这类假绿已在本仓发生过一次（判据空转由变异注入暴露，
非由代码走查发现）。

**单显示器环境下本例的记法**：自动段（步骤 1-4）照常执行并记其结果；步骤 5-6 因前置不满足记 `SKIP`，
此时探针自身报 `PENDING MANUAL` 且退出码为 3。⚠️ **退出码 3 是「待验收」不是「通过」**，不得记 `PASS`。
本机当前即此状态（仅一块物理显示器），故下方记录表中本例的人工段记 `SKIP`。

### 2.8 客户端自绘装饰（CSD）

#### TC-WINDOW-014 X11 后端自绘装饰策略映射与像素判据

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-014 |
| 测试目的 | 验证 `X11Surface` 对 `WindowStyleOptions::decoration` 的三路映射：`ClientSide` 撤 WM 口沿后自绘标题栏带真绘出且安全区上报栏高；`Auto`（SSD）与 `Frameless` 装饰零入侵应用内容 |
| 前置条件 | 仓库根目录；`build-verify-x11` 已按 §1.2 的 X11 探针口径构建（三开关：`AURORA_BACKEND_X11` / `AURORA_ENABLE_DEBUG` / `AURORA_BUILD_VERIFY_TOOLS`）；`DISPLAY` 指向可达的 X server |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `DISPLAY=:0 ./build-verify-x11/aurora_verify_x11_csd 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 记录退出码<br>3. 查看 `out.txt` 的 PASS/FAIL 行、`band=`/`below=` 像素行与 `content_inset` 行<br>4. 查看 `err.txt` 应为空<br>5. （人工目视段）改用可见性 Normal 的最小示例或探针加 `--interactive` 观察：悬停三按钮的底色反馈、按压深于悬停、按住标题空白拖动窗口、标题双击切换最大化 |
| 预期结果 | 2. 退出码 0<br>3. `content_inset` 行 `top=36.0` 且 `left/right/bottom` 为 0；`band=303030`、`below=FFFFFF`；恰 7 行 `[PASS]`、0 行 `[FAIL]`（client-side 三条 + auto 两条 + frameless 两条）<br>4. 恒空（探针输出全走 stdout）<br>5. 悬停底色即时出现且随指针离开消失；按压底色深于悬停且松手才触发动作（拖开即取消）；拖动平滑跟手；双击在最大化与还原间切换 |

#### TC-WINDOW-015 Win32 后端自绘装饰策略映射、热区与释放触发判据

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-015 |
| 测试目的 | 验证 `Win32Host` 对 `WindowStyleOptions::decoration` 的五档映射：`WM_NCCALCSIZE` 吞非客户区使客户区 = 整窗（最大化拉回显示器工作区）；`ClientSide` 标题栏带真绘出（AdwaitaDark `#303030`）且安全区上报栏高；五档的 `WM_NCHITTEST` 热区逐点与共享层口径重合；按钮**释放触发**与「按下拖离松手不动作」取消语义；降级（`shadow_margin_dp` 显式关闭）热区归零；`set_title_bar_style` 热更生效 |
| 前置条件 | 仓库根目录；`build-verify-win32`（或追加了 `-DAURORA_BUILD_VERIFY_TOOLS=ON` 的默认 `build/`）已按 §1.2 的 Win32 探针口径构建（三开关：`AURORA_BACKEND_WIN32` / `AURORA_ENABLE_DEBUG` / `AURORA_BUILD_VERIFY_TOOLS`）；DWM 桌面合成开启（关闭时降级判据自适应，探针自行探测） |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `./build-verify-win32/aurora_verify_win32_csd.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 记录退出码<br>3. 查看 `out.txt` 的 PASS/FAIL 行、`#RRGGBB` 像素行、`HT*` 命中行与 `checks=` 汇总行<br>4. 查看 `err.txt` 应为空<br>5. （人工目视段）拖动 ClientSide 窗口标题带空白移动窗口、标题带双击切换最大化、Win+方向键 Aero Snap、确认 Alt+Space 系统菜单在**系统档**可用（自绘档不提供，属平台边界见 `BUILD_OPTIONS.md` 平台差异注记） |
| 预期结果 | 2. 退出码 0<br>3. `checks=47 pass=47`；`ClientSide` 段标题带 `#303030`、内容区 `#FFFFFF` 零入侵、`content_inset` `top=36`；热更后带 `#202020`（WindowsDark）且按压 `#991D11` 优先于悬停 `#C42B1C`；Auto 段带像素 `#FFFFFF` 且窗口顶部仍回报系统 caption；降级段左/底缘 `HTCLIENT` 化、角点回退标题带 `HTCAPTION`、带像素与栏高不变；最大化段客户区 = 工作区、左缘 `HTCLIENT`、释放最大化钮还原<br>4. 恒空（探针输出全走 stdout）<br>5. 拖动/双击/Aero Snap 由系统 `HTCAPTION` 白给、平滑跟手；系统档 Alt+Space 弹出系统菜单 |

#### TC-WINDOW-016 CSD 预览窗按可见位置点击零漂移（Win32 真机走查）

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-016 |
| 测试目的 | 验证 `demo_csd_gallery` 预览窗在真实窗口环境下「按控件可见位置点击即命中」——`ClientSide` host 自绘路径四按钮逐一生效，`Borderless`/`Frameless` 控件级 TitleBar 的点击命中链建立在绘制位置（绘制与命中零漂移，验收判据 3） |
| 前置条件 | 仓库根目录；`build/` 已构建载体 `demo_csd_gallery`（`cmake --build build --target demo_csd_gallery`）；DWM 桌面合成开启；活动桌面会话（锁屏时窗口无法接收注入输入）；真实鼠标或等效指针注入通道 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 启动载体：`./build/demo_csd_gallery.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在主控制窗点击 `ClientSide preview` 按钮，等预览窗出现（纯执行，无预期结果）<br>3. 按预览窗内 `Cycle title-bar theme (host self-draw)` 按钮的可见位置点击一次（纯执行，无预期结果）<br>4. 读取预览窗状态行文本与顶部自绘标题带的整带配色<br>5. 按 `Toggle window frame shadow` 按钮的可见位置点击一次（纯执行，无预期结果）<br>6. 读取状态行文本<br>7. 按 `Toggle title-bar icon` 按钮的可见位置点击一次（纯执行，无预期结果）<br>8. 读取状态行文本与标题带左端图标槽<br>9. 把该预览窗先缩矮到 `Close this preview` 按钮完全不可见，再向下拉长使该按钮重新可见（纯执行，无预期结果）<br>10. 按拉长后 `Close this preview` 按钮的可见位置点击一次（纯执行，无预期结果）<br>11. 查看预览窗存活状态与 `err.txt` 的新增行<br>12. 点击主控制窗 `Borderless preview` 按钮，在出现的预览窗内依次点击控件级 TitleBar 上的 `Info`、`−`、`□`、`×`（纯执行，无预期结果）<br>13. 查看预览窗存活状态与 `err.txt` 的新增行<br>14. 在该预览窗内按可见位置点击 `Close this preview`（纯执行，无预期结果）<br>15. 查看窗口状态与 `err.txt` 的新增行<br>16. 点击主控制窗 `Frameless preview` 按钮，重复步骤 12 与 14 的点击序列（纯执行，无预期结果）<br>17. 查看窗口状态与 `err.txt` 的新增行<br>18. 查看 `err.txt` 全文 |
| 预期结果 | 4. 状态行由 `host title-bar theme → <主题名>` 更新为循环序（AdwaitaDark → AdwaitaLight → WindowsDark → WindowsLight）的下一档；顶部自绘标题带整带配色随主题即时重绘<br>6. 状态行更新为 `window frame shadow → off`（若点击前已为 `off` 则更新为 `on`，即与点击前状态翻转）<br>8. 状态行更新为 `title-bar icon → set`（若点击前已为 `set` 则 `cleared`）；`set` 后标题带左端出现 24×24 双色演示图标，`cleared` 后图标消失<br>11. 预览窗关闭消失；`err.txt` 新增 1 行，级别 `INF`，消息形如 `[app] window host closed and reaped: id= <N>`（`<N>` 为该预览窗 id）。**判据要点**：点击发生在拉长后按钮的可见位置——修复前该点击落空（须上移约一个标题栏高度才生效），此步骤即原始缺陷场景<br>13. 窗口保持存活不关闭；`err.txt` 恰新增 1 行，级别 `INF`，消息含 `csdg: info clicked`（控件在其绘制位置被命中）；`−` / `□` / `×` 三钮不产生新增行（其表现为 Win32 平台范围空动作，见下方注记）<br>15. 预览窗关闭消失；`err.txt` 新增 1 行回收行（与步骤 11 同格式）<br>17. 与步骤 13 / 15 同口径：`Info` 命中日志新增 1 行、`−` / `□` / `×` 无新增行且窗口存活、`Close this preview` 后窗口消失并新增 1 行回收行<br>18. 除上述 `INF` 行外无 `ERR` / `FTL` 级日志；可出现既有观察性 `WRN`（预览窗关闭时布局父链解除提示，详见执行记录备注） |

**本例与 、及控件级 TitleBar 三钮的平台范围**：步骤 9–11 复原 `demo_csd_gallery` 的原始缺陷场景（的动因）——向下拉长窗口露出 `Close this preview` 后，修复前按按钮可见位置点击无效、须点击其上方约一个标题栏高度才生效；修复后同一可见位置点击即命中。步骤 12–17 的控件级 TitleBar 走查中，`−` / `□` / `×` 经 `WindowChrome` 转发的 `Surface::close()` / `minimize()` / `toggle_maximize()` / `set_fullscreen()` 为默认空实现、仅 Wayland（`xdg_toplevel`）覆盖（见 `specification/07-environment-modifier.md` 的「程序化窗口控制」段）——在 Win32 真机上表现为无害空动作，属既有平台范围、非缺陷，更非本次命中修复的回归；命中是否对齐以 `Info` 动作日志与 `Close this preview` 的可见位置点击判定，不得把三钮的空动作记为失败。

### 2.9 关闭请求闸门（跨后端真机走查）

#### TC-WINDOW-019 关闭请求闸门的置位时机、否决保活与逐后端清位走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-019 |
| 测试目的 | 验证关闭请求闸门在真实窗口环境下的三项表现：OS 关闭钮的置位时点、否决后窗口不销毁且窗口管理器是否重发请求、各后端清位落点的实测差异 |
| 前置条件 | 仓库根目录；关闭闸门真机探针已按被测后端的口径构建（Win32 腿用本机默认 `build/` 追加 `AURORA_BUILD_VERIFY_TOOLS=ON` 重 configure，X11 / GLFW 腿按 §1.2 的独立构建目录，Wayland / macOS / WASM 三腿需对应平台真机）；活动桌面会话（锁屏时窗口收不到 OS 关闭消息）；真实指针或等效注入通道 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 启动装载了「首问否决、再问放行」闸门回调的关闭闸门真机探针，取 Win32 腿，确认主窗已显示（纯执行，无预期结果）<br>2. 点击探针主窗标题栏的关闭按钮一次（纯执行，无预期结果）<br>3. 读取探针输出的关闭请求置位时点记录（相对 `pump_all_once` 与 `on_frame_` 的先后）与窗口存活状态<br>4. 等待约两帧后再次点击同一关闭钮一次（纯执行，无预期结果）<br>5. 读取闸门询问计数与窗口存活状态<br>6. 判定本后端在否决后**是否重发**关闭请求（以询问计数是否自增为据）<br>7. 切到放行档，再次点击关闭钮（纯执行，无预期结果）<br>8. 读取窗口存活状态与 `err.txt` 的回收行<br>9. 在 X11 会话（`DISPLAY` 指向可达 X server）下按同口径重跑步骤 1 至 8（纯执行，无预期结果）<br>10. 记录 X11 腿的三项实测（置位时点 / 是否重发 / 否决后是否存活）<br>11. 在 Wayland 会话下按同口径重跑步骤 1 至 8（纯执行，无预期结果）<br>12. 记录 Wayland 腿的三项实测<br>13. 在 GLFW 构建目录下按同口径重跑步骤 1 至 8（纯执行，无预期结果）<br>14. 记录 GLFW 腿的三项实测（含 `glfwSetWindowShouldClose` 清位后 `glfwWindowShouldClose` 是否回到假）<br>15. 在 macOS 真机上按同口径重跑步骤 1 至 8（纯执行，无预期结果）<br>16. 记录 macOS 腿的三项实测（骨架后端：清位是否生效、是否出现 fail-open `WRN`）<br>17. 在 WASM 页面壳下按同口径重跑步骤 1 至 8（纯执行，无预期结果）<br>18. 记录 WASM 腿的三项实测（含「否决任一窗口即否决全部」的共享标志降级是否如申报发生）<br>19. 汇总六腿记录，与 `specification/06-app-platform.md` §2.4 的申报逐条核对 |
| 预期结果 | 3. 置位时点落在帧内的 `pump_all_once` 之后、`on_frame_` 之前（即 OS 关闭消息由本帧 pump 带进来的当帧旗标）；窗口仍存活且当帧继续出图<br>5. 询问计数为 2（第一次置位与再置位各一次抬沿，同帧多次置位不重复问）；窗口**仍存活**，`err.txt` 无回收行<br>6. 计数自增即证明本后端的窗口管理器在否决后**不再重发**关闭请求；若计数持续自增则记为「重发」，须在记录表备注中写明重发频率（二者都属可接受实测，但必须与规格申报一致）<br>8. 窗口消失；`err.txt` 恰新增 1 行 `INF`，消息形如 `[app] window host closed and reaped: id= <N>`<br>10. 三项齐全且与 Win32 腿的口径逐项可比：置位时点同样落在 pump 之后，否决后窗口存活<br>12. 三项齐全；Wayland 的程序化 `Surface::close()` 写的是同一个后端位，故「程序化 / OS」两路在宿主记账位上须可分（来源判为 `Programmatic` 而非 `OperatingSystem`）<br>14. 三项齐全，且清位后 `glfwWindowShouldClose` 读回假<br>16. 三项齐全；macOS 骨架后端如实记录清位是否生效与是否出现 fail-open `WRN`（不得记为缺陷，属骨架后端的既有范围）<br>18. 三项齐全；页面级单标志共享导致的「否决任一窗口即否决全部」须如实记录是否与规格申报一致<br>19. 六腿记录与规格申报逐条一致；凡申报为降级的项（WASM 共享标志、macOS 骨架）在实测中确有对应表现，未申报的项不得出现降级 |

**本例编号的取定**：现最高为 016；017 / 018 为同批预留后未落用的断档序号，按 §1.4「序号不复用」不作回填，本例取下一个可用序号 019。

**本例与 TC-WINDOW-011 / 012 的分工**：那两条走 `demo_multi_window` 的默认路径（不装闸门回调），只验「关闭能干净退出」与退出策略下的存活边界；本例装载闸门回调，验的是「关闭可被撤销」与逐后端的清位落点。既有 demo 载体不装闸门、无法承载，故本例载体为**探针（非 demo）**，构建口径同 §1.2 的各后端探针段（三开关：后端开关 / `AURORA_ENABLE_DEBUG` / `AURORA_BUILD_VERIFY_TOOLS`）。

**六腿不可互相替代的原因**：OS 关闭请求的置位发生在后端消息路径里，清位是各后端私有标志的回写，二者都是后端私事；无头底座的 `set_should_close(false)` 能证明闸门逻辑正确，不能证明「点了关闭钮窗口真的还在」——后者才是本例的判据，也是「否决后窗口管理器是否重发」这一问只有真机能回答的原因。

### 2.10 窗口几何态恢复（真机走查）

#### TC-WINDOW-020 三档几何态重启恢复与 Minimized 归 Normal 策略走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WINDOW-020 |
| 测试目的 | 验证落盘的窗口几何态（Normal / Maximized / FullScreen）重启后能否回到那一档，以及 Minimized 按政策恢复为 Normal 并留 INFO——真实 OS 窗口环境下的端到端走查 |
| 前置条件 | 仓库根目录；几何态恢复真机探针已按被测后端的口径构建（Win32 腿用本机默认 `build/` 追加 `AURORA_BUILD_VERIFY_TOOLS=ON` 重 configure，X11 / GLFW 腿按 §1.2 的独立构建目录，Wayland / macOS / WASM 腿需对应平台真机）；探针宿主声明持久化键并挂接可落盘的 `Preferences`；活动桌面会话（锁屏时窗口收不到 OS 消息）；真实指针或等效注入通道 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 启动几何态恢复真机探针，取 Win32 腿，指定目标档 `Normal`：把主窗拖到非默认位置并缩放到非默认尺寸，正常关闭探针使几何落盘，再启动第二趟（纯执行，无预期结果）<br>2. 读取第二趟启动时的 mode 读数、主窗位置与尺寸，并查看 `err.txt` 新增行<br>3. 按同口径把目标档切为 `Maximized`：首趟切档后正常关闭落盘，启动次趟读取 mode 读数与窗口外观，并查看 `err.txt` 新增行<br>4. 按同口径把目标档切为 `FullScreen`：首趟切档后正常关闭落盘，启动次趟读取 mode 读数、窗口外观与 `err.txt` 新增行<br>5. 把主窗最小化后正常关闭落盘，启动次趟读取 mode 读数、窗口是否可见与 `err.txt` 新增行<br>6. 在 X11 会话（`DISPLAY` 指向可达 X server）下按同口径重跑 `Normal` / `Maximized` / `FullScreen` / `Minimized` 四组（纯执行，无预期结果）<br>7. 记录 X11 腿四组实测：mode 是否回到那一档、`Minimized` 是否如政策变 `Normal`、位置与尺寸是否恢复<br>8. 在 Wayland 会话下按同口径重跑四组（纯执行，无预期结果）<br>9. 记录 Wayland 腿四组实测<br>10. 在 GLFW 构建目录下按同口径重跑四组（纯执行，无预期结果）<br>11. 记录 GLFW 腿四组实测（含 `FullScreen` 一档的 WRN 留痕是否出现）<br>12. 在 macOS 真机上按同口径重跑四组（纯执行，无预期结果）<br>13. 记录 macOS 腿四组实测（骨架后端：`set_window_mode` 为默认空实现，mode 是否停在 `Normal`）<br>14. 在 WASM 页面壳下按同口径重跑四组（纯执行，无预期结果）<br>15. 记录 WASM 腿四组实测（无 OS 窗口语义，mode 是否停在 `Normal`、位置尺寸是否未动）<br>16. 汇总各腿记录，与 `specification/06-app-platform.md` §2.4「几何态恢复」的申报逐条核对，并将 mode 读数与「缩放与跨屏不更新」「X11 / Wayland 位置与尺寸未实现」两条既有偏差分开记录 |
| 预期结果 | 2. Win32 腿 `Normal`：mode 读数为 `Normal`；主窗位置与尺寸回到首趟落盘值（物理像素口径，Win32 的 `set_size` / `set_position` 为真实现，该档应逐位恢复）；`err.txt` 无新增 `ERR` / `FTL`（Normal 档恢复不产生日志）<br>3. Win32 腿 `Maximized`：mode 读数回到 `Maximized`，窗口以最大化外观呈现；`err.txt` 无新增行（该档只追几何态，不产生日志）<br>4. Win32 腿 `FullScreen`：本后端无 monitor 切换通道（申报内缺口）——`err.txt` 应出现 1 行 WRN（tag `"window"`，消息含 `FullScreen is not implemented on this backend`），mode 实际读数如实记录（预期停在原档而非 `FullScreen`）；此为申报内降级，不得记缺陷，也不得记「读不回」式失败<br>5. Win32 腿 `Minimized`：mode 读数为 `Normal` 且窗口可见（非最小化外观）；`err.txt` 恰新增 1 行 `INF`（tag `"app"`），消息形如 `stored window mode is Minimized; restoring as Normal (key=<k>)`<br>7. X11 腿四组齐全：`Normal` 档的位置与尺寸**按未实现缺口单独记录**（该后端 `set_size` / `set_position` 为空动作，no-op 属申报内，不得记缺陷）；`Maximized` / `FullScreen` 两档 mode 读数应各自回到那一档；`Minimized` 一档同 Win32 口径（变 `Normal` + INFO）。mode 读数与位置尺寸读数**不得混为一谈**<br>9. Wayland 腿四组齐全，口径同 X11 腿（位置与尺寸同样按未实现缺口单独记录）；`Maximized` / `FullScreen` 应经 `xdg_toplevel` 最大化 / 全屏态真实生效<br>11. GLFW 腿四组齐全；`FullScreen` 一档同 Win32 口径（WRN 留痕 + mode 不声称到位）；其余三档 mode 读数各自回到那一档<br>13. macOS 腿四组齐全；骨架后端 `set_window_mode` 为默认空实现 ⇒ mode 停在 `Normal`、无模式切换外观，如实记录，不得记为缺陷（骨架后端的既有范围）<br>15. WASM 腿四组齐全；浏览器无 OS 窗口几何语义 ⇒ mode 停在 `Normal`、位置尺寸不动，属申报内平台范围，不得记为缺陷<br>16. 各腿记录与规格申报逐条一致；凡申报为降级的项（Win32 / GLFW 的 FullScreen、macOS 骨架、WASM 无 OS 语义、X11 / Wayland 位置尺寸未实现）在实测中确有对应表现；mode 读数与「缩放与跨屏不更新」（Linux 三后端缩放变化上报未实现）是两个独立偏差，须分开记录，不得互相解释 |

**本例编号的取定**：现最高为 019（关闭请求闸门用例）；017 / 018 为早期预留后未落用的断档序号——按 §1.4「序号不复用、新用例取下一个可用序号」不作断档回填，本例取 020。

**本例与既有读数的分工**：mode 恢复腿的消费点与两条既有偏差互相独立——①「Linux 三后端缩放变化上报未实现」（改系统缩放或跨屏后 `scale_factor` 不自动更新，影响的是 `Normal` 档的物理 ↔ 逻辑折算，见 §2.7 与 TC-WINDOW-013）；② X11 / Wayland 的 `set_size` / `set_position` 未实现（位置与尺寸恢复在 Linux 上是 no-op）。三者不得混为一个读数：mode 不对就说 mode，尺寸不对先分辨是哪条偏差。

**本例载体为真机探针（非 demo）**：既有 demo 不声明 `persist_id`、不挂几何存储，承载不了恢复腿；构建口径同 §1.2 的各后端探针段（后端开关 + `AURORA_BUILD_VERIFY_TOOLS`），每腿以「首趟落盘、次趟恢复」两趟运行。落盘只走正常关闭路径（`reap_closed` 收集步对 `should_close()==true` 的宿主执行并打一次性闩），故步骤里的关闭一律要求放行关闭，不用 `quit()` 代替。

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-WINDOW-001 | 2026-09-26 | Qoder Agent | PASS | | 步骤 4：退出码 0。步骤 5：`build/demo_custom_surface_headless.png` 存在且为 1920813 字节（约 1.9 MB）。步骤 6：`err.txt` 恰 1 行，级别 `INF`，消息为 `[demo_custom_surface] custom Surface PNG: writePNG: invalid dimensions`，`out.txt` 为 0 字节 | | 步骤 1 的两个目标在既有 `build/` 目录构建成功。该行属 §1.2 已登记的方案 A 构造局限，按 §1.2 不记缺陷，且其 `INF` 级别与「不中止」语义正是本例的判定要点 |
| TC-WINDOW-002 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：标题 `Aurora multi-window (main)`，窗口 802x626 物理像素、客户区 780x570 物理像素，本机 DPI 缩放 150% 即 520x380 逻辑。步骤 3：`err.txt` 恰 1 行 `INF`，消息含 `[multi_window] main window shown (close it to exit)`。步骤 4：计数文本为 `windows = 1` | | 计数文本以窗口自身像素读数为准（经 `PrintWindow` 抓取），无障碍树同值。主窗 6 个按钮的无障碍名与 §2 用例引用的标签逐一对应 |
| TC-WINDOW-003 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：新增 1 行 `INF` 且消息为 `[multi_window] opened window id=2 total=2`，出现标题 `Auxiliary #1`（客户区 660x450 物理像素即 440x300 逻辑），主窗计数像素读数 `windows = 2`。步骤 5：新增行 `opened window id=3 total=3`（id 与上次不同），出现 `Auxiliary #2`，主窗计数像素读数 `windows = 3` | | 回调线程 id 与启动行一致，即点击在 UI 线程派发。顺带观察：此刻无障碍树仍回读 `windows = 2`（快照滞后于帧末更新），故窗口内文本一律以像素读数判定 |
| TC-WINDOW-004 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：点击 `Auxiliary #1` 内的 `Close this window` 后该窗消失并新增 `INF` 行 `[app] window host closed and reaped: id=2`，主窗与 `Auxiliary #2` 均存活可交互（主窗随后仍可新建窗口并记 `opened window id=4 total=3`，`Auxiliary #2` 的关闭按钮点击后同样正常关闭）。步骤 4：主窗计数像素读数 `windows = 2` | | 「#2 按钮仍可点击」以点击其自身关闭按钮并观察窗口消失来证明，而非仅看外观 |
| TC-WINDOW-005 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：消失的是 `Auxiliary #2`（`err.txt` 记 `[app] window host closed and reaped: id=3`，id=3 即第二次新建者），`Auxiliary #1` 保留。步骤 4：主窗计数像素读数 `windows = 2` | | 「最近打开者」以 `err.txt` 的 id 对照确认：第二次新建记 `opened window id=3`，被回收的正是 id=3 |
| TC-WINDOW-006 | 2026-09-28 | Qoder Agent | PASS | | 步骤 3：出现标题 `Modal dialog`、客户区 540x300 物理像素（即 360x200 逻辑）的窗口，`err.txt` 行数保持 1，未因打开模态而增加。步骤 4/5：真实指针点击（`SetCursorPos` + `mouse_event`）取两组坐标各做「模态前→模态中→关模态后」三轮同点对照——新建按钮带中心 (191,234) 上业务行 `opened window id=` 依次 0→1、1→1（不变）、1→2；广播按钮带中心 (385,616) 位于模态窗矩形 (253,253,628,490) **之外**且 `WindowFromPoint` 复核命中窗即主窗 hwnd，其回显文本依次 `broadcast #1`、仍 `broadcast #1`（`err.txt` 无新增行）、`broadcast #2`。`IsWindowEnabled(主窗)` 由 True 翻 False 再自动回 True（模态窗恒 True），主窗计数像素读数全程 `windows = 2`（即主窗加模态窗）未虚增。步骤 5 外观：同一时刻标题栏色带比对，主窗开模态前均色 (229,239,242)、模态中 (239,239,239)（灰化，主色 (236,246,249)→(243,243,243)），模态窗自身 (229,239,242) 与主窗激活态一致，即模态窗为激活外观、主窗为非激活外观。步骤 6/8：真点模态窗自己的按钮带中心 (328,437) 后该窗消失，`IsWindowEnabled(主窗)` 自动恢复 True 且前窗即主窗，随即同坐标一次真点新建成功新增 1 行 `INF` 且消息为 `[multi_window] opened window id=`，主窗计数像素读数 `windows = 2`（模态窗已注销、辅助窗新增） | | 模态拦截在 OS 层实现（`EnableWindow(owner, FALSE)`，见 `src/aurora/window/win32_host.cpp`）。判据取「真实点击的同点三轮」而非 `IsWindowEnabled` 单点：合成点击经 `PostMessage` 直达窗口过程、绕过 OS 对禁用窗口的输入丢弃（本轮复现：主窗禁用态下经 `/api/input/click` 仍触发回调并多出一扇辅助窗口），故该通道不得用作本步判据。「模态窗激活、主窗非激活」的外观项在 26 轮记为无法判定，原因是当时本机锁屏、前台窗恒为锁屏 CoreWindow；解锁会话下前窗可为应用窗口，本轮按标题栏色带的像素差实测判定。坐标换算与蓝带定位口径同 `19-app.md` 的 TC-APP-005（两例共用同一载体与同一轮取证，脚本 `a005c_run.py`、`a005d_run.py`，均位于 build 工作区的 manual-lib 目录、属一次性取证产物未入库）。另按实测把预期 5/8 的计数口径回写为「模态窗自身计入 `window_count()`」 |
| TC-WINDOW-007 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：两扇辅助窗口的回显区在同一时刻的像素截图均为 `broadcast #1`（主窗回显同为 `broadcast #1`）。步骤 5：再点击两次后两窗回显区均为 `broadcast #3`，序号与点击次数一致 | | 广播不写日志，`err.txt` 行数不变。无障碍树在第三次广播后仍回读 `broadcast #1`（快照滞后），判定同样以像素截图为准 |
| TC-WINDOW-008 | 2026-09-26 | Qoder Agent | SKIP | | `EnumDisplayDevicesW` 仅枚举到 1 台活动显示器（`\\.\DISPLAY1`，AMD Radeon RX 7800 XT，主屏），§1.3 的「≥2 台显示器」前置不满足。作为旁证仍执行了步骤 2：`err.txt` 新增 1 行 `INF` 且消息为 `[multi_window] single display; nothing to move to`，主窗位置未变 | | 按 §1.7 记 SKIP 而非 FAIL，载体自身的单显示器分支日志与预期一致 |
| TC-WINDOW-009 | 2026-09-26 | Qoder Agent | SKIP | | 同 TC-WINDOW-008：本机仅 1 台显示器，无从取得第二台不同 DPI 缩放的显示器，前置条件无法满足 | | 依赖用例 TC-WINDOW-008 亦为 SKIP |
| TC-WINDOW-010 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：退出码 0，`EnumWindows` 已枚举不到该进程的任何窗口（含仍打开的 `Auxiliary #1`），进程不残留。步骤 4：`err.txt` 全文 2 行均为 `INF`（`main window shown`、`opened window id=2 total=2`），无 `ERR`/`FTL` | | 辅助窗口未各自关闭即随进程结束，符合 `Application::quit()` 在任何退出策略下都生效的语义（对照 TC-WINDOW-012） |
| TC-WINDOW-011 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：退出码 0，进程不再存在且无窗口残留。步骤 4：`err.txt` 仅启动的 1 行 `INF`，无 `ERR`/`FTL` | | 本机会话锁屏，标题栏属非客户区、合成点击落不到，故步骤 2 以发送 `WM_SYSCOMMAND`/`SC_CLOSE` 等价代替——这正是点击标题栏关闭按钮时系统投递给窗口的消息。本用例文本于本轮修订（原步骤含「新建一扇辅助窗口」，与 `ExitPolicy::LastWindowClosed` 语义冲突，见 §2.6 分工说明） |
| TC-WINDOW-012 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：关闭主窗后进程仍存活，存活窗口只剩标题 `Auxiliary #1` 的一扇，`err.txt` 新增 1 行 `INF` 且消息为 `[app] window host closed and reaped: id=1`（id=1 即主窗）。步骤 5：关闭该辅助窗口后退出码 0，无窗口残留。步骤 6：`err.txt` 全文 3 行均为 `INF`，无 `ERR`/`FTL` | | 关闭最后一扇窗口时进程先行退出，故不再有该窗口的回收日志行，属预期时序。本例为新增，用以固定「主窗关闭不连带关闭其他窗口」这一默认策略边界 |
| TC-WINDOW-013 | 2026-10-03 | WorkBuddy | SKIP | | 自动段（步骤 1-4）全部通过：`scale_factor()` = 1.5（本机 150%）；`(b1)` 480x320 逻辑 dp、`(b2)` 帧缓冲 720x480、`(b3)` 480x320x1.5=720x480 三者逐位相符、`(b4)` 未退化为逻辑尺寸；`(c1)` 构造期无误报 count=0；`(e1)` 注入后 handler 被调用 count=1、`(e2)` 上报值 1.0 == 注入 96/96、`(e3)` 与 `scale_factor()` 一致、`(e5)` 同值再注入计数保持 1（去重生效）、`(e6)` 已采纳缩放保持 1.0；`err.txt` 为 0 字节。人工段（步骤 5-6）未执行：探针报 `PENDING MANUAL`、退出码 3 | | 记 `SKIP` 的原因是前置不满足而非判据失败：本机仅一块物理显示器（`\\.\DISPLAY1` 3840x2160 @150%，`DISPLAY2-5` 的 `stateFlags=0` 即未挂载），无第二个不同缩放的输出可拖，与 TC-WINDOW-008/009 同因。步骤 4 顺带核实了探针的通道分离：输出全走 stdout，stderr 恒空。本例为新增载体（探针而非 demo），故本模块 §1.2/§1.3 已同步登记该载体的独立构建口径与后端例外。⚠️ 取证过程中曾出现一次「自动段 (e5) 转红」的假故障，根因是恢复变异源码后未重编、跑的是变异前的旧 `aurora.lib`（产物 mtime 早于源码 mtime），重建后即恢复全绿——与本仓既有教训一致（记忆恢复后必须重跑验证） |
| TC-WINDOW-014 | 2026-10-12 | WorkBuddy | PASS | | 步骤 2：退出码 0。步骤 3：`content_inset: top=36.0 left=0.0 right=0.0 bottom=0.0`；`band=303030 below=FFFFFF (expect 303030 / FFFFFF)`；恰 7 行 `[PASS]`（client-side 的 inset/带像素/内容零入侵三条 + auto 的 inset/零入侵两条 + frameless 的 inset/零入侵两条），0 行 `[FAIL]`，末行 `checks=7 pass=7`。步骤 4：`err.txt` 为空 | | 自动段三策略映射逐条过；步骤 5 人工目视段待真机会话补记（本会话无指针注入通道）。构建口径为 §1.2 的 `build-verify-x11`（`AURORA_BACKEND_X11=ON -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_VERIFY_TOOLS=ON`） |
| TC-WINDOW-015 | 2026-10-12 | WorkBuddy | PASS | | 步骤 2：退出码 0。步骤 3：末行 `checks=47 pass=47`；ClientSide 段客户区 = 窗口矩形（`WM_NCCALCSIZE` 吞框）、`content_inset` `top=36`、带像素 `#303030` 且内容区 `#FFFFFF` 零入侵、逐点热区回归过（八向 / 标题带 / 按钮→HTCLIENT）、悬停关钮 `#E01B24`、同钮释放触发关窗；热更段带 `#202020`、按压 `#991D11` 覆悬停 `#C42B1C`、拖离松手不关窗且无残留高亮；Auto 段带像素 `#FFFFFF`、窗口顶带仍报 `HTCAPTION`（系统 caption 完好）；降级段左/底缘与角点 `HTCLIENT` 化、角点落入标题带报 `HTCAPTION`、带像素与栏高不变；最大化段客户区 = 显示器工作区、左缘 `HTCLIENT`、带空白 `HTCAPTION`、释放最大化钮完成最大化→还原往返。步骤 4：`err.txt` 为 0 字节 | | 自动段五档映射逐条过；DWM 合成开启；降级判据经 `frame.shadow_margin_dp = 0` 显式注入实测；判据经三次变异（热区门收窄到 csd_border、最大化标题带门加 sizable、win32_surface 删除 `paint_decoration` 调用）逐一转红后恢复复绿，非结构性假绿。步骤 5 人工目视段（拖动/双击/Aero Snap/系统菜单）待真机会话补记。构建口径为本机默认 `build/` 追加 `-DAURORA_BUILD_VERIFY_TOOLS=ON` 重 configure（WIN32/DEBUG 两开关已满足） |
| TC-WINDOW-016 | 2026-10-12 | WorkBuddy | PASS | | 步骤 4：`Cycle title-bar theme` 按钮可见位置点击逐档循环，状态行依次为 `host title-bar theme → AdwaitaLight`、`→ WindowsDark`、`→ WindowsLight`，顶部自绘标题带整带配色随主题逐档重绘。步骤 6：状态行 `window frame shadow → off`。步骤 8：状态行 `title-bar icon → set`，标题带左端出现 24×24 双色图标。步骤 9–11：物理 840×640 缩矮后 `Close this preview` 带从像素扫描中消失，物理 840×940 拉长后于原相对位置重现（bitmap 纵带 y=[698,754]），按该可见位置（屏幕物理 (291,878)）真实点击后窗口立即消失，`err.txt` 新增 `INF` 行 `[app] window host closed and reaped: id=4`。步骤 13：Borderless 预览在绘制位置点击 `Info` 后新增 `INF` 行 `csdg: info clicked`；`−` / `□` / `×` 点击后窗口存活、无新增行。步骤 15：`Close this preview` 后窗口消失并新增回收行 `id=5`。步骤 17：Frameless 同口径复验，`csdg: info clicked` 再新增 1 行、`×` 无动作且窗口存活、`Close this preview` 后新增回收行 `id=6`。步骤 18：全文无 `ERR` / `FTL` | | 取证口径：真机点击经 `SetCursorPos` + `mouse_event` 注入屏幕物理坐标（非 `PostMessage` 直投），落点取 DPI 感知 `PrintWindow` 像素扫描出的按钮带中轴（窗口物理 rect (152,152) 840×780 @ DPI 144 即 150% 缩放）；运行二进制为含 修复的构建（可执行文件 mtime 晚于修复源文件、工作树对该源文件无未提交改动）。平台范围观察：控件级 TitleBar 的 `−` / `□` / `×` 为 Win32 无害空动作（`Surface` 四操作默认空实现、仅 Wayland 生效，见 `specification/07-environment-modifier.md`），命中链本身无恙（同条 `Info` 动作在绘制位置逐次触发日志）。观察性现象：每扇预览窗关闭时稳定新增 1 行 `WRN`（`layout parent detached while the child widget is still alive: Column`，应用根子树解挂序提示），不影响关闭与回收，不记缺陷 |
| TC-WINDOW-019 | | | | | | | |
| TC-WINDOW-020 | | | | | | | |
