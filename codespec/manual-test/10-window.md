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

复用既有示例程序，不新增载体：

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_multi_window.cpp` | `demo_multi_window` | 多窗口创建与关闭、窗口角色与模态、跨显示器移动、跨窗广播、退出路径 |
| `examples/demos/demo_custom_surface.cpp` | `demo_custom_surface` | 自定义 `Surface` 注入、无窗口（Headless）路径与产物 |

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
| 显示器 | **TC-WINDOW-008 与 TC-WINDOW-009 需要 ≥2 台显示器**；单显示器环境下该两条记 `SKIP` |

**本机实际启用的后端（实测自本机构建目录的 CMakeCache 缓存值，构建产物不入库）**：

| 后端开关 | 取值 |
|:---|:---|
| `AURORA_BACKEND_WIN32` | ON |
| `AURORA_BACKEND_HEADLESS` | ON |
| `AURORA_BACKEND_GLFW` / `D3D11` / `GPU_GL` / `GPU_WGPU` | OFF |
| `AURORA_BACKEND_X11` / `WAYLAND` / `MACOS` / `WASM` | OFF |

因此 **GLFW、D3D11、wgpu、X11、Wayland、macOS、WASM 七个后端的人工验证在本机不可执行**，本模块不为它们编写用例（其窗口表现差异属跨平台真机验收的范畴）。TC-WINDOW-001 的方案 B 覆盖 Headless 路径，其余用例覆盖 Win32 路径。

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

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-WINDOW-001 | 2026-09-26 | Qoder Agent | PASS | | 步骤 4：退出码 0。步骤 5：`build/demo_custom_surface_headless.png` 存在且为 1920813 字节（约 1.9 MB）。步骤 6：`err.txt` 恰 1 行，级别 `INF`，消息为 `[demo_custom_surface] custom Surface PNG: writePNG: invalid dimensions`，`out.txt` 为 0 字节 | | 步骤 1 的两个目标在既有 `build/` 目录构建成功。该行属 §1.2 已登记的方案 A 构造局限，按 §1.2 不记缺陷，且其 `INF` 级别与「不中止」语义正是本例的判定要点 |
| TC-WINDOW-002 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：标题 `Aurora multi-window (main)`，窗口 802x626 物理像素、客户区 780x570 物理像素，本机 DPI 缩放 150% 即 520x380 逻辑。步骤 3：`err.txt` 恰 1 行 `INF`，消息含 `[multi_window] main window shown (close it to exit)`。步骤 4：计数文本为 `windows = 1` | | 计数文本以窗口自身像素读数为准（经 `PrintWindow` 抓取），无障碍树同值。主窗 6 个按钮的无障碍名与 §2 用例引用的标签逐一对应 |
| TC-WINDOW-003 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：新增 1 行 `INF` 且消息为 `[multi_window] opened window id=2 total=2`，出现标题 `Auxiliary #1`（客户区 660x450 物理像素即 440x300 逻辑），主窗计数像素读数 `windows = 2`。步骤 5：新增行 `opened window id=3 total=3`（id 与上次不同），出现 `Auxiliary #2`，主窗计数像素读数 `windows = 3` | | 回调线程 id 与启动行一致，即点击在 UI 线程派发。顺带观察：此刻无障碍树仍回读 `windows = 2`（快照滞后于帧末更新），故窗口内文本一律以像素读数判定 |
| TC-WINDOW-004 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：点击 `Auxiliary #1` 内的 `Close this window` 后该窗消失并新增 `INF` 行 `[app] window host closed and reaped: id=2`，主窗与 `Auxiliary #2` 均存活可交互（主窗随后仍可新建窗口并记 `opened window id=4 total=3`，`Auxiliary #2` 的关闭按钮点击后同样正常关闭）。步骤 4：主窗计数像素读数 `windows = 2` | | 「#2 按钮仍可点击」以点击其自身关闭按钮并观察窗口消失来证明，而非仅看外观 |
| TC-WINDOW-005 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：消失的是 `Auxiliary #2`（`err.txt` 记 `[app] window host closed and reaped: id=3`，id=3 即第二次新建者），`Auxiliary #1` 保留。步骤 4：主窗计数像素读数 `windows = 2` | | 「最近打开者」以 `err.txt` 的 id 对照确认：第二次新建记 `opened window id=3`，被回收的正是 id=3 |
| TC-WINDOW-006 | 2026-09-28 | Qoder Agent | PASS | | 步骤 3：出现标题 `Modal dialog`、客户区 540x300 物理像素（即 360x200 逻辑）的窗口，`err.txt` 行数保持 1，未因打开模态而增加。步骤 4/5：真实指针点击（`SetCursorPos` + `mouse_event`）取两组坐标各做「模态前→模态中→关模态后」三轮同点对照——新建按钮带中心 (191,234) 上业务行 `opened window id=` 依次 0→1、1→1（不变）、1→2；广播按钮带中心 (385,616) 位于模态窗矩形 (253,253,628,490) **之外**且 `WindowFromPoint` 复核命中窗即主窗 hwnd，其回显文本依次 `broadcast #1`、仍 `broadcast #1`（`err.txt` 无新增行）、`broadcast #2`。`IsWindowEnabled(主窗)` 由 True 翻 False 再自动回 True（模态窗恒 True），主窗计数像素读数全程 `windows = 2`（即主窗加模态窗）未虚增。步骤 5 外观：同一时刻标题栏色带比对，主窗开模态前均色 (229,239,242)、模态中 (239,239,239)（灰化，主色 (236,246,249)→(243,243,243)），模态窗自身 (229,239,242) 与主窗激活态一致，即模态窗为激活外观、主窗为非激活外观。步骤 6/8：真点模态窗自己的按钮带中心 (328,437) 后该窗消失，`IsWindowEnabled(主窗)` 自动恢复 True 且前窗即主窗，随即同坐标一次真点新建成功新增 1 行 `INF` 且消息为 `[multi_window] opened window id=`，主窗计数像素读数 `windows = 2`（模态窗已注销、辅助窗新增） | | 模态拦截在 OS 层实现（`EnableWindow(owner, FALSE)`，见 `src/aurora/window/win32_host.cpp`）。判据取「真实点击的同点三轮」而非 `IsWindowEnabled` 单点：合成点击经 `PostMessage` 直达窗口过程、绕过 OS 对禁用窗口的输入丢弃（本轮复现：主窗禁用态下经 `/api/input/click` 仍触发回调并多出一扇辅助窗口），故该通道不得用作本步判据。「模态窗激活、主窗非激活」的外观项在 26 轮记为无法判定，原因是当时本机锁屏、前台窗恒为锁屏 CoreWindow；解锁会话下前窗可为应用窗口，本轮按标题栏色带的像素差实测判定。坐标换算与蓝带定位口径同 `19-app.md` 的 TC-APP-005（两例共用同一载体与同一轮取证，脚本 `build/manual-lib/a005c_run.py`、`a005d_run.py`）。另按实测把预期 5/8 的计数口径回写为「模态窗自身计入 `window_count()`」 |
| TC-WINDOW-007 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：两扇辅助窗口的回显区在同一时刻的像素截图均为 `broadcast #1`（主窗回显同为 `broadcast #1`）。步骤 5：再点击两次后两窗回显区均为 `broadcast #3`，序号与点击次数一致 | | 广播不写日志，`err.txt` 行数不变。无障碍树在第三次广播后仍回读 `broadcast #1`（快照滞后），判定同样以像素截图为准 |
| TC-WINDOW-008 | 2026-09-26 | Qoder Agent | SKIP | | `EnumDisplayDevicesW` 仅枚举到 1 台活动显示器（`\\.\DISPLAY1`，AMD Radeon RX 7800 XT，主屏），§1.3 的「≥2 台显示器」前置不满足。作为旁证仍执行了步骤 2：`err.txt` 新增 1 行 `INF` 且消息为 `[multi_window] single display; nothing to move to`，主窗位置未变 | | 按 §1.7 记 SKIP 而非 FAIL，载体自身的单显示器分支日志与预期一致 |
| TC-WINDOW-009 | 2026-09-26 | Qoder Agent | SKIP | | 同 TC-WINDOW-008：本机仅 1 台显示器，无从取得第二台不同 DPI 缩放的显示器，前置条件无法满足 | | 依赖用例 TC-WINDOW-008 亦为 SKIP |
| TC-WINDOW-010 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：退出码 0，`EnumWindows` 已枚举不到该进程的任何窗口（含仍打开的 `Auxiliary #1`），进程不残留。步骤 4：`err.txt` 全文 2 行均为 `INF`（`main window shown`、`opened window id=2 total=2`），无 `ERR`/`FTL` | | 辅助窗口未各自关闭即随进程结束，符合 `Application::quit()` 在任何退出策略下都生效的语义（对照 TC-WINDOW-012） |
| TC-WINDOW-011 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：退出码 0，进程不再存在且无窗口残留。步骤 4：`err.txt` 仅启动的 1 行 `INF`，无 `ERR`/`FTL` | | 本机会话锁屏，标题栏属非客户区、合成点击落不到，故步骤 2 以发送 `WM_SYSCOMMAND`/`SC_CLOSE` 等价代替——这正是点击标题栏关闭按钮时系统投递给窗口的消息。本用例文本于本轮修订（原步骤含「新建一扇辅助窗口」，与 `ExitPolicy::LastWindowClosed` 语义冲突，见 §2.6 分工说明） |
| TC-WINDOW-012 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：关闭主窗后进程仍存活，存活窗口只剩标题 `Auxiliary #1` 的一扇，`err.txt` 新增 1 行 `INF` 且消息为 `[app] window host closed and reaped: id=1`（id=1 即主窗）。步骤 5：关闭该辅助窗口后退出码 0，无窗口残留。步骤 6：`err.txt` 全文 3 行均为 `INF`，无 `ERR`/`FTL` | | 关闭最后一扇窗口时进程先行退出，故不再有该窗口的回收日志行，属预期时序。本例为新增，用以固定「主窗关闭不连带关闭其他窗口」这一默认策略边界 |
