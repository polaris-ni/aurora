# system-drag 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

系统拖放（跨进程拖放）的**平台后端接线**——事件族、控件契约与数据契约由既有切片交付，本卷只覆盖它们与真实 OS 会话之间的那一段：

| 头 / 实现 | 职责 |
|:---|:---|
| `event/event.h` | `SystemDropEvent` 四态与 `SystemDragOutcome` 四结局；`system_drag` 命名空间的拖出入口 |
| `src/aurora/window/detail/system_drag_translate.h` | **零平台头折算层**：剪贴板格式 ↔ MIME、`MK_*` → `ModifierKey`、坐标折算、动作与 `DROPEFFECT` 双向映射、同步载荷阈值 |
| `src/aurora/window/detail/win32_drag.h` | Win32 OLE 双向桥（拖入：`RegisterDragDrop` + `IDropTarget` 四态 + 同步载荷；拖出：`IDropSource` + 数据对象 + 库自管拖拽线程） |
| `src/aurora/event/detail/system_drag_backend.h` | 拖出注册接缝：桥与事件层之间的两条通道（终态登记 + 跨线程要约索取） |
| `src/aurora/event/detail/system_drag_ingress.h` | 拖入入口：宿主投事件的唯一入口与 inert 拖入后端 |

本文档覆盖三类判据：**① 必须有真实 OS 会话参与的**——跨进程拖放的进入 / 悬停 / 离开 / 落下四态、OS 光标反馈、uri-list 载荷往返；**② 必须有真实窗口与 HWND 参与的**——宿主是否真的把 OLE 拖入目标注册到了窗口上；**③ 拖出方向**——无人值守自拖放全链路（`begin_system_drag` → 拖拽线程 `DoDragDrop` → 终态经帧循环投递 → 载荷往返，可由探针自动段证明）与跨进程拖出到外部目标（载荷交付与终态折算，须人工拖放动作）。第一类离线单测做不到：无头 CI 里没有窗口，OS 会话回调永不到达。第二类同样做不到，但可由探针的自动段以反向方式证明（重复注册返回 `DRAGDROP_E_ALREADYREGISTERED`。离线可跑的判据（折算层四类映射、拖入入口的抑制与恢复、拖出终态折算、能力位方向级分叉）全部在 `tests/unit/utest_system_drag_translate.cpp`、`tests/unit/utest_system_drag_ingress.cpp` 与 `tests/unit/utest_drag_payload.cpp` 里，**不在本文档重复**——重复记录会让「手工跑过」与「CI 跑过」两种证据在读者眼里混成一种。

**已接线范围**：Win32 / D3D11 两类 Surface 的**双向**（拖入为既有切片，拖出为本批新增；二者共用同一 `Win32Host` 与同一桥实例，拖出走库自管拖拽线程，终态经事件层登记、帧循环投递）。X11、Wayland、macOS 三平台的双向属后续批次，其用例待对应批次落地后按编号顺延追加，本卷不预留空号。

### 1.2 执行载体

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `tools/verify/win32_drag_live_probe.cpp` | `aurora_verify_win32_drag` | 真机验收：窗口含上面板（落放面板，拖入四态与自拖放落点）与下面板（拖出源，`on_provide_drag` 供 text/plain）；自动段跑五条判据（拖入三条：能力位 / OLE 注册 / 派发链；拖出两条：能力位 / 无人值守自拖放全链路），人工段常驻窗口打印拖入四态与拖出 begin / cancel / end 态 |

探针的命令行口径（`aurora_verify::parse_interactive`）：

| 选项 | 值域 | 缺省 | 用途 |
|:---|:---|:---|
| `--interactive` | 开关 | 关 | 自动段（含拖出能力位）跑完后窗口常驻，打印拖入四态与拖出 begin / cancel / end 态供人工核对；不带此开关时跑完自动段与无人值守自拖放腿即退出 |
| `--help` | 开关 | 关 | 打印用法（不进入探针主体） |

**退出码即结论**：`0` 自动段全部通过、`2` 环境不可用（建窗或取 HWND 失败）、`4` 有 `[FAIL]` 判据行、用法错误为 `64`。探针**不进 CTest**（`aurora_add_verify_probe` 的 `EXCLUDE_FROM_ALL` 与真机要求），产品输出经 `AURORA_LOG_RAW` 走 stdout，因此 `[PASS]` / `[FAIL]` 行可直接重定向成文本比对。

**自动段不是人工段的替代品**：自动段能证明「桥已注册、能力位已翻、派发链语义一致」，以及拖出方向的无人值守自拖放全链路（begin → 拖拽线程 `DoDragDrop` → 终态经帧循环投递 → 载荷往返，无鼠标按键参与）；但**不能**证明 OS 真的把跨进程会话事件投给了本窗口、也不能证明载荷交付到了外部目标——那必须有人工拖放动作。故本卷的人工段是判据主体，自动段是前置。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（MinGW/GCC + Ninja，`AURORA_BACKEND_WIN32` 或 `AURORA_BACKEND_D3D11`） |
| 构建 | 独立构建目录并开 `AURORA_BUILD_VERIFY_TOOLS=ON`；示例见 §2.1 的 `TC-SYSTEMDRAG-001` |
| 桌面会话 | **须有交互式桌面**（拖放是用户手势驱动的 OS 会话，远程桌面与无会话的服务进程不产生拖放消息） |
| 对照 | 文件管理器（资源管理器）作为拖入拖出源与拖出拒绝对照；另备一个不接受拖放的窗口用于光标形态比对，一个接受纯文本的窗口（记事本一类编辑器）作为拖出落点 |
| 样本 | 至少一个本地文件（路径含非 ASCII 字符者更佳，用于验证 uri-list 的 UTF-8 往返） |

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，本模块段为 `SYSTEMDRAG`，自 001 起连续编号。

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

**模块级约定**：探针的 configure 与构建统一置于 `TC-SYSTEMDRAG-001`；其余用例不重复构建，通过依赖用例字段间接依赖该用例产出的探针可执行文件。

**为什么必须统一格式**：本文件由 `tools/check/check_manual_test_format.py` 解析守护（CTest 用例 `check_manual_test_format`，校验字段名与顺序、取值域、依赖拓扑、步骤-预期同号映射与执行记录表格式）。上述约定使每个字段可用**单条正则**校验，无需自然语言推断。

### 1.6 操作步骤与预期结果编号规则

**预期结果与操作步骤同号一一对应**：`预期结果 N` 即「步骤 N 的对应结果」。

1. 预期结果的序号**等于**其对应步骤的序号，两者严格相等——不得因省略而重排、压缩或重新连续编号。
2. 纯执行步骤**不产生预期结果**，其序号在预期结果列表中直接跳过。
3. 故预期结果的序号**不要求连续**，条数也无需等于步骤数。
4. 步骤列表中，纯执行步骤在文本末尾标注「（纯执行，无预期结果）」，供与预期结果列表逐号对照。

### 1.7 判定与记录

| 结果 | 含义 |
|:---|:---|
| PASS | 全部非空预期结果均满足 |
| FAIL | 任一非空预期结果不满足；须在记录表登记该预期结果对应的**步骤号**、实际现象与缺陷编号 |
| BLOCKED | 前置条件无法满足（如载体构建失败），未开始执行 |
| SKIP | 主动判定不适用于本环境（如被测平台与用例限定平台不符） |

**执行记录表格式**（列顺序固定）：

| 列 | 格式 | 允许值 / 正则 |
|:---|:---|:---|
| 用例编号 | 单行 | 与 §2 用例编号一致 |
| 执行日期 | 单行 | `^\d{4}-\d{2}-\d{2}$`；未执行时留空 |
| 执行人 | 单行 | 自由文本；未执行时留空 |
| 结果 | 单行 | `PASS` / `FAIL` / `BLOCKED` / `SKIP`；未执行时留空 |
| 失败步骤号 | 单行 | 留空（未失败或未执行），或 `^\d+(, \d+)*$`（多个以半角逗号加一个空格分隔） |
| 实际现象 | 单行 | 自由文本；仅 `FAIL` 时必填 |
| 缺陷编号 | 单行 | 自由文本 |
| 备注 | 单行 | 自由文本 |

**未执行的用例**：结果列留空，且执行日期 / 执行人 / 失败步骤号 / 实际现象 / 缺陷编号 / 备注一并留空（不得填占位符）；待执行的原因写在 §3 表格之外的正文里。

## 2 用例

### 2.1 后端接线与注册

#### TC-SYSTEMDRAG-001 Win32 拖入后端的构建、能力位与 OLE 注册

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-001 |
| 测试目的 | 验证 Win32 拖入后端确已接线：独立构建产出探针、能力位 `system_drag_in` 在 Win32 / D3D11 下为 true、宿主窗口确已注册为 OLE 拖入目标 |
| 前置条件 | Windows 本机且建有交互式桌面；工具链为 MinGW/GCC + Ninja |
| 依赖用例 | 无 |
| 操作步骤 | 1. 以独立目录 configure 并构建探针：`cmake -S <仓> -B <新目录> -G Ninja -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON`，再 `cmake --build <新目录> --target aurora_verify_win32_drag`（纯执行，无预期结果）<br>2. 查看构建退出码与产物是否存在<br>3. 不带参数运行探针一次<br>4. 核对自动段的拖入三条判据行<br>5. 核对能力位判据与两个方向的既有锚定（跑 `utest_drag_payload` 的用例）<br>6. 同一窗口上以 `--interactive` 再跑一次，确认自动段结论不变 |
| 预期结果 | 2. 构建 rc=0 且产出 `aurora_verify_win32_drag.exe`<br>3. 退出码为 `0`，输出含 `==== Win32 OLE drag bridge live acceptance (drag-in + drag-out) ====`<br>4. `[PASS] surface kind is Win32 or D3D11 (OLE drag-in backend family)`；`[PASS] capabilities().system_drag_in is true after wiring`；`[PASS] RegisterDragDrop on the same HWND reports DRAGDROP_E_ALREADYREGISTERED (bridge registered)`——第三条重复注册被拒即「宿主已注册」的反向证明（同一次运行里另有拖出方向两条判据行，由 `TC-SYSTEMDRAG-006` 核对，本条不重复列举）<br>5. `utest_drag_payload` 的方向级分叉锚定用例全绿：Win32 与 D3D11 的 `system_drag_in` 与 `system_drag_out` 均为 true、其余 SurfaceKind 两个方向均为 false<br>6. 自动段拖入三条判据与步骤 4 逐字相同（`--interactive` 下无人值守自拖放腿不跑，见 `TC-SYSTEMDRAG-006`） |

### 2.2 拖入四态与载荷

#### TC-SYSTEMDRAG-002 从资源管理器拖文件入窗：四态序列与文件清单往返

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-002 |
| 测试目的 | 验证跨进程拖入的四态序列（Enter → Over → Leave / Drop）与 uri-list 载荷往返在真实 OS 会话下成立 |
| 前置条件 | `TC-SYSTEMDRAG-001` 的探针产物齐备；有交互式桌面；至少一个本地文件（含非 ASCII 路径者更佳） |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 以 `--interactive` 启动探针，窗口保持在前台（纯执行，无预期结果）<br>2. 从资源管理器拖一个文件进入窗口，先不松手<br>3. 在窗口内移动指针若干次<br>4. 把指针移出窗口边界<br>5. 重新拖入并在窗口中部松手落下<br>6. 观察探针打印的 `phase=` 行序列与 `drop files=` 行 |
| 预期结果 | 2. 出现 `phase=Enter`（进入即一态，不因未松手而缺失）<br>3. 每次移动都追加 `phase=Over`（按指针采样频率到达，不合并、不丢失）<br>4. 出现 `phase=Leave`（移出即收车，控件侧的自动滚边随之收车）<br>5. 出现 `phase=Drop`<br>6. `drop files=` 后的路径与拖入的文件逐一相同；含非 ASCII 的路径以 UTF-8 正确呈现、无替换字符与截断（uri-list 解码往返无损） |

#### TC-SYSTEMDRAG-003 accept 求交与 OS 光标反馈

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-003 |
| 测试目的 | 验证控件的 accept 查询与源建议动作求交后，`allowed` 经 `DROPEFFECT` 回写成 OS 光标形态 |
| 前置条件 | 同 `TC-SYSTEMDRAG-002`；另备一个明确不接受拖放的对照目标窗口 |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 以 `--interactive` 启动探针（纯执行，无预期结果）<br>2. 拖一个文件悬停在窗口内，观察光标形态<br>3. 在同一位置悬停于对照目标（不接受拖放者）上，观察光标形态<br>4. 回到探针窗口，观察探针打印里 `formats=` 的计数与该文件类型对应的格式数是否相符<br>5. 拖入一个**多格式**源（如同时含文本与文件的选择），观察 `formats=` 计数 |
| 预期结果 | 2. 光标为「可复制」形态（箭头下的加号），不是「禁止放置」<br>3. 对照目标上为「禁止放置」形态——两种形态的差异即 `allowed` 回写生效的证据（`None` → `DROPEFFECT_NONE`）<br>4. 计数与目标声明的格式数一致（格式清单来自源侧枚举，未被改写或截断）<br>5. 多格式源的计数大于单格式源，且顺序按源侧声明的优先级、不重排 |

#### TC-SYSTEMDRAG-004 修饰键改变源建议动作

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-004 |
| 测试目的 | 验证拖入侧的修饰键 → `proposed` 翻译在真实会话下生效（Ctrl → Copy、Shift → Move、Ctrl+Shift → Link） |
| 前置条件 | 同 `TC-SYSTEMDRAG-002`；探针以 `--interactive` 运行 |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 拖文件进窗口并悬停，不加修饰键（纯执行，无预期结果）<br>2. 保持悬停，按下 Ctrl<br>3. 松开 Ctrl，改按 Shift<br>4. 同时按 Ctrl 与 Shift<br>5. 全部松开后再悬停一次 |
| 预期结果 | 2. 光标形态随按下 Ctrl 而变化（proposed 由源侧缺省改为 Copy）<br>3. 按下 Shift 后光标形态再次变化（proposed 为 Move）<br>4. 两键同按时为第三种形态（proposed 为 Link）<br>5. 全部松开后回到与步骤 1 相同的形态（修饰态随按键实时推进，不残留） |

#### TC-SYSTEMDRAG-005 会话放弃：移出与 Esc 取消

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-005 |
| 测试目的 | 验证会话被放弃的两条路径（指针移出窗口、拖拽中按 Esc）都落到 `Leave` 且不产生落放 |
| 前置条件 | 同 `TC-SYSTEMDRAG-002`；探针以 `--interactive` 运行 |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 拖文件进窗口后直接把指针移出窗口并松手（纯执行，无预期结果）<br>2. 观察 `phase=` 行序列末尾<br>3. 再次拖入，在悬停途中按 Esc 取消<br>4. 观察 `phase=` 行序列与 `drop files=` 行是否更新 |
| 预期结果 | 2. 序列末尾为 `Leave`，且本次会话没有 `Drop`<br>3. 出现 `Leave`（OS 侧会话结束即回调 `DragLeave`）<br>4. `drop files=` 行不更新（放弃的会话不产生载荷、不谎报落放） |

### 2.3 拖出方向

#### TC-SYSTEMDRAG-006 拖出自动腿：无人值守自拖放全链路

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-006 |
| 测试目的 | 验证拖出方向确已接线且事件驱动全链路成立：能力位 `system_drag_out` 为 true、`begin_system_drag()` 发起即返回、OS 拖拽循环在无人值守（无鼠标按键，光标泊于落放面板）下落放回本窗口、终态经帧循环投递且要约载荷往返一致 |
| 前置条件 | `TC-SYSTEMDRAG-001` 的探针产物齐备（拖出腿随同一探针交付，无需另建）；有交互式桌面（COM 拖拽循环与会话回调需要） |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 不带参数运行探针一次（复用 `TC-SYSTEMDRAG-001` 的产物，不重新构建）（纯执行，无预期结果）<br>2. 核对退出码<br>3. 核对拖出能力位判据行<br>4. 核对自拖放全链路的判据行与耗时行<br>5. 跑 `utest_drag_payload` 的方向级分叉锚定用例 |
| 预期结果 | 2. 退出码为 `0`<br>3. `[PASS] capabilities().system_drag_out is true after wiring`<br>4. `[PASS] begin_system_drag() starts the session (Ok)`；`[PASS] on_system_drag_begin dispatched on session start`；`[PASS] on_system_drag_end delivered through the frame loop (pump_reads)`；`[PASS] terminal outcome is Succeeded (got Succeeded)`；`[PASS] negotiated action is Copy (got Copy)`；`[PASS] accept query happened for the self-drop`；`[PASS] self-drop reached this window's drop target (Drop phase)`；`[PASS] drop payload round-trips the offered text/plain (got "aurora win32 drag-out self-drop payload")`；`[info] self drag-out settled in` 行给出有限毫秒数，且全程不出现 `drag-out leg timed out`（20s 截止未触发）<br>5. 全绿：Win32 与 D3D11 的 `system_drag_in` 与 `system_drag_out` 均为 true、其余 SurfaceKind 两个方向均为 false |

#### TC-SYSTEMDRAG-007 跨进程拖出到外部目标：载荷交付与成功终态

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-007 |
| 测试目的 | 验证跨进程拖出：从探针下面板拖出要约的 text/plain 到外部接受文本的窗口，载荷被真实交付且终态折算为 Succeeded + Copy |
| 前置条件 | 同 `TC-SYSTEMDRAG-002`；另备一个接受纯文本的窗口（记事本一类编辑器，编辑区为空）；探针以 `--interactive` 运行 |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 以 `--interactive` 启动探针，先打开外部文本目标的空窗口（纯执行，无预期结果）<br>2. 在探针**下面板**按下左键并拖出本窗口，悬停在外部文本目标的编辑区上<br>3. 观察光标形态<br>4. 在编辑区上松手落下<br>5. 观察探针打印的 `[drag-out]` 行序列与外部窗口内容 |
| 预期结果 | 2. 出现 `[drag-out] begin`（发起即派发，不等终态）<br>3. 光标为「可复制」形态（要约缺省动作 Copy 经 `DROPEFFECT` 回写）<br>4. 外部窗口编辑区出现载荷文本 `aurora win32 drag-out self-drop payload`（跨进程交付，不是探针自己打印的）<br>5. 出现 `[drag-out] end:Succeeded:Copy`（终态经帧循环投递，结局与谈成动作可分辨） |

#### TC-SYSTEMDRAG-008 拖出放弃：Esc 取消与目标拒绝

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-SYSTEMDRAG-008 |
| 测试目的 | 验证拖出会话被放弃的两条路径终态可辨且不谎报成功：Esc 取消落 `Cancelled`、目标不接受落 `Rejected`，均不得出现 Succeeded |
| 前置条件 | 同 `TC-SYSTEMDRAG-007`；外部目标另备一个不接受拖放的窗口（资源管理器窗口即可，text/plain 不被接受）；探针以 `--interactive` 运行 |
| 依赖用例 | TC-SYSTEMDRAG-001 |
| 操作步骤 | 1. 以 `--interactive` 启动探针（纯执行，无预期结果）<br>2. 从下面板拖出本窗口，在悬停途中按 Esc<br>3. 观察 `[drag-out]` 行序列<br>4. 再次从下面板拖出，悬停到不接受拖放的外部目标上，确认光标为「禁止放置」后松手（若光标为「可复制」则换一个不接受拖放的窗口重做）（纯执行，无预期结果）<br>5. 观察 `[drag-out]` 行序列与两次会话的终态 |
| 预期结果 | 2. 出现 `[drag-out] begin` 与 `[drag-out] cancel`（发起即派发 begin；Esc 被拖拽循环判为取消，cancel 严格早于 end 投递）<br>3. 出现 `[drag-out] end:Cancelled:None`（取消是用户意图路径，不按故障折算，且不出现 Succeeded）<br>5. 第二次会话出现 `[drag-out] end:Rejected:None`（目标不接受 = `DROPEFFECT_NONE`，按拒绝折算而非失败）；两次会话全程不出现 `end:Succeeded`，且外部目标窗口未收到任何内容 |

## 3 执行记录表

本卷随首批落地，八条用例均**尚未执行**：结果列与其联动列（执行日期 / 执行人 / 失败步骤号 /
实际现象 / 缺陷编号）一并留空，备注亦留空（未执行不得填占位符）。人工段需交互式桌面与人工
拖放动作，无头环境无法产生 OS 会话，故不留假记录。

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-SYSTEMDRAG-001 | | | | | | | |
| TC-SYSTEMDRAG-002 | | | | | | | |
| TC-SYSTEMDRAG-003 | | | | | | | |
| TC-SYSTEMDRAG-004 | | | | | | | |
| TC-SYSTEMDRAG-005 | | | | | | | |
| TC-SYSTEMDRAG-006 | | | | | | | |
| TC-SYSTEMDRAG-007 | | | | | | | |
| TC-SYSTEMDRAG-008 | | | | | | | |
