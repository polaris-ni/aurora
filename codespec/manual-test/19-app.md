# app 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`app/` —— 应用与窗口聚合层（20 个公共头），在实测依赖分层中位于 L7（最上层之一）。

该模块把「一个或多个窗口」「场景树与焦点」「帧循环」「事件总线」「系统级集成（托盘、热重载、剪贴板、无头测试控制器）」收束为一个可启动的应用单元。它的可观测行为大多落在**真实窗口与真实桌面**上：窗口可见性、多窗口独立性、模态拦截、通知区域图标与气泡、系统剪贴板互操作——这些都无法在无头单测中复现。

公共头清单：`application.h` `clipboard.h` `display.h` `file_dialog.h` `generate_ui.h` `hot_reload.h` `menu.h` `perf_overlay.h` `scene.h` `scheduler.h` `scroll_storage.h` `shortcuts.h` `system_tray.h` `test_controller.h` `ui_prompt.h` `validate.h` `validate_ui.h` `window_bus.h` `window_geometry.h` `window_host.h`。

### 1.2 执行载体

复用既有示例程序，并**新增**两个载体。全部载体统一在 `TC-APP-001` 步骤 1 一次构建：

| 载体 | 构建目标 | 状态 | 用途 |
|:---|:---|:---|:---|
| `examples/demos/demo_app.cpp` | `demo_app` | 复用 | `au::App()` 一行式启动基线 |
| `examples/demos/demo_app_lifecycle.cpp` | `demo_app_lifecycle` | 复用 | 窗口可见性/几何态驱动的动画暂停与恢复 |
| `examples/demos/demo_multi_window.cpp` | `demo_multi_window` | 复用 | 多窗口独立帧循环、跨窗总线广播、模态拦截 |
| `examples/demos/demo_test_controller.cpp` | `demo_test_controller` | 复用 | 无头「渲染 → 交互 → 断言」闭环 |
| `examples/demos/demo_hot_reload.cpp` | `demo_hot_reload` | **新增** | 编辑 `ui.json` 即时重建控件树 |
| `examples/demos/demo_system_tray.cpp` | `demo_system_tray` | **新增** | 通知区域图标、气泡、右键菜单与激活回调 |
| `examples/demos/demo_timers.cpp` | `demo_timers` | 复用 | 无 `Scheduler` 时定时器的降级契约 |
| `examples/demos/demo_rich_text_edit.cpp` | `demo_rich_text_edit` | 复用 | 真实系统剪贴板与外部应用互操作 |

**新增载体的理由**：`demo_hot_reload` 与 `demo_system_tray` 是本次为本系列新增——「编辑文件即时生效」与「通知区域图标 / 气泡 / 右键菜单」这两项能力此前**无任何交付载体**，且在人工判定上不可替代：前者需观察「保存文件 → 界面刷新」的因果链，后者需在真实桌面通知区域观察图标是否出现、气泡是否弹出、右键菜单是否可点。

关于各载体的观测量与已知现象：

- `demo_app` 走 `au::App()` 流式路径，**不经** `run_demo` 启动器，故 stdout 与 stderr 均为空；这是它与其余载体的基线差异。
- `demo_test_controller` 要求构建开关 `AURORA_BACKEND_HEADLESS` 为 ON（默认）；开关关闭时载体编译另一分支，仅打印 `demo_test_controller: AURORA_BACKEND_HEADLESS 未开启，跳过` 并返回 0。
- `demo_system_tray` 的真实实现是 **Win32 专有**（`Shell_NotifyIcon` + 隐藏消息窗口）；非 Windows / Headless 下 `SystemTray` 全部方法为 no-op。
- `demo_timers` 经 `run_demo` 只建窗口、**不建** `Application`，故没有 `Scheduler`，其定时器不触发；库的表现为「降级 + 明确诊断」。
- `demo_hot_reload` 读写**当前工作目录**下的 `ui.json`，文件不存在时自动写入一份默认内容。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja） |
| 构建 | 库已构建；八个载体均为按需目标，不在默认构建内 |
| 终端 | 支持 UTF-8 输出 |
| 分离输出 | 诊断日志写 stderr |
| 指针与键盘 | 真实鼠标与键盘 |
| 桌面通知区域 | TC-APP-010 与 TC-APP-011 需要 Windows 通知区域可用；非 Windows / Headless 环境下这两条记 `SKIP` |
| 系统剪贴板 | TC-APP-012 需要真实系统剪贴板，且本机装有记事本 |
| 文件访问 | TC-APP-007 至 TC-APP-009 需可读写当前工作目录下的 `ui.json` |

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `app`，即 `TC-APP-001` 起，本模块编号正则为 `^TC-APP-\d{3}$`。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
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

**模块级约定**：本模块全部载体的构建步骤统一置于 `TC-APP-001` 的步骤 1；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。

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

本模块按「人工判定是否优于程序判定」取舍，只覆盖人工判定占优的子域，不追求对 20 个头的全覆盖。

**纳入人工测试的子域**：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 一行式启动基线 | `au::App()` 不带启动器日志这一差异，只有在真实进程的 stdout/stderr 上才能核对 |
| 可见性驱动的动画暂停 | 「最小化后计数停止、恢复后继续」是跨时间的端到端事实，无头单测看不到真实窗口状态迁移 |
| 多窗口独立性 | 关闭一扇辅窗是否波及其余窗口，需多窗口同时可见 |
| 模态拦截 | 模态期间输入被屏蔽、关闭后自动恢复，需真实输入设备 |
| 跨窗广播 | 一次广播是否送达**所有**窗口，需多窗口同时可见 |
| 无头测试闭环 | TestController 的逐行输出与退出码是 CI 契约，需真实进程输出 |
| 热重载 | 「保存文件 → 重建树」是文件系统与帧循环的耦合，且「非法 JSON 不破坏旧树」是控制流不变量 |
| 系统托盘 | 通知区域图标是否出现、气泡是否弹出、右键菜单是否可点，只能在真实桌面观察 |
| 系统剪贴板互操作 | 「复制到记事本、从记事本粘贴回来」验证的是**系统**剪贴板边界，进程内缓冲测不出 |
| 定时器降级 | 「无 `Scheduler` 时不触发并打印诊断」是降级契约，需确认进程不静默、不崩溃 |

**不纳入人工测试的头及其理由**：

| 头 | 理由 | 已有覆盖 |
|:---|:---|:---|
| `clipboard.h` | 无独立载体；真实系统剪贴板路径由 TC-APP-012 经 `demo_rich_text_edit` 的 Ctrl+C / Ctrl+X / Ctrl+V 覆盖，进程内语义由内存测试后端全覆盖 | `tests/unit/utest_clipboard.cpp` |
| `file_dialog.h` `ui_prompt.h` `validate_ui.h` | 弹出真实系统对话框/提示，属 GUI 交互，项目约定不引入 GUI 交互测试；`file_dialog` 的 headless 钩子与取消路径由单测覆盖 | `tests/unit/utest_file_dialog.cpp` |
| `validate.h` `generate_ui.h` | UI 树校验与代码生成语义由 `aurora_cli` 的 `validate` / `to-code` 子命令与 golden 基准覆盖 | `tests/golden/`；另见 `codespec/manual-test/11-widget.md` |
| `shortcuts.h` | 键组合匹配语义由单测全覆盖；端到端按键派发已作为 `codespec/manual-test/21-debug.md` 的载体出现 | `tests/unit/utest_shortcuts.cpp` |
| `scene.h` `window_host.h` `window_geometry.h` `display.h` | 结构与几何模型，可观测行为已由 window 模块文档覆盖；多显示器迁移在本机单显示器环境不可复现（载体明确日志 `single display; nothing to move to`） | `codespec/manual-test/10-window.md` |
| `menu.h` | `MenuItem` 语义（含分隔符）由单测覆盖；其端到端表现由托盘用例 TC-APP-011 在真实通知区域观察 | `tests/unit/utest_system_tray.cpp` |
| `perf_overlay.h` | 叠层绘制与性能门禁由 perf 模块文档覆盖 | `codespec/manual-test/17-perf.md` |
| `application.h` 的异步/音频接线 | 异步任务与音频设备接线分别由 state 与 media 模块文档覆盖 | `codespec/manual-test/07-state.md`、`codespec/manual-test/16-media.md` |

`demo_hot_reload` 与 `demo_system_tray` 为本次新增载体；`demo_timers` 复用既有载体，其「无 `Scheduler` 时定时器不触发」是被**刻意纳入**的降级契约观测点，而非缺陷假设。

## 2 用例清单

### 2.1 载体基线与一行式启动

#### TC-APP-001 载体构建与 `au::App()` 一行式启动基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-001 |
| 测试目的 | 确认全部载体可一次构建，且 `au::App()` 一行式启动路径不产生任何启动器日志 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；本机工具链可用 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建全部载体：`cmake --build build --target demo_app demo_app_lifecycle demo_multi_window demo_test_controller demo_hot_reload demo_system_tray demo_timers demo_rich_text_edit`（纯执行，无预期结果）<br>2. 启动 `./build/demo_app.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>3. 查看窗口标题与客户区尺寸<br>4. 查看窗口内的两行文本<br>5. 查看 `out.txt` 与 `err.txt` 的内容 |
| 预期结果 | 3. 标题为 `au::App() fluent wrapper`，客户区约为 420×300<br>4. 首行为 `au::App() demo`，次行为 `surface: <整数>`（`<整数>` 为当前后端标识）<br>5. `out.txt` 与 `err.txt` **均为空**（0 行）——该载体走 `au::App()` 流式路径，不经 `run_demo` 启动器，故无任何 `[run_demo]` 日志；这正是它与其余载体的基线差异 |

### 2.2 窗口可见性与多窗口独立性

#### TC-APP-002 窗口可见性切换驱动动画暂停与恢复

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-002 |
| 测试目的 | 验证窗口进入隐藏态时动画暂停、回到可见态时恢复，并核对窗口几何态日志 |
| 前置条件 | 载体 `demo_app_lifecycle` 已构建成功；鼠标与键盘可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_app_lifecycle.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题与客户区尺寸<br>3. 查看 `err.txt` 的启动阶段内容，并读取窗口内的 `WindowState`、`WindowMode`、`ticks` 三行读数<br>4. 记录 `ticks` 当前值，等待约 1 秒后再次读取<br>5. 最小化窗口，读取两行读数并查看 `err.txt` 的新增行<br>6. 保持最小化约 1 秒后读取 `ticks`<br>7. 从任务栏恢复窗口，读取两行读数并查看 `err.txt` 新增行；等待约 1 秒后再次读取 `ticks`<br>8. 最大化窗口，读取两行读数并查看 `err.txt` 新增行 |
| 预期结果 | 2. 标题为 `App Lifecycle · Aurora Demo`，客户区约为 520×440<br>3. 启动阶段 `err.txt` **为空**（窗口几何态尚未变化）；三行读数分别形如 `WindowState = <态名>`、`WindowMode = <态名>`、`ticks = N`<br>4. `ticks` 在非暂停时以约 200ms 为周期递增，等待约 1 秒后读数明显增大<br>5. `WindowState = Hidden` 且 `WindowMode = Minimized`（最小化**同时**置两态）；`err.txt` 新增**恰 1 行** `INF` 级 `[AppLifecycle] WindowMode -> Minimized`<br>6. `ticks` **停止递增**（载体把 `WindowState != Visible` 视为暂停动画），读数保持步骤 5 时的值<br>7. `WindowState` 回到 `Visible`、`WindowMode` 回到 `Normal`；`err.txt` 新增**恰 1 行** `INF` 级 `[AppLifecycle] WindowMode -> Normal`；等待约 1 秒后 `ticks` **重新开始递增**<br>8. `WindowMode = Maximized` 时 `WindowState` **仍为 `Visible`**（最大化不改变可见性）；`err.txt` 新增**恰 1 行** `INF` 级 `[AppLifecycle] WindowMode -> Maximized` |

#### TC-APP-003 多窗口独立帧循环：关闭辅窗不影响主窗

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-003 |
| 测试目的 | 验证每扇辅助窗口拥有独立生命周期，关闭其一不波及主窗与其余窗口 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看主窗标题、客户区尺寸与 `err.txt` 的启动行<br>3. 在主窗点击 `New auxiliary window` 两次，查看新窗口标题、`err.txt` 新增行与主窗计数文本<br>4. 在 `Auxiliary #1` 窗口内点击 `Close this window`（纯执行，无预期结果）<br>5. 观察主窗与 `Auxiliary #2` 的存活与可交互性，并读取主窗计数文本<br>6. 在 `Auxiliary #2` 窗口内点击 `Close this window`，再次观察主窗并读取计数文本 |
| 预期结果 | 2. 主窗标题为 `Aurora multi-window (main)`，客户区约为 520×380；`err.txt` 恰 1 行 `INF` 级 `[multi_window] main window shown (close it to exit)`<br>3. 出现标题为 `Auxiliary #1` 与 `Auxiliary #2` 的两扇窗口（各自约 440×300，role=Auxiliary）；`err.txt` 新增 2 行，形如 `[multi_window] opened window id=<id> total=<n>`；主窗计数文本变为 `windows = 3`<br>5. `Auxiliary #1` 消失；主窗与 `Auxiliary #2` 仍正常显示，`Auxiliary #2` 内的 `Close this window` 按钮仍可点击（关闭不影响其它窗口）；主窗计数文本约一帧后变为 `windows = 2`<br>6. `Auxiliary #2` 消失，**主窗仍存活且可交互**（辅窗关闭不影响主窗的帧循环）；计数文本约一帧后变为 `windows = 1` |

### 2.3 跨窗事件与模态

#### TC-APP-004 跨窗事件总线广播送达所有窗口

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-004 |
| 测试目的 | 验证经窗口总线发布的一次通知能被主窗与全部辅窗同时收到 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe`，并新建两扇辅助窗口（纯执行，无预期结果）<br>2. 读取主窗与两扇辅窗的回显文本初值<br>3. 在主窗点击 `Broadcast to all windows (bus)` 一次（纯执行，无预期结果）<br>4. 同时查看三扇窗口的回显文本<br>5. 再点击该按钮两次，再次查看三扇窗口的回显文本 |
| 预期结果 | 2. 三扇窗口的回显文本均为 `(no broadcast yet)`<br>4. **主窗与两扇辅窗**的回显文本**同时**变为 `broadcast #1`（经 `app.bus().post(...)` 送达全部窗口，而非仅发送者）<br>5. 三扇窗口的回显文本均变为 `broadcast #3`（序号随点击累加，与总点击次数一致） |

#### TC-APP-005 模态窗打开期间屏蔽主窗输入

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-005 |
| 测试目的 | 验证模态窗口打开期间拥有者主窗的输入被屏蔽，关闭后无需额外操作即自动恢复 |
| 前置条件 | 载体 `demo_multi_window` 已构建成功；鼠标可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_multi_window.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在主窗点击 `Open modal dialog (blocks main window)`（纯执行，无预期结果）<br>3. 查看新窗口标题、客户区尺寸与 `err.txt` 的行数<br>4. 在主窗内点击 `New auxiliary window`（纯执行，无预期结果）<br>5. 检查主窗计数文本与 `err.txt` 是否变化，并观察两窗的激活外观<br>6. 在模态窗口内点击 `Close modal`（纯执行，无预期结果）<br>7. 再次在主窗内点击 `New auxiliary window`，检查主窗计数文本与 `err.txt` |
| 预期结果 | 3. 出现标题为 `Modal dialog`、客户区约 360×200 的窗口（role=Transient、owner 为主窗、modal=true）；`err.txt` 行数**不因打开模态而增加**（打开模态不产生日志）<br>5. 主窗计数文本保持打开模态前的值；`err.txt` **无新增行**（主窗输入确被屏蔽，按钮回调未执行）；模态窗为激活外观、主窗为非激活外观<br>7. 主窗计数文本增加 1、`err.txt` 新增 1 行 `INF` 级 `[multi_window] opened window id=`（关闭模态后输入自动恢复，无需额外点击激活） |

### 2.4 无头测试闭环

#### TC-APP-006 TestController 无头闭环自断言

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-006 |
| 测试目的 | 验证无头「渲染 → 交互 → 断言」闭环的逐行输出与退出码符合契约 |
| 前置条件 | 载体 `demo_test_controller` 已构建成功；构建开关 `AURORA_BACKEND_HEADLESS` 为 ON（默认） |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 运行 `./build/demo_test_controller.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 记录进程退出码<br>3. 查看 `err.txt` 的内容与行数<br>4. 逐行核对 `out.txt` 的行数与内容<br>5. 核对 `out.txt` 第 2 行三个字段之间的空白形态 |
| 预期结果 | 2. 退出码为 0（断言全部通过后自行退出，不进入事件循环）<br>3. `err.txt` 为 0 行（空文件，无诊断日志）<br>4. `out.txt` **恰 7 行**，依次为 `frames after first pump: 1`、`find_by_key("tap")=1  find_by_key("name")=1  find_by_type("Button")=1`、`pump_and_settle frames: 2, total frames: 2`、`counter matches 'clicks = 2': yes`、`expect_visible(button): pass`、`expect_prop(input, value='Aurora'): pass`、`demo PASSED`<br>5. 第 2 行三个字段之间各为**两个空格**（`find_by_key("tap")=1`、`find_by_key("name")=1`、`find_by_type("Button")=1` 两两之间各 2 个空格），单空格即 FAIL |

### 2.5 热重载

#### TC-APP-007 热重载：保存 ui.json 触发整树重建

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-007 |
| 测试目的 | 验证 `ui.json` 内容变化会在下一帧重建整棵控件树并记录一条重建日志 |
| 前置条件 | 载体 `demo_hot_reload` 已构建成功；当前工作目录可写（载体读写该目录下的 `ui.json`） |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_hot_reload.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题、客户区尺寸与 `err.txt` 的行数<br>3. 查看当前工作目录下的 `ui.json`，确认其描述的控件类型与文本<br>4. 修改 `ui.json` 中第一个子节点（`Text`）的内容并保存（纯执行，无预期结果）<br>5. 观察窗口内文本是否更新，并查看 `err.txt` 新增行<br>6. 再次修改 `ui.json` 的文本内容并保存（纯执行，无预期结果）<br>7. 再次查看窗口内文本与 `err.txt` 新增行 |
| 预期结果 | 2. 标题为 `Hot reload · Aurora Demo`，客户区约为 640×300；首次启动且 `ui.json` 不存在时载体自动写入默认内容，`err.txt` 恰 1 行 `INF` 级 `[hot_reload] wrote default ui.json -- edit it to trigger a reload`（`ui.json` 已存在时该行为 0 行）<br>3. `ui.json` 描述一棵 `Column`：子节点 0 为 `Text`，内容为 `Edit ui.json, save, and this text is rebuilt live.`；子节点 1 为 `TextInput`，占位符为 `Type here first, then edit ui.json`，且**刻意未声明** `value`<br>5. 窗口内 `Text` 文本随保存内容更新；`err.txt` 新增**恰 1 行** `INF` 级 `[hot_reload] tree rebuilt from ui.json`<br>7. 再次出现**恰 1 行** `[hot_reload] tree rebuilt from ui.json`，且窗口文本反映最新内容（每次文件变化对应 1 行重建日志） |

#### TC-APP-008 热重载：JSON 未声明的属性按树路径保留

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-008 |
| 测试目的 | 验证重建时按树路径回填 JSON 未显式声明的标量属性，已键入的输入内容不丢失 |
| 前置条件 | 载体 `demo_hot_reload` 已构建成功；当前工作目录下的 `ui.json` 已存在并可写 |
| 依赖用例 | TC-APP-001, TC-APP-007 |
| 操作步骤 | 1. 启动 `./build/demo_hot_reload.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在窗口内的 `TextInput` 中输入一段文本（如 `Aurora`），不修改 `ui.json`（纯执行，无预期结果）<br>3. 读取输入框当前显示的文本<br>4. 修改 `ui.json` 中 `Text` 子节点的内容并保存，**不触碰** `TextInput` 节点（纯执行，无预期结果）<br>5. 检查窗口内 `Text` 文本与 `TextInput` 输入框内容，并查看 `err.txt` 新增行 |
| 预期结果 | 3. 输入框显示步骤 2 键入的文本（`TextInput` 节点未在 JSON 中声明 `value`，其界面文本属运行期状态）<br>5. `Text` 已按新的 JSON 内容重建；`TextInput` **仍显示步骤 2 键入的文本**（重建时按**树路径**回填 JSON 未显式声明的标量属性，故已键入内容保留）；`err.txt` 新增**恰 1 行** `INF` 级 `[hot_reload] tree rebuilt from ui.json` |

#### TC-APP-009 热重载：非法 JSON 不破坏当前树

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-009 |
| 测试目的 | 验证写入非法 JSON 时不重建、不产生错误日志、不崩溃，旧树保持不动 |
| 前置条件 | 载体 `demo_hot_reload` 已构建成功；当前工作目录下的 `ui.json` 已存在并可写 |
| 依赖用例 | TC-APP-001, TC-APP-007 |
| 操作步骤 | 1. 启动 `./build/demo_hot_reload.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 记录窗口内 `Text` 的内容与 `err.txt` 的行数<br>3. 将 `ui.json` 覆盖为一段非法 JSON（如故意缺失闭合括号）并保存（纯执行，无预期结果）<br>4. 观察窗口内 `Text` 是否变化，并查看 `err.txt` 的行数<br>5. 关闭窗口并记录进程退出码 |
| 预期结果 | 2. `Text` 显示上一次合法内容（默认内容或上一次重建结果）；`err.txt` 行数为已知基线<br>4. 窗口内 `Text` **保持步骤 2 的内容不变**（非法内容不重建、旧树保持不动）；`err.txt` **行数不变**（不重建，且无 `ERR` / `FTL` 级日志）<br>5. 退出码为 0；进程在非法 JSON 期间始终存活，未崩溃（非法输入不中止） |

### 2.6 系统托盘

#### TC-APP-010 系统托盘：图标、气泡与激活回调

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-010 |
| 测试目的 | 验证通知区域图标出现、窗口按钮与左键单击各自触发气泡与激活回调 |
| 前置条件 | 载体 `demo_system_tray` 已构建成功；本机为 Windows 且通知区域可用；鼠标可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_system_tray.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题、客户区尺寸与 `err.txt` 启动行，并确认通知区域出现托盘图标<br>3. 清点窗口内按钮与首行回显文本<br>4. 在窗口内点击 `Show balloon`（纯执行，无预期结果）<br>5. 查看窗口首行回显文本与系统气泡<br>6. 左键单击通知区域中的托盘图标（纯执行，无预期结果）<br>7. 查看气泡标题与正文，并查看 `err.txt` 新增行 |
| 预期结果 | 2. 标题为 `System tray · Aurora Demo`，客户区约为 520×340；`err.txt` 恰 1 行 `INF` 级 `[tray] window shown; look for the icon in the notification area`；通知区域出现该应用图标<br>3. 窗口内三枚按钮为 `Show balloon`、`Hide tray icon`、`Show tray icon`；首行回显文本为 `(no balloon yet)`<br>5. 回显文本变为 `Balloon from the window`，并弹出标题 `Aurora`、正文 `Balloon from the window` 的气泡；`err.txt` 新增 1 行 `INF` 级 `[tray] balloon shown`<br>7. 气泡标题为 `Aurora`、正文为 `Icon activated`；`err.txt` 新增 `INF` 级行，含 `[tray] balloon shown` 与 `[tray] activated`（左键单击命中 `on_activate`） |

#### TC-APP-011 系统托盘：右键菜单与自菜单退出

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-011 |
| 测试目的 | 验证托盘右键菜单的项序、图标隐藏/显示与自菜单退出路径 |
| 前置条件 | 载体 `demo_system_tray` 已构建成功；本机为 Windows 且通知区域可用；鼠标可用 |
| 依赖用例 | TC-APP-001, TC-APP-010 |
| 操作步骤 | 1. 启动 `./build/demo_system_tray.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 在通知区域图标上右键单击，展开上下文菜单（纯执行，无预期结果）<br>3. 查看菜单项及其顺序与分隔符<br>4. 点击菜单中的 `Show balloon`（纯执行，无预期结果）<br>5. 查看窗口回显文本与气泡正文<br>6. 再次右键展开菜单，依次点击 `Hide icon` 与 `Show icon`（纯执行，无预期结果）<br>7. 查看通知区域图标的变化与 `err.txt` 新增行<br>8. 再次右键展开菜单，点击 `Quit`（纯执行，无预期结果）<br>9. 查看 `err.txt` 新增行并记录进程退出码 |
| 预期结果 | 3. 菜单项自上而下为 `Show balloon`、分隔符、`Hide icon`、`Show icon`、分隔符、`Quit`<br>5. 窗口回显文本与气泡正文均为 `Balloon from the tray context menu`；`err.txt` 新增 1 行 `INF` 级 `[tray] balloon shown`<br>7. 点击 `Hide icon` 后通知区域图标消失、`err.txt` 新增 1 行 `INF` 级 `[tray] icon hidden`；点击 `Show icon` 后图标重新出现、`err.txt` 新增 1 行 `INF` 级 `[tray] icon shown`<br>9. `err.txt` 新增 1 行 `INF` 级 `[tray] quit requested from tray menu`；随后应用退出，退出码为 0 |

### 2.7 系统剪贴板互操作

#### TC-APP-012 真实系统剪贴板与外部应用互操作

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-012 |
| 测试目的 | 验证复制/剪切/粘贴走真实系统剪贴板，可与外部应用双向互操作 |
| 前置条件 | 载体 `demo_rich_text_edit` 已构建成功；本机装有记事本；鼠标与键盘可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_rich_text_edit.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题与 `err.txt` 启动行<br>3. 在文本框内键入一段可识别文本（如 `AuroraClip`），选中并按 Ctrl+C（纯执行，无预期结果）<br>4. 打开记事本按 Ctrl+V，检查粘贴出的内容<br>5. 在记事本中键入另一段可识别文本（如 `FromNotepad`），选中并按 Ctrl+C（纯执行，无预期结果）<br>6. 回到 `RichTextEdit` 窗口，将光标置于末尾按 Ctrl+V，检查文本框内容<br>7. 在 `RichTextEdit` 中选中一段文本并按 Ctrl+X，再回到记事本按 Ctrl+V 检查 |
| 预期结果 | 2. 标题为 `RichTextEdit · Aurora Demo`；`err.txt` 恰 1 行 `INF` 级 `[run_demo] window shown: RichTextEdit · Aurora Demo(close window to exit)`<br>4. 记事本粘贴出的内容与步骤 3 复制的内容一致（Ctrl+C 确实写入**系统**剪贴板，而非进程内缓冲）<br>6. 文本框末尾追加了 `FromNotepad`（Ctrl+V 读取的是系统剪贴板，外部应用写入的内容可被读回）<br>7. 记事本粘出的内容等于 `RichTextEdit` 中被剪切的那段文本，且该文本在 `RichTextEdit` 中已被移除（Ctrl+X 生效，剪切内容进入系统剪贴板） |

### 2.8 定时器降级

#### TC-APP-013 无 Scheduler 时 Timer 降级为不触发并给出诊断

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-APP-013 |
| 测试目的 | 验证缺少 `Scheduler` 时定时器不触发但进程降级而非中止，并给出明确诊断 |
| 前置条件 | 载体 `demo_timers` 已构建成功；鼠标可用 |
| 依赖用例 | TC-APP-001 |
| 操作步骤 | 1. 启动 `./build/demo_timers.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题与客户区尺寸<br>3. 查看 `err.txt` 的行数与内容构成<br>4. 逐行核对 `WRN` 级诊断 JSON 的 `where` 字段<br>5. 观察窗口内时钟与倒计时文本，等待约 3 秒后再次观察 |
| 预期结果 | 2. 标题为 `Timer · Aurora Demo`，客户区约为 520×440<br>3. `err.txt` 共 4 行：首行为 `INF` 级 `[run_demo] window shown: Timer · Aurora Demo(close window to exit)`，其余**恰 3 行**均为 `WRN` 级诊断 JSON<br>4. 3 行 JSON 均含 `"where":"Timer mounted without a running Scheduler (Scheduler::current() == nullptr); ticks will not fire."`（每个计时器各产生 1 条诊断）<br>5. 时钟文本保持初值 `clock: --:--:--`、倒计时保持 `countdown: 10`，且约 2 秒后 `auto-revealed after 2s!` 始终不出现——定时器**未触发**（该载体经 `run_demo` 只建窗口、不建 `Application`，故无 `Scheduler`）；库的表现为「降级 + 明确诊断」，进程不静默、不崩溃 |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-APP-001 | 2026-09-27 | Qoder Agent | PASS | | 步骤 1：八个目标在同一次 `cmake --build build` 调用内全部构建成功（`demo_app` `demo_app_lifecycle` `demo_multi_window` `demo_test_controller` `demo_hot_reload` `demo_system_tray` `demo_timers` `demo_rich_text_edit` 八个 exe 均在位，时间戳为本次执行）。步骤 3：标题 `au::App() fluent wrapper`，客户区 420x300 像素。步骤 4：首行 `au::App() demo`，次行 `surface: 1`。步骤 5：`out.txt` 与 `err.txt` 均 0 行（空文件） | | 步骤 4 的文本需临时开取证通道（设 `AURORA_INSPECTOR_PORT`）才能读到，那次运行 stderr 只多 inspector 自身的日志行；步骤 5 的「双 0 行」取自未开该通道的原始运行，两者是两次独立启动。该载体走 `au::App()` 流式路径、不经 `run_demo`，故确无任何 `[run_demo]` 启动行，此即它与其余载体的基线差异。截图 `APP_001_win.png` |
| TC-APP-002 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：标题 `App Lifecycle · Aurora Demo`，客户区 520x440 像素。步骤 3：不开取证通道的原始运行启动阶段 stderr 为 0 行；三行读数形如 `WindowState = Visible`、`WindowMode = Normal`、`ticks = 15`。步骤 4：约 1 秒后 `ticks = 20`，增量 5，与 200ms 周期相称。步骤 5：`err.txt` 新增**恰 1 行** `INF` 且消息 `[AppLifecycle] WindowMode -> Minimized`，同时 OS 侧 `IsIconic` 为 True。步骤 6：保持最小化约 1.2 秒后 `ticks` 仍为 20，增量 0（动画确已暂停）。步骤 7：`IsIconic` 回到 False，新增恰 1 行 `WindowMode -> Normal`，其后约 1 秒 `ticks` 由 26 增至 31。步骤 8：读数 `WindowMode = Maximized` 而 `WindowState` 仍为 `Visible`，客户区变 3840x2054 像素，新增恰 1 行 `WindowMode -> Maximized`。退出码 0 | | 最小化期间窗口内两行读数停在 `Visible` 与 `Normal`：该读数画在客户区上，而「隐藏即暂停帧循环」正是本例要验的行为，暂停期内不会有新的重绘来刷新它，故 `WindowState = Hidden` 改由两条独立判据佐证——OS 的 `IsIconic` 为 True，以及 `include/aurora/window/window_state.h` 的 `compute_window_state(minimized=true)` 恒返回 `Hidden`。最小化与最大化经 `ShowWindow` 触发（锁屏会话下真实光标不可用，该调用与点标题栏按钮进入同一条窗口状态机路径）。该载体直连 `create_native_window`、不经 `run_demo`，故未开 DPI 感知，客户区像素数等于逻辑尺寸；经 `run_demo` 的其余载体在本机 150% 缩放下为逻辑尺寸的 1.5 倍。截图 `APP_002_1_baseline.png` 等三张 |
| TC-APP-003 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：主窗标题 `Aurora multi-window (main)`，客户区 780x570 像素（150% 缩放下的 520x380 逻辑）；`err.txt` 恰 1 行 `INF` 且消息 `[multi_window] main window shown (close it to exit)`；计数文本初值 `windows = 1`。步骤 3：出现标题 `Auxiliary #1` 与 `Auxiliary #2`，各 660x450 像素（即 440x300 逻辑）；新增 2 行 `INF`，依次为 `opened window id=2 total=2` 与 `opened window id=3 total=3`；主窗计数文本 `windows = 3`。步骤 5：#1 消失后窗口清单只剩 `Auxiliary #2` 与主窗，存活窗口 id 由 1/2/3 变为 1/3（消亡的正是被关的那扇，另一扇未波及），主窗计数文本 `windows = 2`。步骤 6：再关 #2 后只剩主窗，计数文本 `windows = 1`，主窗仍存活且其按钮回调随后仍可触发；退出码 0 | | 点击经 inspector 的 `/api/input/click` 进程内派发（不需前台，锁屏会话下唯一可用通道），但该路由只寻址主窗根节点，辅窗自身的 `Close this window` 按钮不可经它触达：故 #1 的关闭改由向其窗口过程发 `WM_CLOSE`（点该按钮最终请求的即同一关闭路径），#2 的关闭改点主窗的 `Close last auxiliary window`，并用 `opened window id=3` 与消亡窗口的对应关系确认关的是最近那扇。原生旁证：两扇辅窗的 `GetWindow(GW_OWNER)` 与 `GWLP_HWNDPARENT` 均为 None，即辅助窗不依附主窗，生命周期各自独立。计数文本以窗口自身像素读数为准。截图 `APP_003_1_two_aux.png` 等三张 |
| TC-APP-004 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：主窗与两扇辅窗（id 1/2/3）的回显文本均为 `(no broadcast yet)`。步骤 4：一次点击后，同一次采样里三扇窗口的回显文本**同时**为 `broadcast #1`。步骤 5：再点两次后三窗均为 `broadcast #3`，序号与累计点击次数一致 | | 逐窗读数走 `/api/tree?window=<id>`，三窗结果取自同一轮采样，故「同时」是可核对的而非事后追认。广播路径不写日志，`err.txt` 行数不变。收尾只关主窗，进程因仍剩两扇辅窗而继续存活（默认 `ExitPolicy::LastWindowClosed` 语义，与 `10-window.md` 的 TC-WINDOW-012 一致），最终由执行脚本终止，故末次退出码 1，不属本例断言项。截图 `APP_004_broadcast3.png` |
| TC-APP-005 | 2026-09-28 | Qoder Agent | PASS | | 步骤 3：出现标题 `Modal dialog`、客户区 540x300 像素（即 360x200 逻辑）的窗口，其 `GetWindow(GW_OWNER)` 与 `GWLP_HWNDPARENT` 都指向主窗 hwnd；`err.txt` 行数保持 1，未因打开模态而增加。步骤 4/5 的真实指针点击（解锁会话下 `SetCursorPos` + `mouse_event`，非合成通道）分两组坐标各做三轮对照：① 新建按钮带中心 (191,234)——模态前 `[multi_window] opened window id=` 业务行 0→1（记 `opened window id=2 total=2`），模态中同点 1→1 不变且计数文本保持 `windows = 2`，关模态后同点 1→2（记 `opened window id=4 total=2`）；该轮模态窗矩形 (127,127,502,364) 恰好盖住此点，故另设② 排除「点被上层窗口吃掉」：改点主窗最下方 `Broadcast to all windows (bus)` 带中心 (385,616)，它在模态窗矩形 (253,253,628,490) **之外**、`WindowFromPoint` 复核命中窗就是主窗 hwnd，模态前真点一次回显文本变 `broadcast #1`，模态中同点再真点回显**仍是** `broadcast #1` 且 `err.txt` 无新增行，关模态后同点变 `broadcast #2`。② 全程 `IsWindowEnabled(主窗)` 由 True 翻 False 再自动回 True（模态窗恒 True），计数文本无一处虚增。步骤 5 的激活外观：同一时刻抓帧比对标题栏色带，主窗打开模态前主色 (236,246,249)、带均色 (229,239,242)，模态中变主色 (243,243,243)、均色 (239,239,239)（灰化），而模态窗自身标题栏均色 (229,239,242) 与主窗模态前一致——即模态窗为激活外观、主窗为非激活外观。步骤 6/7：真点模态窗自己的按钮带中心 (328,437) 后该窗消失、`IsWindowEnabled(主窗)` 自动回到 True 且前窗即主窗（`FG=主窗 hwnd`），随后在同坐标一次真点即恢复生效，中间无需任何额外的激活或补点 | | role=Transient 与 modal=true 由载体声明（`examples/demos/demo_multi_window.cpp` 第 89 至 96 行），模态实现即 `EnableWindow(owner, FALSE)`（`src/aurora/window/win32_host.cpp:868`），而 inspector 不导出 role，故本例以「owner 链加 owner 禁用态翻转加真实点击的落点复核」为原生判据。**合成点击仍不可用作屏蔽判据**（本轮复现同一对照）：模态期间经 `/api/input/click` 点主窗新建按钮照样触发回调并多出一扇辅窗，因为该路由是进程内派发、不经 OS 对禁用窗口的输入丢弃——26 轮把步骤 4/5 记成「只能取 `IsWindowEnabled`，外观项恒不可判」，前者的口径沿用至今、后者是当时锁屏会话所限（前台窗恒为锁屏 CoreWindow），非产品缺陷；解锁会话下真实指针与前窗均可用，两项现按原判据实测。按钮定位取整窗抓帧内的蓝色横带（Button 主题填充色 0,0,255，DIB 字节序 BGRA，主窗自上而下 6 枚按钮依次对应 6 条带，末带经回显文本反证为广播按钮）；点击点 = `CLIENTORG` + (物理带中心 − 客户区物理偏移) / `scale_factor`（scale 由 `/api/debug/state` 读出，本机 1.5，不硬编码）。截图 `APP_005_modal_open.png`；执行脚本 `build/manual-lib/a005c_run.py`（新建按钮同坐标三轮）与 `a005d_run.py`（未被遮挡坐标三轮），日志同名 `.txt` |
| TC-APP-006 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：退出码 0。步骤 3：`err.txt` 为 0 行。步骤 4：`out.txt` **恰 7 行**，逐行与 §2 预期文本一致，依次为 `frames after first pump: 1`、`find_by_key("tap")=1` 起头的那行、`pump_and_settle frames: 2, total frames: 2`、`counter matches 'clicks = 2': yes`、`expect_visible(button): pass`、`expect_prop(input, value='Aurora'): pass`、`demo PASSED`。步骤 5：第 2 行按字符级统计只有 2 处连续空格，每处恰 2 个空格字符，全行空格字符共 4 个，无任何单空格处，即三字段之间各为 2 个空格 | | 该载体为无头闭环，不建真实窗口，故本例不受锁屏会话影响；构建开关 `AURORA_BACKEND_HEADLESS` 为 ON。空格形态用字符遍历统计（数连续空格段的长度分布）判定，未依赖肉眼；首轮记录里把「空格字符总数 4」误写成「单空格段 4」，本行按复测的段长度分布更正口径 |
| TC-APP-007 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：标题 `Hot reload · Aurora Demo`，客户区 640x300 像素；首次启动且 `ui.json` 不存在时载体自动写入默认内容，`err.txt` 恰 1 行 `INF` 且消息 `[hot_reload] wrote default ui.json -- edit it to trigger a reload`。步骤 3：`ui.json` 顶层为 `Column`，子节点 0 是 `Text` 且 `content` 为 `Edit ui.json, save, and this text is rebuilt live.`，子节点 1 是 `TextInput` 且只声明 `placeholder` 为 `Type here first, then edit ui.json`，确未声明 `value`。步骤 5：改文保存后窗口内 `Text` 变为 `First edit: rebuilt live text V1`，`err.txt` 新增**恰 1 行** `[hot_reload] tree rebuilt from ui.json`。步骤 7：第二次改文后 `Text` 变为 `Second edit: rebuilt live text V2`，同样恰新增 1 行重建日志。退出码 0 | | 载体每帧轮询 `HotReload::try_sync`，一次文件变化对应恰好一行重建日志，故行数即「变化被消费次数」的度量。为确认该契约不被取证通道污染，另跑一次不开 inspector 的对照运行（标签 `APP_007x`），其 stderr 全文仍是「默认写入 1 行加每次编辑 1 行」的原始形态。该载体不经 `run_demo`，未开 DPI 感知，故客户区像素数等于逻辑尺寸（150% 缩放下由 DWM 位图放大）。截图 `APP_007_1_default.png` 等三张 |
| TC-APP-008 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2/3：经 `/api/input/click` 聚焦 `TextInput`（树路径 1）后经 `/api/input/text` 键入 `Aurora`，回读 `value` 为 `Aurora`、占位符仍为 `Type here first, then edit ui.json`。步骤 4/5：只改 `Text` 子节点并保存后，`Text` 已重建为 `Reloaded while input kept its value`，而 `TextInput` 的读数**仍是键入的 `Aurora`**（未被重建抹去），`err.txt` 新增**恰 1 行** `[hot_reload] tree rebuilt from ui.json`。退出码 0 | | 首轮执行时 `TextInput` 的占位符读数为空串：根因是 `src/aurora/widget/serialization.cpp` 把 `TextInput` 注册成 `reg_no_props`（整类丢弃 JSON 属性），使 §1.2 声称的属性往返契约与「ui.json 声明的属性生效」这一前提根本不成立；已改为 `reg_default` 并补 `utest_serialization.textinput_rebuilds_declared_props_from_json` 锁定（提交 `56fff37`），上述读数为修复后复跑结果。按树路径回填的运行态保留逻辑在 `HotReload::try_sync` 的 `preserve_state` 与 `restore_state`，且跳过 JSON 已声明的键。输入经 inspector 进程内派发而非真实键盘，键入内容与真实键入在该控件的 `value` 上同构。截图 `APP_008_1_typed.png`、`APP_008_2_after_reload.png` |
| TC-APP-009 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：先以一次合法编辑把基线钉为 `Text = Baseline before invalid write`，此时 `err.txt` 业务行 2 行（默认写入 1 行加重建 1 行）。步骤 3/4：把 `ui.json` 整份覆盖为缺失闭合括号的非法 JSON 并等待约 3 秒，窗口内 `Text` **保持基线不变**，进程始终存活，业务行数仍为 2 且无新增行，`ERR` 与 `FTL` 级行数为 0。步骤 5：关闭窗口，退出码 0 | | 附加复核（超出 §2 步骤）：非法内容之后再写回合法内容，树仍能重建（新增恰 1 行重建日志且 `Text` 更新为 `Recovered after invalid content`），即「不重建」只是跳过本次变化，文件监视器并未停摆。载体注释把「解析失败不抛错、旧树不动」列为刻意观察点（`examples/demos/demo_hot_reload.cpp` 的帧回调）。非法文件由脚本直接整份写入，不经载体的 JSON 读回路径。截图 `APP_009_invalid.png` |
| TC-APP-010 | 2026-09-28 | Qoder Agent | PASS | | 步骤 2：标题 `System tray · Aurora Demo`，客户区 780x510 像素（即 520x340 逻辑）；`err.txt` 恰 1 行 `INF` 且消息 `[tray] window shown; look for the icon in the notification area`；通知区域的溢出面板（顶层窗 `TopLevelWindowForOverflowXamlIsland`）内出现名为 `Aurora tray demo` 的图标，中心 (3448,1972)。步骤 3：三枚按钮依次为 `Show balloon`、`Hide tray icon`、`Show tray icon`，首行回显 `(no balloon yet)`。步骤 4/5：点 `Show balloon` 后回显变为 `Balloon from the window`，`err.txt` 恰新增 1 行 `[tray] balloon shown`。步骤 6/7：以真实指针左键单击该图标，`err.txt` 恰新增 1 行 `[tray] balloon shown` 加 1 行 `[tray] activated`，窗口回显变为 `Icon activated`（气泡正文即该串，标题 `Aurora` 由载体 `show_balloon("Aurora", msg)` 的固定入参给出） | | 本轮为解锁会话下的复跑，判由 SKIP 改 PASS，且**依赖一处产品修复**：`Shell_NotifyIcon` 以 `NOTIFYICON_VERSION_4` 注册后，回调 `lParam` 是打包编码（低 16 位为事件、高 16 位为 `uID`），旧实现按整值与鼠标消息比较，故真实系统下一次左键单击收到 4 条回调却零命中，`on_activate` 从不触发、右键菜单也从不展开——首轮记为 SKIP 时看到的正是这条坏路径。修复落在 `src/aurora/app/detail/tray_events.h`（版本感知的分类函数）与 `src/aurora/app/system_tray_win32.cpp`，回归由 `tests/unit/utest_system_tray.cpp` 的两条分类用例锁定；一次单击恰新增一对日志行（而非两对）即「不重复触发」的判据。气泡本体由 shell 呈现，正文以载体回显与 `last_balloon_message` 为观测量。截图 `APP_010_flyout.png`（面板内图标在位），日志 `APP_010_011_err.txt` |
| TC-APP-011 | 2026-09-28 | Qoder Agent | PASS | | 步骤 3：在图标上真实右键单击，弹出 `#32768` 上下文菜单（250x168 像素），自上而下为 `Show balloon`、分隔符、`Hide icon`、`Show icon`、分隔符、`Quit`，与声明序一致。步骤 5：点菜单 `Show balloon` 后窗口回显为 `Balloon from the tray context menu`，`err.txt` 恰新增 1 行 `[tray] balloon shown`。步骤 7：点 `Show icon` 新增 1 行 `[tray] icon shown`；点 `Hide icon` 新增 1 行 `[tray] icon hidden`，且溢出面板的图标名清单不再含 `Aurora tray demo`（其余 8 项仍在）；随后点窗口内 `Show tray icon`，清单恢复含该项。步骤 9：点 `Quit` 新增 1 行 `[tray] quit requested from tray menu`，进程随即结束，退出码 0 | | 判由 SKIP 改 PASS，同样依赖 TC-APP-010 备注里的版本 4 `lParam` 修复（修复前右键根本不展开菜单，项序只能核到代码声明）。步骤 7 的字面读法「Hide 之后由菜单里的 Show icon 恢复」在真实系统上不可执行——图标一旦隐藏便无从右键，故 `[tray] icon shown` 改由图标仍在位时点击同一菜单项取证，「图标重新出现」另经窗口 `Show tray icon` 按钮取证，两者合起来覆盖该预期。窗口按钮不记日志这一载体设计（`examples/demos/demo_system_tray.cpp` 第 89 与 91 行仅调 `tray.hide()` 与 `tray.show()`）首轮已记录，本轮复核仍成立。取证纪律：菜单为 `TrackPopupMenu` 模态循环，PowerShell 冷启动约 6 秒后自动收起，故「展开菜单—定位行—点击」必须在同一次进程内完成，跨进程补点会得到静默失败的假阴性（本轮实测两次）。截图 `APP_011_menu.png`、`APP_011_flyout_hidden.png`，清单 `flyout_icons_visible.txt` 与 `flyout_icons_hidden.txt`，退出码 `APP_011_quit_exit_code.txt` |
| TC-APP-012 | 2026-09-28 | Qoder Agent | PASS | | 步骤 1/2：载体按修复后的库重新链接并启动（`AURORA_INSPECTOR_PORT=39212`），标题 `RichTextEdit · Aurora Demo`，客户区 960x600 像素（640x400 逻辑 @150%）；`err.txt` 业务行恰 1 行 `INF` 且消息 `[run_demo] window shown: RichTextEdit · Aurora Demo(close window to exit)`；`/api/tree` 第 3 个子节点即 `RichTextEdit`，初值为 `Aurora RichTextEdit` 换行加一行 Ctrl 快捷键说明。剪贴板可用性先单独验证：另一个互不相干的 PowerShell 进程写入 `AURORA_PROBE_123` 并读回成功，即首轮记 SKIP 时的 `OpenClipboard` 失败确系锁屏会话所致。步骤 3：真实指针单击文本区定光标后 `type_text` 键入 10 字符 `AuroraClip`（落在 `redo (Ctrl+Y` 之后、句点之前），先把系统剪贴板重置为哨兵 `SENTINEL_BEFORE`，再按 Ctrl+A 与 Ctrl+C，独立进程读回 LEN=120 且内容等于整篇文档（含刚键入的 `AuroraClip`）——哨兵已被覆盖，故 Ctrl+C 写入的是**系统**剪贴板而非进程内缓冲。步骤 4：记事本（`C:\Windows\System32\notepad.exe`，pid 4736）Ctrl+V 粘出同一段，截图可见首行 `Aurora RichTextEdit` 与第二行开头，行宽截断处之后的尾巴以剪贴板 120 字符读数为准。步骤 5：记事本清空后键入 `FromNotepad`，Ctrl+A 与 Ctrl+C，独立进程读回 LEN=11 且内容即 `FromNotepad`（外部应用写入可被读回）。步骤 6：回编辑器 Ctrl+End 后 Ctrl+V 两次，随后 Ctrl+A 与 Ctrl+X 读回 LEN=142，恰为原 120 字符文档加两份 `FromNotepad` 拼接，即两次粘贴都进了模型。步骤 7：该次剪切后编辑器已空（再键入 `AFTER_CUT` 后 Ctrl+A 与 Ctrl+C 只读回 LEN=9 的这 9 字符），另一次 Ctrl+A 与 Ctrl+X 后独立进程读回 LEN=9 内容 `AFTER_CUT`；记事本清空并 Ctrl+V 后画面显示 `AFTER_CUT`、状态栏「行 1, 列 10」与「9 个字符」，与编辑器中被移除的那段一致且编辑器不再含它 | | 本轮判由 SKIP 改 PASS，解锁的是两件事：真实输入落地，与系统剪贴板本身可用。组合键通道仍需留意——computer-use 的 `press_key` chord 在约 1ms 内投完 4 个 SendInput 事件，而 aurora 在派发时刻以 `GetAsyncKeyState` 采样修饰键（`src/aurora/window/win32_host.cpp:41`），实测首轮 Ctrl+C 之后哨兵原样在位（键被丢）；改用自写脚本按住 Ctrl 140ms 再投主键后稳定命中，与 TC-RENDER-013 的「真键盘按住 Ctrl」口径一致。观测缺口（另立任务追查）：快捷键驱动的编辑不刷新 `RichTextEdit` 的 UIA `Value` 与画面（`/api/tree` 与窗口截图停在初值），故本例的内容断言一律以系统剪贴板为模型 oracle，未采信树读数与像素读数。更正首轮记录的一条库侧结论：`Clipboard::set_text` 在 `OpenClipboard` 失败时并非静默，`src/aurora/app/clipboard.cpp:136` 有 `AURORA_LOG_WARN`；但同函数在 `text.empty()` 时确实静默早退，故「复制空内容可清空剪贴板」不成立。截图 `APP_012_notepad_pasted.png`、`APP_012_notepad_cut_pasted.png`，探针回读 `clip_probe.txt`（LEN 依次为 15、120、11、142、9 五轮） |
| TC-APP-013 | 2026-09-27 | Qoder Agent | PASS | | 步骤 2：标题 `Timer · Aurora Demo`，客户区 780x660 像素（即 520x440 逻辑）。步骤 3：`err.txt` **共 4 行**，首行为 `INF` 级 `[run_demo] window shown: Timer · Aurora Demo(close window to exit)`，其余**恰 3 行**均为 `WRN` 级诊断 JSON。步骤 4：三条 JSON 的 `where` 字段完全一致，均为 `Timer mounted without a running Scheduler (Scheduler::current() == nullptr); ticks will not fire.`，`category` 为 general、`severity` 为 warning。步骤 5：约 2 秒与约 5.4 秒两次抓取窗口表面（各 802x716 像素）逐像素比对，**差异像素 0**——时钟停在 `clock: --:--:--`、倒计时停在 `countdown: 10`、`auto-revealed after 2s!` 从未浮现；进程全程存活，关窗退出码 0 | | 「始终不出现」必须以像素为准：`au::Show` 门的子节点即使门关着也仍会在 `/api/tree` 里回读出 `content`，故树读数对该断言是假证据；两帧整幅 0 差异同时排除了「整屏位移造成的假阳性」，并一并证明计时器未触发。三行诊断对应载体声明的 3 个 `Timer`（各产 1 条）。该载体经 `run_demo` 只建窗口、不建 `Application`，故 `Scheduler::current()` 为空，表现为「降级加明确诊断」而非中止。取证通道开启的那次运行 stderr 多出 inspector 行，故步骤 3 的「共 4 行」取自不开该通道的原始运行。截图 `APP_013_win.png`，差分两帧 `APP_013_frame1.png`、`APP_013_frame2.png` |
