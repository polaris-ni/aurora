# widget 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`widget/` —— 控件层（**75 个公共头**，全库最大的模块），在实测依赖分层中位于 L5。

本模块同时承担两项职责：

1. **控件实现**（约 60 个头：`button.h`、`text.h`、`text_input.h`、`slider.h`、`scroll.h`、`lazy_list.h`、各类图表、弹层与容器等）；
2. **元数据中枢**（约 15 个头：`widget.h`、`node.h`、`lifecycle.h`、`props_io.h`、`descriptor.h`、`serialization.h`、`inspect.h`、`codegen.h`、`a11y_tree.h`、`a11y_diff.h` 等）。

第二项是全库反向依赖的汇聚点——`state`、`render`、`event`、`layout`、`perf`、`debug`、`theming`、`navigation`、`media` 等模块的下层头均引用 `widget.h` 或 `props_io.h`。这是有意的元数据依赖，不构成架构违规，但意味着**本模块的改动面远大于其目录规模**，人工回归价值相应更高。

### 1.2 执行载体

本模块复用既有载体，不新增载体。载体分两类，用途不同：

| 载体 | 类型 | 用途 |
|:---|:---|:---|
| `build/aurora_cli.exe` | 命令行工具 | 属性 schema 反射、UI 树校验、代码生成——输出确定、可脚本化、有退出码 |
| `examples/demos/demo_lazy_list.cpp` | 窗口程序 | 万行虚拟滚动的流畅度与索引映射 |
| `examples/demos/demo_inspector.cpp` | 窗口程序 | 检查器面板、无障碍语义树观测 |
| `examples/demos/demo_serialization.cpp` | 窗口程序 | 序列化 diff 与注册表规模 |
| `examples/demos/demo_reorderable_list.cpp` | 窗口程序 | 列表重排后的状态保持 |
| `examples/demos/demo_text_input.cpp` | 窗口程序 | 文本编辑、选择与输入法 |
| 各控件 1:1 demo（`demo_button`、`demo_pickers` 等） | 窗口程序 | 同类控件的视觉一致性 |

**为什么优先用命令行工具**：`aurora_cli` 的 `validate` / `components` / `describe` / `to-code` 子命令以退出码与 JSON 表达结果，判定不需要人眼逐像素比对，同时仍保留人工判定的对象——**属性体系是否自洽、诊断是否可操作、生成代码是否可读**。这比在窗口里找属性面板更稳定，也与「人工判定优于程序判定」的取舍一致（程序能判的部分交给 `tests/unit/` 与 `tests/integration/`，人工只判语义质量）。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja） |
| 构建 | 库与 `aurora_cli` 已构建；窗口类载体为按需目标，不在默认构建内 |
| 终端 | 支持 UTF-8 输出 |
| 分离输出 | 诊断日志写 stderr；判定前须重定向 |
| 输入法 | **TC-WIDGET-013 需要中文输入法** |
| 辅助技术 | **TC-WIDGET-014 需要系统读屏工具**（如 Windows 讲述人），无该工具时记 `SKIP` |

**本仓库 78 个 demo 中没有任何一个为无障碍而设**，因此 TC-WIDGET-014 使用 `demo_inspector` 作为间接载体——其被检视树含 `Text` / `Button` / `TextInput` / `Checkbox` 四类语义丰富的控件。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `widget`，即 `TC-WIDGET-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
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

**模块级约定**：本模块窗口类载体的构建步骤统一置于 `TC-WIDGET-001` 的步骤 1；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。`aurora_cli` 属默认构建产物，不需单独构建。

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

**关于 75 个头为何只写 14 条用例**：本模块的 `src/` 实现与 `tests/unit/` 下的同名单测几乎一一对应，单个控件的**行为正确性**（属性取值、布局几何、序列化字节、枚举往返、事件回调）已由自动化覆盖。人工用例的价值不在于把这些重跑一遍，而在于覆盖三类自动化测不到的对象——**语义质量**（诊断是否可操作、生成代码是否可读）、**观感**（滚动是否流畅、同类控件是否一致）、**外部系统集成**（辅助技术能否读到语义树）。

**纳入人工测试的子域**：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 属性 schema 与校验诊断 | 「提示能否照着做」属可操作性判断，程序只能判字段非空 |
| 代码生成产物的可读性 | 生成代码是否符合本仓库书写习惯，需人读 |
| 虚拟滚动的流畅度与索引映射 | 一万行的滚动体感与「点击行号是否对应」需真实交互 |
| 检查器面板与实际树的镜像关系 | 面板是否忠实反映被检视树，属人因判断 |
| 列表重排后的状态保持 | 重排后是否出现残留或状态串台，需连续交互 |
| 同类控件的视觉一致性 | 间距、基线、字号的横向一致属视觉判断 |
| 文本编辑与输入法 | 中文组合输入过程是否丢字、光标是否跳位，程序难以构造 |
| 无障碍语义树 | 语义树能否被系统辅助技术读到，必须由真实 AT 客户端验证 |

**不纳入人工测试的头及其理由**（按职责分组）：

| 头（节选） | 理由 | 已有覆盖 |
|:---|:---|:---|
| `button.h` `text.h` `slider.h` `switch.h` `checkbox.h` `radio_spin.h` `chip.h` `dropdown.h` `pickers.h` `progress.h` `stepper.h` `segmented_control.h` `tab_bar.h` `bottom_nav_bar.h` `divider.h` `spacer.h` `placeholder.h` `skeleton.h` `expansion_panel.h` `dismissible.h` | 单控件行为有确定输入输出，可穷举断言 | `tests/unit/` 下同名 `utest_<控件>` |
| `canvas.h` `image_widget.h` `rich_text.h` `rich_text_edit.h` `text_span.h` | 渲染结果由 golden 比对覆盖；其**编辑交互**由 TC-WIDGET-012 覆盖 | 同名 `utest_*`、`tests/golden/` |
| `bar_chart.h` `line_chart.h` `pie_chart.h` `scatter_chart.h` `sparkline.h` `chart_common.h` `data_widgets.h` | 图表数值到几何的映射为确定性计算 | 同名 `utest_*` |
| `grid.h` `grid_view.h` `stack.h` `containers.h` `layout_builder.h` `layout_query.h` `alignment.h` `breakpoint_builder.h` `splitter.h` `sticky_header.h` `scroll.h` `scroll_viewport.h` `pull_to_refresh.h` | 布局与滚动几何为确定性计算 | 同名 `utest_*`、`itest_overflow_strategy` |
| `dialog.h` `drawer.h` `popup.h` `toast.h` `show.h` `menu_bar.h` `toolbar.h` `title_bar.h` `command_palette.h` `form.h` `recipes.h` | 弹层与容器的显隐逻辑为确定性状态机 | 同名 `utest_*`、`itest_context_menu_tooltip` |
| `props_io.h` `descriptor.h` `widget.h` `node.h` `yaml.h` | 属性 JSON 往返与节点操作为确定性逻辑；其**语义质量**由 TC-WIDGET-002 至 TC-WIDGET-006 覆盖 | `utest_props_*`、`itest_prop_validation`、`itest_props_constraint` |
| `lifecycle.h` `timer.h` `repeater.h` `lazy_row.h` | 生命周期回调时序与复用逻辑为确定性 | 同名 `utest_*`、`itest_widget_lifetime` |
| `serialization.h` `codegen.h` `inspect.h` | 序列化与代码生成本身为确定性转换；其**产物质量**由 TC-WIDGET-005 与 TC-WIDGET-006 覆盖 | `utest_serialization`、`itest_to_code`、`itest_api_json_integrity` |
| `a11y_tree.h` `a11y_diff.h` | 语义树构建与快照 diff 为确定性算法；其**是否被外部 AT 读到**由 TC-WIDGET-014 覆盖 | `utest_a11y_tree` `utest_a11y_diff` `utest_a11y_action` |
| `inspector_panel.h` `inspect.h` | 检查器面板的渲染为确定性；其**镜像关系**由 TC-WIDGET-009 覆盖 | `utest_inspector_panel` |
| `reorderable_list.h` `lazy_list.h` | 重排与虚拟化的结构逻辑为确定性；其**交互观感**由 TC-WIDGET-007 至 TC-WIDGET-010 覆盖 | `utest_reorderable_list` `utest_lazy_list` |

## 2 用例清单

### 2.1 载体基线

#### TC-WIDGET-001 载体构建与命令行工具运行基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-001 |
| 测试目的 | 确认为数众多的候选载体可构建运行，且命令行工具的组件清单输出形态稳定 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；`build/aurora_cli.exe` 与库均已构建 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建窗口类载体：`cmake --build build --target demo_lazy_list demo_inspector demo_serialization demo_reorderable_list demo_text_input`（纯执行，无预期结果）<br>2. 运行 `./build/aurora_cli.exe components 1> components.json 2> err.txt`（纯执行，无预期结果）<br>3. 记录该命令的退出码<br>4. 检查 `components.json` 的形态并计算其元素个数<br>5. 启动 `./build/demo_lazy_list.exe 1> out.txt 2> err2.txt`，查看窗口标题与 `err2.txt` |
| 预期结果 | 3. 退出码为 0<br>4. 输出为合法 JSON 数组，元素为 PascalCase 类型名（如 `Badge`、`BarChart`）；按「总行数减 2」（首行 `[`、末行 `]`）计算，元素个数为 **73**<br>5. 标题为 `LazyList · Aurora Demo`；`err2.txt` 恰 1 行 `INF` 级 `[run_demo] window shown: LazyList · Aurora Demo` |

### 2.2 属性 schema 与校验诊断

#### TC-WIDGET-002 合法 UI 树校验通过

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-002 |
| 测试目的 | 验证结构合法的 UI 树能通过校验，且成功路径的输出形态明确 |
| 前置条件 | `build/aurora_cli.exe` 已构建；`tests/fixtures/ai_compat/` 可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 运行 `./build/aurora_cli.exe validate tests/fixtures/ai_compat/valid_form.json > ok.json 2>&1`（纯执行，无预期结果）<br>2. 记录退出码<br>3. 检查 `ok.json` 的内容 |
| 预期结果 | 2. 退出码为 **0**（与校验失败的 1、用法错误的 2 区分）<br>3. 输出为 JSON，含 `"ok": true`，且**不含** `error` 字段 |

#### TC-WIDGET-003 未知控件类型的诊断可操作

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-003 |
| 测试目的 | 验证错误路径不仅报出问题，还给出**照做即可修复**的指引（这是 AI-first 工具链的核心可用性要求） |
| 前置条件 | `build/aurora_cli.exe` 已构建；`tests/fixtures/ai_compat/` 可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 运行 `./build/aurora_cli.exe validate tests/fixtures/ai_compat/error_unknown_widget.json > bad.json 2>&1`（纯执行，无预期结果）<br>2. 记录退出码<br>3. 检查 `bad.json` 的 `ok` 字段与其中的错误码<br>4. 检查错误信息中的 `message`、`hint`、`severity` 与 `auto_fixable` 字段<br>5. 运行 `./build/aurora_cli.exe describe NotARealWidget`，检查未知类型时的响应（纯执行，无预期结果）<br>6. 运行 `./build/aurora_cli.exe describe Button`，检查已知类型的响应 |
| 预期结果 | 2. 退出码为 **1**（校验失败，非用法错误）<br>3. `"ok"` 为 `false`；错误码为 `widget-unknown-type`<br>4. `message` 明确指出未被注册的类型名；`hint` 给出注册途径；`severity` 为 `warning`（说明是可恢复问题而非致命错误）；`auto_fixable` 为 `true`<br>6. 输出 JSON 对象，含该类型的属性键集合，键名与 `include/aurora/widget/button.h` 的 `ButtonProps` 字段一致 |

#### TC-WIDGET-004 组件 schema 可反射查询与检索

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-004 |
| 测试目的 | 验证组件类型体系可被反射查询，且检索结果与查询词相关 |
| 前置条件 | `build/aurora_cli.exe` 已构建 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 运行 `./build/aurora_cli.exe search text > search.json 2>&1`（纯执行，无预期结果）<br>2. 记录退出码并检查命中列表<br>3. 检查命中的类型名与查询词的关系<br>4. 运行 `./build/aurora_cli.exe components`，核对步骤 2 的命中项是否都在全量清单中（纯执行，无预期结果）<br>5. 运行 `./build/aurora_cli.exe schema 1> schema.json 2>&1`，检查输出是否为完整 API 描述 |
| 预期结果 | 2. 退出码为 0；命中列表非空<br>3. 每个命中的类型名均含查询词 `text`（大小写不敏感），如 `Text`、`TextInput`、`RichText`、`TextSpan` 一类；不含无关键<br>5. 输出为 JSON，覆盖全部已注册组件，且其中的组件名集合与步骤 4 的全量清单一致 |

### 2.3 序列化与代码生成

#### TC-WIDGET-005 序列化 diff 的补丁规模与注册表规模

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-005 |
| 测试目的 | 验证两棵仅一处文本不同的树之间的补丁规模与变更规模相称，且注册表类型数与工具链清单一致 |
| 前置条件 | 载体 `demo_serialization` 已构建成功 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_serialization.exe`（纯执行，无预期结果）<br>2. 读取窗口内 `patch ops =` 行的数值<br>3. 读取窗口内 `WidgetRegistry type count =` 行的数值<br>4. 把步骤 3 的数值与 TC-WIDGET-001 步骤 4 测得的组件元素个数比对 |
| 预期结果 | 2. 显示一个**较小的正整数**（两棵树仅第二行文本由 `world` 变为 `aurora`，补丁应集中在该差异上，不应等于整棵树的节点数）<br>3. 显示一个正整数<br>4. 两者**相等**（窗口内的注册表与命令行工具的组件清单来自同一注册表）；若不等即 FAIL |

#### TC-WIDGET-006 代码生成产物形态正确且两种风格结构等价

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-006 |
| 测试目的 | 验证由 UI 树生成的 C++ 代码使用本库真实类型与属性键，且不同风格仅改变构造形式而非语义 |
| 前置条件 | `build/aurora_cli.exe` 已构建；`tests/fixtures/ai_compat/valid_form.json` 可用 |
| 依赖用例 | TC-WIDGET-001, TC-WIDGET-002 |
| 操作步骤 | 1. 运行 `./build/aurora_cli.exe to-code tests/fixtures/ai_compat/valid_form.json > fluent.cpp 2>&1`（纯执行，无预期结果）<br>2. 记录退出码并检查输出的代码形态<br>3. 核对代码中出现的类型名与属性键<br>4. 运行 `./build/aurora_cli.exe to-code tests/fixtures/ai_compat/valid_form.json --style step > step.cpp 2>&1`（纯执行，无预期结果）<br>5. 比较两份输出的类型构造形式差异与语义等价性<br>6. 运行 `./build/aurora_cli.exe to-yaml tests/fixtures/ai_compat/valid_form.json > tree.yaml 2>&1`，检查 YAML 形态（纯执行，无预期结果）<br>7. 检查 `tree.yaml` 的缩进与键结构 |
| 预期结果 | 2. 退出码为 0；输出为 C++ 代码文本，含 `au::` 命名空间限定的类型构造<br>3. 出现的类型名与属性键均真实存在（可与 `describe` 的输出以及 `include/aurora/widget/` 下的头文件核对），**不出现占位符或臆造键名**<br>5. 两份输出描述同一棵树（类型与属性键集合一致），差异体现在构造形式：`fluent.cpp` 使用指定初始化器形式，`step.cpp` 使用先声明后逐属性赋值的形式<br>7. 为合法 YAML，缩进体现树的层级，键名与步骤 3 核对过的属性键一致 |

### 2.4 虚拟滚动

#### TC-WIDGET-007 万行列表滚动流畅、边界无越界

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-007 |
| 测试目的 | 验证一万行列表依靠虚拟化保持滚动流畅，且滚动到两端时不出现空白或越界 |
| 前置条件 | 载体 `demo_lazy_list` 已构建成功；鼠标滚轮可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_lazy_list.exe`（纯执行，无预期结果）<br>2. 持续向下滚动约 10 秒（纯执行，无预期结果）<br>3. 观察滚动过程的连续性<br>4. 滚到最底部后继续滚动 5 次（纯执行，无预期结果）<br>5. 观察列表底部与顶部边界处的呈现 |
| 预期结果 | 3. 滚动连续无卡顿、无长时间白屏；按行高 32 像素计算，滚过一万行的时间由滚轮速率决定，但**不应出现逐行掉帧式的顿挫**<br>5. 到达底部后继续滚动不再改变列表位置，列表末尾行完整可见、下方无异常空白；反向滚到顶部时同样不越界 |

#### TC-WIDGET-008 虚拟化索引与点击行号一致

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-008 |
| 测试目的 | 验证虚拟滚动下控件的显示索引与数据索引一致（列表复用最易出的错是「视觉与数据错位」） |
| 前置条件 | 载体 `demo_lazy_list` 已构建成功；鼠标可用；当前目录可写 |
| 依赖用例 | TC-WIDGET-001, TC-WIDGET-007 |
| 操作步骤 | 1. 启动 `./build/demo_lazy_list.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 向下滚动到中部（纯执行，无预期结果）<br>3. 读取某一行的完整文本，记录其中的编号（纯执行，无预期结果）<br>4. 点击该行（纯执行，无预期结果）<br>5. 查看 `err.txt` 新增行中的编号 |
| 预期结果 | 5. 新增恰 1 行 `INF` 级日志，消息为 `[lazy_list] clicked row <N>`；`<N>` 与步骤 3 记录的行文本中的编号**完全相同**。若点击第 5000 行却记录为其他编号，说明索引映射错位，即 FAIL |

### 2.5 检查器与语义树

#### TC-WIDGET-009 检查器面板忠实镜像被检视树

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-009 |
| 测试目的 | 验证检查器的树浏览器反映的是被检视树的**真实结构**，而非硬编码的示意结构 |
| 前置条件 | 载体 `demo_inspector` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_inspector.exe`（纯执行，无预期结果）<br>2. 观察左侧树浏览器列出的节点类型与层级（纯执行，无预期结果）<br>3. 对照右侧被检视树的实际内容，核对两侧结构<br>4. 在左侧树中选中 `TextInput` 类型的节点（纯执行，无预期结果）<br>5. 查看右侧属性面板列出的属性键 |
| 预期结果 | 3. 左侧树包含右侧被检视树的全部节点类型（`Column`、`Text`、`TextInput`、`Button`、`Checkbox`），父子层级一致，无多余或缺失节点<br>5. 属性面板列出该节点的属性键；所列键**均存在于** `describe` 对该类型输出的属性键集合中，不出现臆造键名 |

#### TC-WIDGET-010 可重排列表拖拽后无残留与状态串台

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-010 |
| 测试目的 | 验证重排操作后各行的视觉与状态随行迁移，不留在原位置（列表复用最典型的缺陷是「行内容移动了但状态没跟着走」） |
| 前置条件 | 载体 `demo_reorderable_list` 已构建成功；鼠标可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_reorderable_list.exe`（纯执行，无预期结果）<br>2. 记录列表中前若干行的文本顺序（纯执行，无预期结果）<br>3. 把第 1 行拖拽到第 3 行之后（纯执行，无预期结果）<br>4. 核对拖拽后各行文本的顺序，以及每行内是否保留了各自的独立状态（如勾选、输入内容）<br>5. 连续快速拖拽换位 10 次（纯执行，无预期结果）<br>6. 核对列表最终状态 |
| 预期结果 | 4. 行的文本顺序按拖拽结果调整（原第 1 行现位于原第 2、3 行之后），且**每行携带自己的状态一起移动**，不出现「文本移动了但勾选留在原位」的串台<br>6. 10 次换位后列表行数与行文本集合与初始一致（无重复行、无丢失行、无空白占位），滚动也仍正常工作 |

#### TC-WIDGET-011 同类控件并列时视觉一致

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-011 |
| 测试目的 | 验证同一类控件在并列放置时的高度、基线与内间距一致，不出现逐个走样 |
| 前置条件 | 载体 `demo_pickers` 与 `demo_button` 已构建成功 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_pickers.exe`（纯执行，无预期结果）<br>2. 横向比对同屏并列的同类控件（纯执行，无预期结果）<br>3. 检查各控件的高度、文本基线、内间距与边框是否对齐一致<br>4. 启动 `./build/demo_button.exe`，对按钮组重复同样比对（纯执行，无预期结果）<br>5. 检查按钮的圆角、内边距与文字垂直居中是否一致 |
| 预期结果 | 3. 同类控件高度一致、文本基线共线、内间距一致；不存在某一个控件明显偏高、偏低或文字偏移<br>5. 各按钮的圆角半径与内边距一致，文字在垂直方向居中于按钮盒内，无上偏或下偏 |

### 2.6 文本输入

#### TC-WIDGET-012 文本编辑、光标与选择

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-012 |
| 测试目的 | 验证文本输入控件的编辑操作、光标定位与选区呈现符合常规编辑器直觉 |
| 前置条件 | 载体 `demo_text_input` 已构建成功；鼠标与键盘可用 |
| 依赖用例 | TC-WIDGET-001 |
| 操作步骤 | 1. 启动 `./build/demo_text_input.exe`（纯执行，无预期结果）<br>2. 点击输入框后输入一段英文与数字（纯执行，无预期结果）<br>3. 检查文字是否即时出现在输入框内且光标位于末尾<br>4. 用方向键左移光标到文本中部，再输入一个字符（纯执行，无预期结果）<br>5. 检查插入位置<br>6. 用 `Shift` 加方向键选中一段文本，检查选区的视觉呈现；再按退格键删除选区（纯执行，无预期结果）<br>7. 检查选区是否被正确删除 |
| 预期结果 | 3. 输入的字符即时逐个出现，光标紧跟在最后输入字符之后，无延迟、无丢字<br>5. 新字符插入在光标所在位置（非追加到末尾、非覆盖相邻字符）<br>7. 选中的高亮区域按一次退格键即被整体删除（而非仅删除一个字符）；删除后光标停在删除位置 |

#### TC-WIDGET-013 中文输入法组合输入不丢字不移位

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-013 |
| 测试目的 | 验证输入法组合期间不出现丢字、重复字符或光标跳位 |
| 前置条件 | 载体 `demo_text_input` 已构建成功；**系统已启用中文输入法**；键盘可用 |
| 依赖用例 | TC-WIDGET-001, TC-WIDGET-012 |
| 操作步骤 | 1. 启动 `./build/demo_text_input.exe`，点击输入框（纯执行，无预期结果）<br>2. 切换中文输入法，输入拼音 `zhongwen`（纯执行，无预期结果）<br>3. 观察组合期间的候选与预编辑呈现<br>4. 从候选列表中选择「中文」上屏（纯执行，无预期结果）<br>5. 检查输入框内容<br>6. 继续输入英文一个词（需先切回英文态），检查中英混排（纯执行，无预期结果）<br>7. 检查混排结果与光标位置 |
| 预期结果 | 3. 组合期间预编辑内容可见，候选窗口位置贴近光标，不与输入框错位<br>5. 输入框内容恰为「中文」两个字，**无重复、无缺字、无拼音残留**<br>7. 英文紧接在中文字符之后，中英混排显示正常，光标位于末尾 |

### 2.7 无障碍

#### TC-WIDGET-014 语义树被系统辅助技术读取

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-WIDGET-014 |
| 测试目的 | 验证控件的无障碍语义树确实被操作系统暴露给辅助技术客户端（本项**必须**由真实 AT 工具验证，任何程序内断言都无法替代） |
| 前置条件 | 载体 `demo_inspector` 已构建成功；**本机已启用系统读屏工具**（如 Windows 讲述人），并掌握其元素浏览方式；无该工具时记 `SKIP` |
| 依赖用例 | TC-WIDGET-001, TC-WIDGET-009 |
| 操作步骤 | 1. 启动 `./build/demo_inspector.exe`（纯执行，无预期结果）<br>2. 启动系统读屏工具，切换到该窗口（纯执行，无预期结果）<br>3. 用读屏逐元素浏览被检视树区域，记录它读出的角色与名称<br>4. 用读屏浏览到被检视树中的 `Submit` 按钮，记录读出的角色与名称<br>5. 用读屏浏览到 `Checkbox`，先在界面上改变其勾选状态，再重新浏览并记录读出的状态 |
| 预期结果 | 3. 读屏能逐元素读出被检视树中的节点，每个元素均有**非空的角色与名称**（能区分出文本框、按钮、复选框一类的角色）<br>4. `Submit` 按钮被读出的名称为 `Submit`（或与其标签一致的文本），角色为按钮一类<br>5. 勾选状态改变后，读屏读出的状态随之变化（如由「未选中」变为「已选中」）。若读屏读不出任何元素、或读出的角色与名称全为空，即 FAIL |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-WIDGET-001 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：`components` 退出码 0；步骤 4：`components.json` 共 75 行（首 `[` 末 `]`）→ 元素 **73** 个，首两项 `Badge`/`BarChart`、末项 `VideoPlayer`，全部 PascalCase；步骤 5：标题 `LazyList · Aurora Demo`，`err2.txt` 恰 1 行 `INF` 级 `[run_demo] window shown: LazyList · Aurora Demo` | | 步骤 1 的 5 个窗口载体（`demo_lazy_list`/`demo_inspector`/`demo_serialization`/`demo_reorderable_list`/`demo_text_input`）均构建成功；§1.1 的「75 个公共头」是头文件数，与本用例预期的 73 个注册类型不是同一口径 |
| TC-WIDGET-002 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：退出码 0；步骤 3：输出为 `{"ok": true}`，含 `"ok": true` 且不含 `error` 字段 | | 与步骤 2 的失败态（TC-WIDGET-003 的 rc=1）构成三档退出码区分 |
| TC-WIDGET-003 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：退出码 1；步骤 3：`"ok": false`，错误码 `widget-unknown-type`；步骤 4：`message` 点名未注册类型 `'NotARealWidget'`，`hint` = `Register it first via WidgetRegistry::register_factory`，`severity` = `warning`，`auto_fixable` = `true`；步骤 6：`describe Button` 输出属性键含 `label`/`enabled`/`corner_radius`/`padding`/`font_size`，与 `include/aurora/widget/button.h` 的 `ButtonProps` 字段一致 | | 步骤 5 记录一处可诊断性缺口：`describe` 对未注册类型仍返回退出码 0 并回显空壳对象（`type` 原样回显、`prop_descriptors` 为空数组），未给出「该类型未注册」的任何诊断或候选提示 |
| TC-WIDGET-004 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：退出码 0，命中列表非空（4 项）；步骤 3：命中类型为 `RichText`/`RichTextEdit`/`Text`/`TextInput`，4 项名称均含子串 `text`（大小写不敏感），无无关键；步骤 4：4 项全部落在 `components` 的 73 项全量清单内；步骤 5：`schema` 退出码 0，`widgets` 数组 73 项，其类型名集合与步骤 4 全量清单**完全相等**（双向差集均为空） | | `schema` 顶层键为 `alias`/`enums`/`include`/`language`/`library`/`widgets`，除组件外另含枚举反射 |
| TC-WIDGET-005 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：窗口内 `patch ops = 1`（两棵树仅第二行文本 `world`→`aurora`，补丁规模与变更相称，远小于整树节点数）；步骤 3：`WidgetRegistry type count = 73`；步骤 4：与 TC-WIDGET-001 步骤 4 的 73 **相等** | | 标题 `Serialization · Aurora Demo`（客户区 780×630 物理像素），`stderr` 的 `ERR`/`FTL` 0 行，退出码 0 |
| TC-WIDGET-006 | 2026-09-26 | Qoder Agent | FAIL | 5, 7 | 步骤 2：`to-code` 退出码 0，输出为 `au::` 限定构造；步骤 3：出现的类型与属性键（`Text.content`、`TextInput`、`Button.label`）均真实存在，无占位符或臆造键；步骤 5：两份产物**并不等价**——`fluent.cpp` 经 `g++ -std=c++20 -fsyntax-only` 通过（rc 0），`step.cpp` 编译失败（rc 1，`5:23 error: expected primary-expression before 'auto'`，因 `__w0.children = { auto __w1 = au::Text{};` 把变量声明写进了初始化列表内）；步骤 7：`tree.yaml` 非合法 YAML——嵌套映射被压进同一行（`-     props:       content: "Name:"`），键与值的层级被展平、缩进断裂 | | 两风格「类型与属性键集合一致、仅构造形式不同」这一半成立（键集合相同），缺陷集中在 `step` 风格产物不可编译、`to-yaml` 产物不合法。前者落在代码生成的初始化列表拼接，后者落在 `include/aurora/widget/yaml.h` 的 `emit_object_value` 对内嵌对象就地内联 |
| TC-WIDGET-007 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：持续下滚 10.1 秒、期间取帧 91 次，全白帧 0 次，相邻帧 ink 差分中位 215 / 最大 3705、差分为 0 的相邻帧对 0（即无停滞帧、无空白帧），首行索引采样 `[12, 31, 50, 68, 87, 106]` 连续推进无跳变；步骤 5：滚到最底（8400 格）后行窗口停在 9982..9999 且含末行 `Row 9999`，再滚 5 次末索引不变（不再改变位置），末行绘制于 dp y=448 高 32 而视口底为 480 → 末尾行完整可见、下方无异常空白；反向回顶后行窗口 0..17，再滚 5 次首索引仍为 0 | | `/api/debug/perf` 在 `run_demo` 载体上恒返回 0（`FrameStats::record()` 仅由 `src/aurora/app/window_host.cpp` 调用），故「流畅」改由逐帧 ink 差分外证，帧间隔与掉帧计数不可用 |
| TC-WIDGET-008 | 2026-09-26 | Qoder Agent | FAIL | 5 | 步骤 3：滚到中部（4000 格）后帧内可见 11 行依次为 `Row 5000`..`Row 5010`，首行墨水带位于 dp 115.3..129.3、逐行 +32dp（行高与 32dp 设定一致）；步骤 4/5：点击该行文字中心，日志编号**恒为可见编号减 1**——可见 5003→`clicked row 5002`、5004→5003、5007→5006、5009→5008、5010→5009（5 次一致，每次恰新增 1 行 `INF`），而点击最顶可见行 `Row 5000` 的文字（dp 115..129）不产生任何日志 | | 命中盒按行框 dp 131+32k 判定（与 `pick` 报出的 `LazyList` y=131 吻合），绘制带却在 dp 115.3+32k，两套几何整体相差 15.7dp≈半行，使点击落入上一行。同一列表内步长恒 1、且按几何坐标点击的首块锚点为 `Row 0`，说明错位来自**绘制与命中的原点不一致**而非行索引计算错误 |
| TC-WIDGET-009 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2/3：左侧树浏览器列出 `Column`/`Text`/`TextInput`/`TextInput`/`Button`/`Checkbox` 六条，与被检视树的 DFS 序列（`0/2/1/1` 起的 6 节点）类型与父子层级逐项一致，帧内人眼核对无多余、无缺失；步骤 4/5：选中 `TextInput` 行后属性面板列出 `value`/`placeholder`/`font_size`/`corner_radius`/`padding`/`cursor_color`/`enabled`/`text_color`/`placeholder_color`/`background`/`focused_border_color`/`border_color`/`border_width`/`selection_color`，全部落在 `describe TextInput` 的 21 个属性键集合内，无臆造键名 | | 记录三处面板缺陷：属性值列与键列重叠（键名被截断成 `placehold`、`border_c`）、`Export Code` 按钮压住类型标题、点击行与高亮带相差一行（点 dp y=295 时高亮带落在 238..274）。`/api/debug/pick` 报出的 bounds 与实际绘制位置不一致，故所有点击落点均按帧标定 |
| TC-WIDGET-010 | 2026-09-28 | Qoder Agent | PASS | | 解锁会话用真实指针（`SendInput` 绝对坐标，载体 DPI-unaware ⇒ 1dp=1px）复跑。步骤 2：初始 8 行 `#0 需求评审`…`#7 排下轮计划`，顺序镜像 `(no reorder yet)`，`scroll_offset=0`。步骤 3/4：手柄带外（dp x=300）起拖零变化，带内（dp x=326..374）起拖即 `moved #0 → #3`，`#0 需求评审` 整行随指针移动、`≡` 一并迁移，无「文本走了状态留在原位」；无串台判据取「回调 join 出的数据序 == 渲染行文本」，11 次拖拽逐次相等。落位下标经 7 组对照钉死：指针停在目标行中点上方 14dp ⇒ 落该行下标（`moved #4 → #2`、`moved #2 → #4`），下方 14dp ⇒ 落下一行（`moved #4 → #3`、`moved #2 → #5`），上/下拖一致；主循环取点正落在行中线上，`±2dp` 滞回带内的结果由拖拽方向裁决，故 10 次里 8 次回调下标为「请求值 +1」、2 次恰等，另两次「从邻行中线上方拖到该中线」直接不提交（`slot==from`）——属取点压在判界，不是控件缺陷。步骤 5：连续 10 次快速换位 + 1 次滚动中换位全部生效（`生效=11/11`），提交耗时 0.2~1.2s（松手后须等 spring 静止才改数据），拖到视口边缘 56dp 内触发自动滚动、`scroll_offset` 峰值 10.58dp。步骤 6：行数 8、去重 8、`#0..#7` 索引连续、无空行与空白占位，文本集合 == 初始集合 == 源码 8 项；滚轮 -480/+480 使 `scroll_offset` 在 0↔20 双向变化，末尾把首个可见行拖到末个可见行仍提交（`moved #0 → #7`） | | 改判 BLOCKED→PASS：26 日那轮受会话锁定所迫只走 `PostMessage`，按下事件被条目点击消费，16 次起拖零变化，先前结论是通道限制而非控件缺陷。取证过程中暴露并修复一个产品缺陷：Inspector 的 `/api/tree` 等端点在 worker 线程直接下树遍历，与拖拽期的改树并发即 use-after-free（宿主 `0xC0000005`，cdb 栈 `route_request → Inspector::tree_json_full → dump_tree_json_full`，无 marshal 帧）——同一脚本修复前第 4 次拖拽进程即亡（`t010_v3.txt`、`t010_isolate_before_fix.txt`），修复后 11 次拖拽加全程 `/api/tree` 轮询存活（`t010_v3d.txt`）。修复为树端点统一经主线程 marshal（`InspectorServer::Impl::on_tree`），并补 `utest_inspector_server.tree_routes_marshal_tree_traversal_to_the_poster_thread`；变异自证：去掉 marshal 后该用例转红（排水超时）。另澄清：本用例六步无键盘要求，26 日备注的「键盘重排通道无外泄观测量」不属本用例判据 |
| TC-WIDGET-011 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2/3：`demo_pickers` 中 `DatePicker` 与 `TimePicker` 顶边对齐（同一 dp 90 起），日期网格 7 列间距均匀、行间距一致，`ColorPicker` 两行色块的墨水带为 dp 140..166.7 与 dp 168..194.7，高度均为 27.3dp、两行间距 2dp 一致，无逐个走样；步骤 4/5：`demo_button` 三个按钮（`-1`/`+1`/`reset`）的墨水带完全重合于客户 px 19..74（同一 56px 带）→ 同高且文字基线共线，控件树读出三者 `corner_radius` 均 6.0、`padding` 均 {top 6, left 12, right 12, bottom 6}、`font_size` 均 14，文字垂直居中于按钮盒 | | 帧为 `PrintWindow` 抓取，判定前先做非背景采样确认非空帧（本用例两帧采样分别 3110 与 1293 个暗色点）；两载体退出码均 0 |
| TC-WIDGET-012 | 2026-09-28 | Qoder Agent | PASS | | 解锁会话用真实 `SendInput` 复跑（绝对坐标点击、按键走扫描码、字符走 `WM_CHAR`）。步骤 2/3：点击第一个输入框后逐字符输入 `abc123`，值逐字符增长 `a`→`ab`→…→`abc123` 无丢字，光标 x 单调右移 55→73→87→103→120→136，聚焦底色判据全程成立；步骤 4：`VK_LEFT` ×3 → 光标 x 136→87、值仍 `abc123`；步骤 5：插入 `X` → 值 `abcX123`、光标 x=103，即新字符落在光标处、非追加末尾、未覆盖相邻字符；步骤 6：`Shift`+`VK_LEFT` ×2 → 选区色像素 0→829，值不变；步骤 7：一次 `VK_BACK` → `abcX123`→`ab123`（恰删 2 个字符 == 选区长度，整体删除而非仅删一个），选区像素回落 0、光标停在删除位置 x=73。对照：点第二个输入框后注入 `Q` 只上屏到第二框（`values=['ab!#123','Ada LovelaceQ']`），第一框不动 | | 26 日 FAIL 的步骤 5 是产品缺陷，修复后复跑改判 PASS：`TextInput` 未覆写 `wants_navigation_keys()`，←/→ 被几何焦点导航认领——焦点一旦移到兄弟控件，此后所有按键（含字符与退格）都发给新焦点，输入框当场失焦、再也无法用键盘编辑，外在表现即「方向键之后不再收字」。修复见 `specification/05-event-navigation.md` §4.2（`TextInput` 认领 ←/→、`RichTextEdit` 认领 ←/→/Home/End，↑/↓ 仍回落焦点导航），单测 `utest_dispatcher.direction_keys_reach_a_focused_text_input_before_focus_navigation`（变异自证：关掉认领即转红）。同轮修复第二缺陷：中部 `Shift`+左键扩选时锚点误取插入点下标，每次多选一个字符（`utest_text_input.shift_arrow_selects_exactly_one_char_per_press`）。另记录一处未实现项（不属本用例判据，未修）：`Home`/`End` 在 `TextInput` 上是 no-op——`VK_END`+插 `!`、`VK_HOME`+插 `#` 后光标不动，字符仍落在原光标位（`ab123`→`ab!123`→`ab!#123`） |
| TC-WIDGET-013 | 2026-09-28 | Qoder Agent | PASS | | 解锁会话用真实 `SendInput`（按键走扫描码以过输入法）在 `demo_text_input` 上按七步复跑，输入法=微软拼音（`hkl=0x08040804`，中文态）。步骤 1：点击输入框并清空 → `value=''`；步骤 2：中文态逐字母 `z h o n g w e n` → `value` 全程为空（拼音不进数据模型，契约正确），文字带墨点逐键增长 279→462→628→792→1002→1234→1433→1597、光标 x 52→70→88→106→124→153→169→187 单调右移，即 preedit 全程可见、无丢字、无跳位；步骤 4：`VK_SPACE` 选首候选 → `value='中文'` 恰两字，墨点由 1597 回落 560（preedit 撤下只剩正式文本）、光标 x=96；步骤 5 判据：无拼音残留（全串非 ASCII）、无重复上屏（`len(set)==len`）；步骤 6/7：`Shift` 切英文态续输 `ci` → `value='中文ci'`，英文紧贴中文字符之后、光标右移到 115 位于末尾。对照（取消路径）：中文态组合 `zh`（墨点 462、光标 70）→ `VK_ESC` → 回到占位底图（墨点 1318、无光标）且 `value` 仍空，随后注入字符 `9` 正常上屏 → 撤组合无残留、键盘通道即时恢复 | | 26 日 BLOCKED 的根因已查明并修复，故改判 PASS。断点**不在输入法**：临时在宿主与派发链插桩取证，`WM_IME_STARTCOMPOSITION` → `WM_IME_COMPOSITION(lp=0x1B8，即 GCS_COMPSTR、COMPATTR、COMPCLAUSE、CURSORPOS、DELTASTART)` → 上屏帧 `lp=0x1C00`（`GCS_RESULTSTR`，桥侧解出 `committed="你好"`）逐条抵达，`emit_state` 投出的 `TextCompositionEvent` 亦非空；真正的丢失点在**载体**——`examples/demos/demo_common.h` 的 `run_demo` 手写派发只认 `MouseEvent`/`KeyEvent`/`ScrollEvent`/`TextInputEvent`，缺 `TextCompositionEvent` 分支，组合事件在 demo 里被静默丢弃，表现与「输入法失效」无异。修复即补齐该分支（与 `Application` 的 `WindowHost::dispatch` 同口径），属 demo 载体、按 AGENTS.md §7 不附单测；`Application` 路径本就正确，这解释了仓库探针 `aurora_verify_win32_ime --interactive` 早先能看到 preedit 而 demo 看不到。同轮做过 A/B：在建桥处额外 `ImmGetContext`+`ImmReleaseContext` 认领上下文，对消息投递毫无影响（两侧 trace 逐条一致），该改动已回退未采纳。步骤 3 的子项「候选窗口位置贴近光标」本机无法程序化取证：微软拼音的候选 UI 由 `TextInputHost.exe` 的「Windows 输入体验」全屏 `Windows.UI.Core.CoreWindow` 承载，窗口矩形恒为整屏 (0,0,2560,1440)、`PrintWindow` 抓回全黑，读不出候选列表落点；库内侧仅能确认 `ImmSetCandidateWindow(CFS_CANDIDATEPOS 与 CFS_EXCLUDE)` 已按 preedit 插入点盒提交定位（代码级证据）。接线约束已回写 `GUIDELINE.md` §39.1 |
| TC-WIDGET-014 | 2026-09-26 | Qoder Agent | SKIP | | 用例未执行：本机无可驱动的系统读屏工具——会话锁定下无法启动并操作讲述人，也无法以音频确认朗读结果。按 §1.3「无该工具时记 `SKIP`」处置 | | 补充证据（不作为本用例判定依据）：仓库真机探针 `aurora_verify_win32_ua`（与读屏同路径的 UIA 客户端，当期构建，退出码 0）在 ControlView 下遍历到 18 个节点，其中 `FrameworkId=Aurora` 的自有节点 12 个，`Button`/`Checkbox`/`Slider`/`TextInput`/`Text` 五类 `ControlType` 均出现且 `Name` 非空（如 `确定`/`启用自动更新`/`音量`/`请输入`），`Invoke`/`Toggle`/`Value`/`RangeValue` pattern 均可取到，`labelled-by` 关系可导航；`TextPattern` 对只读控件不暴露属设计降级 |
