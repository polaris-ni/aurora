# theming 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`theming/` —— 主题层（14 个公共头），在实测依赖分层中位于 L5。

| 头 | 职责 |
|:---|:---|
| `theme.h` | 主题值对象：色角色集、排版字阶、CSD 角色组、四组度量令牌、布局惯例组与明暗档位，浅色/深色生成器与两轴解算入口 `from_style` / `default_style` / `defaults` |
| `theme_style.h` | 风格轴枚举 `ThemeStyle{Material, Windows, MacOs, Gnome, Kde}`（与明暗轴正交） |
| `color_roles.h` | 色角色全集（Material 3 命名 + Aurora 扩展角色） |
| `type_scale.h` | 排版 15 档 |
| `csd_roles.h` | 窗口装饰（CSD）组件级配色角色组 |
| `shape_scale.h` | 形状档位令牌（六档圆角，逻辑 dp） |
| `spacing_scale.h` | 间距档位令牌（留白与内边距，逻辑 dp） |
| `control_metrics.h` | 控件基准度量（控件高、命中下限、焦点环、滚动条等，逻辑 dp） |
| `layout_conventions.h` | 布局与交互惯例组（对话框按钮序 / 标题对齐 / 菜单栏位置 / 滚动条模式 / 焦点环形态 / 动效倍率） |
| `csd_style_bridge.h` | Theme → CSD 派生通道（typed） |
| `theme_scope.h` | 主题作用域（`ThemeScope`，继承自通用 `Provider`） |
| `theme_query.h` | 按条件查询主题值 |
| `theme_extension.h` | 类型安全扩展（应用自定义令牌面） |
| `color_scheme.h` | 明暗档位枚举与 OS 偏好口径 |

本模块的人工增益集中在**视觉可判定性**：主题的本质产物是颜色与对比度，而「文字在背景上是否清晰可读」「两个主题是否真的明暗相反」这类判断只能由人眼完成。程序能断言 `Theme::dark().roles.surface` 的值等于某个具体色值，但断言不了这个色值在屏幕上看起来是否像深色主题。

### 1.2 执行载体

复用既有示例程序并新增一份风格对照载体，不新增其他载体：

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_theme.cpp` | `demo_theme` | 浅色与深色两个主题作用域并排呈现，及其对子树的影响边界（`TC-THEMING-001` 至 `TC-THEMING-004`） |
| `examples/demos/demo_theme_styles.cpp` | `demo_theme_styles` | 五风格档 × 明暗两档十格并排、单格大图与令牌数值表、本机实况区的两轴重绘、实开对话框的按钮序、字体族缺失回退（`TC-THEMING-005` 至 `TC-THEMING-012`） |

`demo_theme` 在同屏并列放置两个 `ThemeProvider`，分别注入 `Theme::light()` 与 `Theme::dark()`，两者内部包裹的是**同一个控件类** `ThemedBox`。该控件在绘制时经 `ctx.environment<Theme>()` 读取主题；`Window` 的默认根注入保证主题恒存在，控件内未注入主题的回退路径不再可达。因此该载体可同时验证「注入生效」「作用域隔离」两项，且同一控件类在两个作用域下呈现不同颜色，恰好排除了「颜色来自控件自身」的解释。

**`demo_theme` 不覆盖风格轴**：它注入的两枚主题都是 Material 档的明暗两列，且该 demo 的页面底色与文字色写死在调色板常量里（`examples/demos/demo_common.h` 的 `Card` 与 `pal::AURORA_SURFACE`），根注入解算出的平台风格档对它没有任何影响。风格轴的对照由 `demo_theme_styles` 承担，两份载体的分工是**互不重叠**的：前者判「注入是否落到子树」，后者判「落到子树的风格档看起来像不像那个平台」。

`demo_theme_styles` 的四处可观察面与用例的对应关系：

| 观察面 | 载体侧形态 | 用例 |
|:---|:---|:---|
| 十格并排 | 滚动视口内五行 × 两列，每格是同一套控件（标题、分隔线、按钮、复选框、滑杆、下拉、正文）包进该档 `ThemeProvider`；同行两格只差明暗档，同列五格只差风格档 | TC-THEMING-006 |
| 单格大图 | 点格右上角 `Zoom` 进大图：同套装放大 + 该档全部标定令牌的数值表（含角色十六进制值）；`Back` 回全览 | TC-THEMING-007 |
| 本机实况区 | 页面底色由载体经 `inherit_theme(ctx)` 读**根注入**主题；风格 / 明暗两个下拉分别驱动 `Window::set_theme_style` 与 `Window::set_os_color_scheme`，实况套装不显式设色 | TC-THEMING-008 |
| 实开对话框 | `Confirm dialog` 按当前实况主题起 `confirm`，判读 `conventions.dialog_button_order` 的分档排布 | TC-THEMING-009 |

该载体的页面底色**刻意不写死**——底色是唯一能证明「风格档经 `Window` 的优先链进入子树」的面，这与 `demo_theme` 的做法相反。字体族则按各档标定值显式喂进 `Text`（`Text` 目前不自动读字阶，见 §1.8），故十格同时把「族名声明」与「字号」两件事摆在一起；本机缺哪族字体由 `TC-THEMING-010` 就地判定。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja）；`TC-THEMING-011` / `TC-THEMING-012` 需 Linux 桌面会话或无桌面环境的容器 |
| 构建 | 库已构建；载体为按需目标，不在默认构建内 |
| 终端 | 支持 UTF-8 输出 |
| 显示 | 需可准确辨认颜色的显示器；如需留存证据，可对窗口截图 |
| 缩放 | 本机为 150% DPI（逻辑 760×800 的客户区对应物理 1140×1200 像素）；按缩放折算逻辑尺寸时以系统缩放档位为准 |
| 字体 | `TC-THEMING-010` 需要一台**不安装**四档标定族名（`Segoe UI Variable Text` / `SF Pro Text` / `Adwaita Sans` / `Noto Sans`）的机器；Windows 本机默认即满足，系统字体目录只登记 `Segoe UI Variable` 与 `Noto Sans SC` 等同形异名的族 |
| 分离输出 | 诊断日志写 stderr |
| 远程检视（可选） | 设 `AURORA_INSPECTOR_PORT` 后载体起 Inspector，可用 `GET /api/debug/snapshot?source=fb` 取整窗像素、`GET /api/debug/pick?x=&y=` 读逻辑坐标处的布局盒、`POST /api/input/pointer` 驱动下拉与按钮。人工判读不依赖它，但「肉眼分不清两枚蓝」的场合（如 `#005A9E` 与 `#1C71D8`）用它取像素值可把判读变成可复核的数值 |

本模块**不含**经共享 `State<Theme>` 的运行时换肤时序（`ThemeScope` 支持，但两份载体都是静态注入或经 `Window` 入口切换，无「运行中改共享主题值」的入口），该方向列入 §1.8。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `theming`，即 `TC-THEMING-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
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

**模块级约定**：本模块两份载体的构建统一置于 `TC-THEMING-001` 的步骤 1，一条命令列出全部目标（`demo_theme` 与 `demo_theme_styles`）；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。每份载体另有各自的启动基线：`demo_theme` 的基线即 `TC-THEMING-001` 本体，`demo_theme_styles` 的基线是 `TC-THEMING-005`（只启动与清点，不再构建）——`TC-THEMING-002` 至 `004` 依赖前者，`TC-THEMING-006` 至 `012` 依赖后者。

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

本模块按「人工判定是否优于程序判定」取舍，不追求对 theming/ 全部头文件的全覆盖。

**纳入人工测试的子域**：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 主题的视觉明暗 | 「深色主题看起来确实是深色」属感知判断，程序只能比对色值 |
| 文字对比度 | 可读性是感知属性，程序只能算对比度比值而无从判断实际观感 |
| 作用域隔离 | 「主题只影响作用域内的子树」需同屏并列对照才能排除全局污染的伪像 |
| 同一控件的跨主题差异 | 用同一个控件类在两个作用域下呈现不同，是排除「颜色写死在控件里」的唯一直观证据 |
| 跨风格档的观感分野 | 「Windows 档看起来像 Windows、KDE 档看起来像 KDE」是纯感知判断；程序能断言 `from_style(Kde, Dark).roles` 逐值等于标定表，却断言不了那套值并排摆出来后像不像 Breeze。载体 `demo_theme_styles` 的十格并排（TC-THEMING-006）与实开对话框（TC-THEMING-009）承担该子域 |
| 风格档经根注入落到实况 | 换档后「整窗底色与控件配色是否同时跟上、有无残留旧色块」是像素面判断；程序侧只能断言解算结果与缓存失效，看不见屏幕（TC-THEMING-008） |
| 令牌数值表与画面自洽 | 大图档把该档全部标定值以文本列出，人眼核对「表里写的 `primary` 就是画面上那枚按钮的颜色」——这是把「查表值」与「观感值」对上号的唯一场合（TC-THEMING-007） |
| 声明字体族缺失时的回退 | 「字还在不在、是不是豆腐块、大小是否合理」是感知判断；程序只能断言解析链落到哪枚面，判不出渲染结果（TC-THEMING-010） |

**不纳入人工测试的头及其理由**：

| 头 | 理由 | 已有覆盖 |
|:---|:---|:---|
| `theme.h` 的角色集与生成器 | 浅色/深色主题的角色取值为确定性常量；其**感知正确性**由 TC-THEMING-002 与 TC-THEMING-004 覆盖 | `utest_theme` |
| `theme.h` 的 `ThemeExtension<T>` 注册表 | 类型键的注册与查询为确定性逻辑，且本载体未展示扩展 | `utest_theme_extension` |
| `theme.h` 的 `from_style` / `default_style` / `defaults` | 两轴解算是纯查表：给定（风格档, 明暗档）得到逐字段确定的主题，人眼在这件事上没有程序给不出的信息——程序直接比整棵 `ColorRoles` 树。观感面（「这档像不像那个平台」）才是人工的活儿，归上一表 | `utest_theme_presets` |
| `theme_style.h` | 只有一个枚举与其字符串化，无视觉表现；「哪台机器解算出哪一档」的判据在环境证据侧 | `utest_theme_probe` |
| `shape_scale.h` / `spacing_scale.h` / `control_metrics.h` / `layout_conventions.h` | 四组令牌的登记与逐风格覆写为确定性常量表；令牌的**消费面**（控件默认值真的读到它）由像素与几何断言更准，人眼分不清 4dp 与 6dp 圆角。其「表值与画面同源」由 TC-THEMING-007 的令牌数值表就地核对一次 | `utest_theme_tokens` / `utest_button` / `utest_dialog` |
| `color_scheme.h` | 枚举与 OS 偏好读取口径为确定性逻辑；「`Unspecified` 折不折平成明色」这条观感判据由 TC-THEMING-008 覆盖，窗口装饰侧的明暗观感在 `manual-test/10-window.md` | `utest_theme_csd_bridge` |
| `color_roles.h` / `type_scale.h` / `csd_roles.h` | 角色取值、字阶与 CSD 配色的登记为确定性逻辑，其感知正确性由 TC-THEMING-002 / 004（明暗两列）与 TC-THEMING-006 / 010（十格字阶与族名回退）覆盖 | `utest_theme_roles` / `utest_theme_csd_roles` |
| `theme_scope.h` | 作用域压栈/弹栈与继承为确定性逻辑；其**隔离效果**由 TC-THEMING-003 覆盖。经共享 `State<Theme>` 的运行时换肤需专门的换肤入口，两份载体都不含，故不立用例 | `utest_theme_scope` |
| `theme_query.h` | 条件查询为确定性求值，无视觉表现；三处兜底恒等于 Material 基线这条不变量由断言守 | `utest_theme_query` |
| `csd_style_bridge.h` | 「预设基座 + CSD 角色组派生」是逐字段映射，且派生结果的机度与字形口径由窗口装饰侧的用例覆盖，本模块不重复 | `utest_theme_csd_bridge` |

**载体的已知局限（现状，非计划）**——以下六条决定了本模块的用例边界，读者据此判断「某档看起来如何」这类问题为什么没有对应用例：

1. 六枚布局惯例里只有 `dialog_button_order` 与 `dialog_title_alignment` 有绘制侧消费者（两处都在 `include/aurora/widget/dialog.h`）；`focus_ring_style`、`scrollbar_mode`、`menu_bar_placement`、`motion_scale` 四枚登记后无消费者，`utest_theme_tokens` 只断言其值。故 macOS 档声明的 `FocusRingStyle::OuterGlow` 与 macOS / GNOME 档声明的 `ScrollbarMode::Overlay` **在屏幕上不可观察**，本模块不为其立用例；画面上的焦点环只由 `roles.focus_ring` 与 `controls.focus_ring_width` / `focus_ring_inset` 三枚决定，TC-THEMING-009 里 `Yes` 按钮的外框即这一条。
2. `dialog_title_alignment` 虽有消费者，但五档都不覆写它（全取基线 `Leading`），分野为零 ⇒ TC-THEMING-009 只判按钮排布，标题对齐无从比对。
3. `Dialog` 只铺遮罩并居中内容，**不画卡面**（其几何契约把卡片宽与底色交给宿主）：本载体的卡片底色、圆角、内边距由 demo 侧自绘。故对话框用例判的是按钮排布与遮罩对比，不是「卡片像不像该平台的对话框」。
4. `Dropdown` 的盒面与面板底色写死为白，只有强调色跟随主题——深色档里下拉盒仍呈白底，十格与实况区都是这个现状，判读时不当作主题缺陷。
5. `Text` 不自动读 `type_scale`，族名与字号由载体显式喂入。故本模块证明的是「标定值读得到、喂进去画面跟得上」，控件自动读字阶这条尚未接线（消费面现状见 `codespec/specification/07-environment-modifier.md`）。
6. 展开的下拉面板按树序绘制，其后的兄弟节点会盖住它。判读十格并排时以面板收起态为准；TC-THEMING-008 需要展开面板取字时，先把该格滚到视口末位再点。

另：经共享 `State<Theme>` 的运行时换肤（`ThemeScope` 支持）在两份载体上都没有入口——`demo_theme` 是静态注入，`demo_theme_styles` 经 `Window` 的风格 / 明暗入口切换——故该方向仍不立用例，与 §1.3 末一致。

## 2 用例清单

### 2.1 载体基线

#### TC-THEMING-001 载体构建与启动基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-001 |
| 测试目的 | 确认载体可构建、启动并同时呈现两个主题作用域，为后续用例提供基线 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；本机工具链可用 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建两份载体：`cmake --build build --target demo_theme demo_theme_styles`（纯执行，无预期结果）<br>2. 启动 `./build/demo_theme.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>3. 查看窗口标题与客户区尺寸<br>4. 清点窗口内自上而下呈现的内容项<br>5. 查看 `err.txt` 的内容与行数 |
| 预期结果 | 3. 标题为 `Theme · Aurora Demo`，客户区约为 540×480<br>4. 共 4 项：标题、一行说明文字、标注 `light theme` 的盒子、标注 `dark theme` 的盒子；两个盒子之间有明显间隔<br>5. 恰 1 行 `INF` 级 `[run_demo] window shown: Theme · Aurora Demo`；无 `ERR` / `FTL` 级日志 |

### 2.2 主题注入的视觉效果

#### TC-THEMING-002 浅色与深色作用域呈现明暗相反的配色

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-002 |
| 测试目的 | 验证注入的主题确实被控件读到并用于绘制，且两个主题的明暗取向符合预期 |
| 前置条件 | 载体 `demo_theme` 已构建成功；显示器颜色可准确辨认 |
| 依赖用例 | TC-THEMING-001 |
| 操作步骤 | 1. 启动 `./build/demo_theme.exe`（纯执行，无预期结果）<br>2. 观察 `light theme` 盒子的背景色与文字色<br>3. 观察 `dark theme` 盒子的背景色与文字色<br>4. 对比两个盒子的整体明暗关系<br>5. 观察两个盒子顶部色条的颜色 |
| 预期结果 | 2. `light theme` 盒子背景为浅色（明显亮于卡片背景或与之接近），文字为深色<br>3. `dark theme` 盒子背景为深色，文字为浅色<br>4. 两个盒子的明暗**明显相反**——一个以浅色底深色字呈现，另一个以深色底浅色字呈现<br>5. 两个盒子顶部色条的颜色不同（分别取自各自主题的主色），可直接区分 |

#### TC-THEMING-003 主题作用域不泄漏到兄弟与祖先节点

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-003 |
| 测试目的 | 验证主题只影响作用域内的子树，不污染同级的其他节点 |
| 前置条件 | 载体 `demo_theme` 已构建成功 |
| 依赖用例 | TC-THEMING-001, TC-THEMING-002 |
| 操作步骤 | 1. 启动 `./build/demo_theme.exe`（纯执行，无预期结果）<br>2. 观察窗口标题与说明文字行的配色（这两项位于任何 `ThemeProvider` 之外）（纯执行，无预期结果）<br>3. 记录标题与说明文字的背景色与文字色<br>4. 记录两个盒子之间间隔区域的背景色（纯执行，无预期结果）<br>5. 对比步骤 3、4 的背景色与 `dark theme` 盒子的深色背景 |
| 预期结果 | 3. 标题与说明文字的背景停留为页面默认（浅色卡片底），文字为深色——**未**被 `dark theme` 作用域染成深底浅字<br>5. 两个盒子之间的间隔区域背景为页面默认色，未被任一主题的底色填充；即深色仅出现在 `dark theme` 盒子的范围内，边界清晰，无渗漏 |

#### TC-THEMING-004 两种主题下文字与背景对比度均可读

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-004 |
| 测试目的 | 验证两套主题的配色均满足基本可读性，不出现低对比度导致文字难辨的情况 |
| 前置条件 | 载体 `demo_theme` 已构建成功；显示器颜色可准确辨认；建议在正常室内光线下观察 |
| 依赖用例 | TC-THEMING-001, TC-THEMING-002 |
| 操作步骤 | 1. 启动 `./build/demo_theme.exe`（纯执行，无预期结果）<br>2. 注视 `light theme` 盒子内的文字，判断其与背景的对比（纯执行，无预期结果）<br>3. 判断该文字是否无需费力即可辨认<br>4. 对 `dark theme` 盒子内的文字重复同样判断（纯执行，无预期结果）<br>5. 对两个盒子顶部色条与其相邻背景的交界处重复同样判断 |
| 预期结果 | 3. `light theme` 盒子内的文字清晰可辨，无需贴近屏幕或放大才能阅读<br>5. `dark theme` 盒子内的文字同样清晰可辨；两处色条与背景的交界均清晰，不存在「浅色条压在浅色底」或「深色条压在深色底」这类难以区分的组合 |

### 2.3 风格档对照（载体 `demo_theme_styles`）

#### TC-THEMING-005 第二载体启动基线：实况区与十格全览同屏

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-005 |
| 测试目的 | 确认风格对照载体可启动并同时呈现实况区与十格全览，为 `TC-THEMING-006` 至 `TC-THEMING-012` 提供基线 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置且两份载体均已构建；本机为带桌面会话的 Windows |
| 依赖用例 | TC-THEMING-001 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>2. 查看窗口标题与客户区尺寸<br>3. 自上而下清点页面结构项<br>4. 读出实况区三行启动读数的内容<br>5. 查看 `err.txt` 的内容与行数，以及 `out.txt` 的字节数 |
| 预期结果 | 2. 标题为 `Theme Styles · Aurora Demo`（分隔符 U+00B7），客户区逻辑尺寸 760×800（本机 150% 缩放下对应物理 1140×1200）<br>3. 共 13 项，自上而下依次为：大标题 `Theme styles · side-by-side carrier`、`probe` 读数行、`default` 读数行、`boot readings` 提示行、分隔线、含两枚下拉的一行、`Confirm dialog` 按钮、分隔线、含 `Live button` 与复选框的一行、滑杆、分隔线、图注 `Grid: 5 styles x 2 schemes -- click Zoom`、滚动视口<br>4. 三行读数逐字为 `probe Windows / Win32 / desktop=Unknown`、`default=Windows os=Light setter=(unset)`、`boot readings above; live strip below must repaint`；前两行的平台名与风格档随本机变（Windows 上恒为 `Windows / Win32` 与 `Windows`），`os=` 取本机系统明暗偏好<br>5. `err.txt` 恰 1 行 `INF`，含 `[demo_theme_styles] window shown (close window to exit): default_style=Windows scheme=Light`，无 `ERR` / `FTL`；`out.txt` 为 0 字节 |

#### TC-THEMING-006 十格并排的跨风格观感分野

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-006 |
| 测试目的 | 验证五套风格档并排时观感分野可辨，且同一行两格之间只差明暗档 |
| 前置条件 | 载体 `demo_theme_styles` 已构建成功；显示器颜色可准确辨认 |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe`（纯执行，无预期结果）<br>2. 读出滚动视口首屏可见的格与其角标文字<br>3. 向下滚动到末行，清点十格角标文字的全集与排布方向<br>4. 对比同一行左右两格的整体明暗与强调色深浅<br>5. 对比同一列五格的强调色（`Alpha` 按钮与滑杆填充）<br>6. 对比 Material 行与其余四行的正文观感（同一句 `The quick brown fox 0123456789`） |
| 预期结果 | 2. 首屏见约三行（视口高 460 逻辑、全览内容高 611 逻辑），左上角标为 `Material / Light`，其右为 `Material / Dark`<br>3. 角标恰十枚且行序为风格档、列序为明暗档：`Material / Light`、`Material / Dark`、`Windows / Light`、`Windows / Dark`、`macOS / Light`、`macOS / Dark`、`GNOME / Light`、`GNOME / Dark`、`KDE / Light`、`KDE / Dark`<br>4. 每行左格浅底深字、右格深底浅字，明暗相反；同列同族强调色在暗档转为更浅的一档（如 Windows 行左深蓝、右浅蓝）<br>5. 五档主色彼此可辨：Material 纯蓝、Windows 深蓝、macOS 亮蓝、GNOME 蓝、KDE 青。其中 KDE 的青与其余四档的蓝一眼可分；Windows 与 GNOME 两枚蓝需按 §1.3 的像素口径复核或改看卡片底色<br>6. Material 档正文明显大于其余四档，300 逻辑宽格内该句在 Material 行折成两行、其余四行均单行 |

#### TC-THEMING-007 点格放大后的大图与令牌数值表自洽

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-007 |
| 测试目的 | 验证单格大图把该档标定令牌以文本列出，且表值与画面取色同源、不各说各话 |
| 前置条件 | 载体 `demo_theme_styles` 已构建成功 |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe`（纯执行，无预期结果）<br>2. 点 `Material / Light` 格右上角 `Zoom`，观察大图形态<br>3. 读该大图令牌数值表的前三行<br>4. 对照大图内 `Alpha` 按钮的取色与步骤 3 表中的 `primary` 值<br>5. 点右上角 `Back` 回全览<br>6. 点 `KDE / Dark` 格的 `Zoom`，读其表的首行、`roles` 行与 `body` 行<br>7. 对比两张大图内正文行的字号观感 |
| 预期结果 | 2. 视口内只剩这一张大图，格宽由 300 逻辑变 580 逻辑，右上角按钮文字由 `Zoom` 变 `Back`，套装下方多出分隔线与十行令牌数值表<br>3. 前三行逐字为 `style=Material scheme=Light`、`roles surface=#FFFFFF container=#EEEEEE primary=#0000FF on_primary=#FFFFFF`、`roles on_surface=#000000 outline_variant=#C6C6C6 error=#E01B24 focus_ring=#000000`<br>4. 二者一致——画面按钮的蓝就是表里写的 `#0000FF`，不出现「表里一个值、画面另一个值」的分叉<br>5. 回到五行 × 两列的全览，`Material / Light` 格恢复 300 逻辑宽、按钮文字恢复 `Zoom`<br>6. 首行 `style=KDE scheme=Dark`，`roles surface=#202326 container=#202326 primary=#3DAEE9 on_primary=#FCFCFC`，`body = 10.0 pt family = Noto Sans`<br>7. KDE 大图正文明显小于 Material 大图正文（10.0 pt 对 14.0 pt），两者均完整成字形、无缺字框 |

#### TC-THEMING-008 实况区随两轴切换整窗重绘

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-008 |
| 测试目的 | 验证经 `Window` 的风格 / 明暗入口换档后整窗底色与实况控件同时跟上，且十格标定档不受污染 |
| 前置条件 | 载体 `demo_theme_styles` 已构建成功；本机为带桌面会话的 Windows |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe`（纯执行，无预期结果）<br>2. 记录初始整窗底色与两枚实况按钮（`Confirm dialog`、`Live button`）的取色<br>3. 点开风格下拉，清点其选项与初始选中项<br>4. 选 `KDE`，观察整窗底色与实况控件<br>5. 点开明暗下拉选 `Dark`，观察同一处<br>6. 把明暗下拉选回 `Unspecified`，观察同一处<br>7. 检查滚动视口内的十格全览是否随上述切换改变 |
| 预期结果 | 2. 底色为浅灰（本机实测 `#F3F3F3`）、两枚按钮为深蓝（实测 `#005A9E`），即本机探测档 `Windows / Light`<br>3. 选项恰五枚且顺序为 `Material`、`Windows`、`macOS`、`GNOME`、`KDE`；初始选中项是 `Windows`（本机解算档）而非首项 `Material`<br>4. 两枚实况按钮同时转为青色（实测 `#3DAEE9`），整窗底色同时转为更冷的浅灰（实测 `#EFF0F1`），标题与读数文字仍清晰；无「按钮已变、底色未变」或局部残留旧蓝的撕裂<br>5. 整窗底色翻为深色（实测 `#202326`），大字与标题转为浅色，强调色仍为青色 `#3DAEE9`<br>6. 底色回到步骤 4 的同一浅色（实测 `#EFF0F1`），与 `Light` 档观感无差——`Unspecified` 不折平成第三列，而是解到明色列<br>7. 十格全览逐格不变：每格由自己的 `ThemeProvider` 钉住标定档，切换只作用于根注入所及的实况区与页面底色 |

#### TC-THEMING-009 三档对话框按钮序实开

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-009 |
| 测试目的 | 验证 `conventions.dialog_button_order` 的三种排布在真实对话框上互不相同且与档位一致 |
| 前置条件 | 载体 `demo_theme_styles` 已构建成功；活动桌面会话（锁屏时窗口收不到注入输入） |
| 依赖用例 | TC-THEMING-005, TC-THEMING-008 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe`，把风格下拉选 `Windows`（纯执行，无预期结果）<br>2. 点 `Confirm dialog`，观察对话框卡片内两枚动作按钮的排布<br>3. 点 `No` 关闭对话框，把风格下拉选 `GNOME`（纯执行，无预期结果）<br>4. 再点 `Confirm dialog`，观察按钮排布<br>5. 点 `No` 关闭，把风格下拉选 `macOS`（纯执行，无预期结果）<br>6. 再点 `Confirm dialog`，观察按钮排布<br>7. 任取一档，观察遮罩下页面与卡片上文字的可读性 |
| 预期结果 | 2. 两枚按钮**横排**、位于卡片左下，顺序 `Yes` 在左、`No` 在右（`AcceptFirst`），`Yes` 带焦点外框<br>4. 两枚按钮**横排**、位于卡片右下，顺序 `No` 在左、`Yes` 在右（`AcceptLast`）——与步骤 2 既不同序也不同位<br>6. 两枚按钮**竖排**、右对齐，`Yes` 在上、`No` 在下（`VerticalRightAligned`），与步骤 2、4 的横排形态一眼可分<br>7. 遮罩为半透明黑，页面被压暗但文字仍可辨认；卡片为不透明底面，其上的深色标题 `Dialog conventions` 与正文清晰，无浅字压浅底 |

#### TC-THEMING-010 声明字体族缺失时正文仍完整成形

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-010 |
| 测试目的 | 验证四档标定的平台字体族在本机都不存在时，正文经兜底链完整渲染且不丢字号分级 |
| 前置条件 | 本机系统字体目录不登记 `Segoe UI Variable Text` / `SF Pro Text` / `Adwaita Sans` / `Noto Sans` 四枚族名（Windows 本机默认满足）；载体 `demo_theme_styles` 已构建成功 |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 启动 `./build/demo_theme_styles.exe`（纯执行，无预期结果）<br>2. 逐格读出十格内的正文行 `The quick brown fox 0123456789`<br>3. 进任一档大图，读令牌表 `body` 行的族名，并逐档清点五枚声明族名<br>4. 对照本机字体目录的登记名，判断这五枚族名有几枚真实存在<br>5. 观察这些格的正文有无空框、方块（豆腐块）、缺笔画或整段不渲染<br>6. 对比 Material 档（声明伪族名 `sans-serif`）与其余四档的字形风格 |
| 预期结果 | 2. 十格正文逐格完整可读，26 个字母与数字 0-9 全在，无一格空白或缺字<br>3. 五档声明依次为 `sans-serif`（Material）、`Segoe UI Variable Text`（Windows）、`SF Pro Text`（macOS）、`Adwaita Sans`（GNOME）、`Noto Sans`（KDE）<br>4. 四枚平台族名**一枚都不存在**（本机登记的是 `Segoe UI Variable`、`Noto Sans SC` 等同形异名族），只有 `sans-serif` 作为伪族名恒可解析<br>5. 无空框、无豆腐块、无缺笔画；文字全部落到兜底面，字号仍等于各档标定值，故 Material 档最大、KDE 档最小的分级关系保持<br>6. 四档缺失族的字形彼此看不出差别（同一副兜底面）——本用例因此只判「回退是否无损」，不判「字形像不像该平台」；后者需装上真族名的机器另判 |

### 2.4 Linux 外壳与回落

#### TC-THEMING-011 Linux 双外壳真机切换

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-011 |
| 测试目的 | 验证 Linux 上的桌面外壳探测确实解算出对应风格档，同一份二进制在两外壳下观感分野 |
| 前置条件 | Linux 桌面会话各一次（GNOME 与 KDE Plasma），WSLg 或真机均可；仓已 configure 并构建 `demo_theme_styles` |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 在 GNOME 会话下启动 `./build/demo_theme_styles.exe`，读 `probe` 行的 `desktop=` 值与 `default=` 值<br>2. 记录实况区底色与两枚实况按钮的取色<br>3. 在 KDE Plasma 会话下启动同一二进制，读同样两行<br>4. 记录 KDE 会话下的实况区取色<br>5. 对比两次运行的整窗观感分野 |
| 预期结果 | 1. `desktop=Gnome` 且 `default=GNOME`——探测到的外壳直接解算成同名风格档<br>2. 底色与强调色为 Adwaita 明档取值（浅底近白 `#FAFAFB`、蓝 `#1C71D8`）<br>3. `desktop=Kde` 且 `default=KDE`<br>4. 底色为 Breeze 明档取值（`#EFF0F1`）、强调色为青 `#3DAEE9`<br>5. 两次是同一份二进制、未经任何重编译，分野全部来自运行期探测；风格下拉的初始选中项在两机上分别显示 `GNOME` 与 `KDE` |

#### TC-THEMING-012 无桌面环境证据时回落 Material

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-THEMING-012 |
| 测试目的 | 验证 Linux 上取不到桌面外壳证据时 `Theme::default_style()` 回落 Material，实况区呈现 Material 档 |
| 前置条件 | Linux 会话但不声明桌面外壳（清空 `XDG_CURRENT_DESKTOP` 与 `DESKTOP_SESSION`，如裸 X11 / Xvfb 容器）；载体 `demo_theme_styles` 已构建成功 |
| 依赖用例 | TC-THEMING-005 |
| 操作步骤 | 1. 以 `env -u XDG_CURRENT_DESKTOP -u DESKTOP_SESSION ./build/demo_theme_styles.exe 1> out.txt 2> err.txt` 启动（纯执行，无预期结果）<br>2. 读 `probe` 行的 `desktop=` 值与 `default=` 值<br>3. 记录实况区底色与两枚实况按钮的取色<br>4. 与十格全览里 `Material / Light` 格并排比对<br>5. 查看 `err.txt` 的行数与级别 |
| 预期结果 | 2. `desktop=Unknown` 且 `default=Material`——无证据时回落 Material 基线而不是猜某档<br>3. 强调色为纯蓝 `#0000FF`、页面底为白 `#FFFFFF`<br>4. 实况区与 `Material / Light` 格取色一致，证明回落走的是同一张标定表而非另写一套兜底色<br>5. `err.txt` 首行 `INF` 含 `window shown`，其 `default_style=Material`；无 `ERR` / `FTL` |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-THEMING-001 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：标题 `Theme · Aurora Demo`（分隔符 U+00B7），客户区物理 810×720 px，按本机 144 DPI 折算逻辑 540×480。步骤 4：自上而下 4 项，逐行像素定位为客户区 y=40..110 的渐变标题横幅（均值 102,114,221、取样 109 种色，说明是渐变而非纯色块）、y=140..215 的说明文字行（白底、最深像素 0,0,0）、y=225..345 的 `light theme` 盒、y=357..477 的 `dark theme` 盒，两盒间隔 12 物理 px（8 逻辑 px）且为页面底色。步骤 5：stderr 恰 1 行 `INF` 含 `[run_demo] window shown: Theme · Aurora Demo`，无 `ERR` / `FTL`；stdout 0 行 | | 「明显间隔」按可测量口径核对：间隔非零、为页面默认底色、两盒边框各自独立。载体截图见执行记录附件 `theme_box1.png` / `theme_box2.png`（构建目录 `build/manual-lib/evidence/`，不入库） |
| TC-THEMING-002 | 2026-09-26 | Qoder Agent | PASS | | 步骤 2：`light theme` 盒背景 (255,255,255)、文字最深像素 (0,0,0)。步骤 3：`dark theme` 盒背景 (32,33,36)、文字最浅像素 (255,255,255)。步骤 4：两盒明暗相反，背景 WCAG 相对亮度 1.000 对 0.015，文字与底色对比度分别 21.00 与 16.10。步骤 5：顶部色条颜色不同，light 为 (0,0,255)、dark 为 (90,120,240) | | 色条取值恰为 `Theme::light().primary` 与 `Theme::dark().primary`（`include/aurora/theming/theme.h` 的默认值与 `dark()` 覆写值），且 light 盒文字为 (0,0,0) 而非 `ThemedBox` 未注入主题时的回退色 (17,24,39)——两处都排除了「颜色来自控件自身回退」这一替代解释。另：按 2026-10-13 角色化重构后的现值，上述两项对应 `Theme::light().roles.primary` 与 `Theme::dark().roles.primary`（`color_roles.h`），dark 取值已按 WCAG AA 对比度修正为 M3 色调板值，是该次重构唯一有意的观感变化 |
| TC-THEMING-003 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：标题横幅与说明文字行所在区域的底色为 (255,255,255)（页面卡片底），说明文字最深像素 (0,0,0)，未被染成深底浅字。步骤 5：两盒之间间隔行（y=345..357）在盒左、盒中、盒右三个横向位置取样均为 (255,255,255)；同一水平线上盒子范围之外（客户区 x=5、x=400、x=805）也均为 (255,255,255) | | 深色只出现在 `dark theme` 盒的 y=357..477 与 x≈23..348 范围内，边界逐像素清晰（边框线为 `AURORA_BORDER` 灰），无向祖先与兄弟节点的渗漏 |
| TC-THEMING-004 | 2026-09-26 | Qoder Agent | PASS | | 步骤 3：light 盒文字对底色对比度 21.00（WCAG 2.x 相对亮度比），16 pt 字形在 330×120 物理 px 盒内逐字可辨。步骤 5：dark 盒文字对底色对比度 16.10；色条与其相邻底色对比 light 为 8.59、dark 为 4.12，四处交界均高于 3:1，不存在浅条压浅底或深条压深底 | | 本例的「无需费力即可辨认」已折算为对比度数值判定（阈值取 WCAG AA 的 4.5:1 正文与 3:1 非文本），故代理署名具备可复核依据；观感层面仍建议用户按 §1.7 附的截图复核一次 |
| TC-THEMING-005 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：标题 `Theme Styles · Aurora Demo`（分隔符 U+00B7），Inspector 整帧快照物理 1140×1200 px，本机 150% 缩放折算逻辑 760×800，`/api/debug/pick` 回根 `Stack` 盒 760×800。步骤 3：13 项逐一定位——大标题、三行读数、分隔线、双下拉行、`Confirm dialog` 按钮、分隔线、`Live button` + 复选框行、滑杆、分隔线、图注、滚动视口（`Scroll` 盒 x=12 y=366 w=736 h=460）。步骤 4：三行读数逐字命中。步骤 5：`err.txt` 恰 1 行 `INF`、160 字节，含 `default_style=Windows scheme=Light`，无 `ERR` / `FTL`；`out.txt` 0 字节 | | 载体为 `EXCLUDE_FROM_ALL` 目标，构建统一在 TC-THEMING-001 步骤 1；本次 `cmake --build build --target demo_theme demo_theme_styles` rc=0，唯一告警是他人基线项（`widget/codegen.h` 的 unused parameter）。Inspector 通道为可选取证面，非执行前置 |
| TC-THEMING-006 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：首屏见 Material 行整格 + Windows 行上半，`Scroll` 视口 460 逻辑、全览内容列 611 逻辑。步骤 3：十枚角标逐枚读出，行序 Material / Windows / macOS / GNOME / KDE、列序 Light / Dark。步骤 4：同行左浅右深；Windows 行左按钮 (0,90,158)、右按钮 (118,185,237)。步骤 5：五档主色 Material (0,0,255)、Windows (0,90,158)、macOS (0,122,255)、GNOME (28,113,216)、KDE (61,174,233)。步骤 6：Material 两格该句折两行，其余八格单行 | | 五枚主色与标定表逐值相等（`#0000FF` / `#005A9E` / `#007AFF` / `#1C71D8` / `#3DAEE9`）。步骤 5 里 Windows 与 GNOME 两枚蓝的肉眼区分度低，本例的数值判据取自像素取样；纯目视执行者建议改看卡片底色（四档浅灰各有差）或按 §1.3 起 Inspector 复核 |
| TC-THEMING-007 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：格宽 300→580 逻辑、按钮文字 `Zoom`→`Back`，套装下多出分隔线 + 10 行数值表。步骤 3：前三行逐字命中 `style=Material scheme=Light` / `roles surface=#FFFFFF container=#EEEEEE primary=#0000FF on_primary=#FFFFFF` / `roles on_surface=#000000 outline_variant=#C6C6C6 error=#E01B24 focus_ring=#000000`。步骤 4：`Alpha` 按钮取样 (0,0,255)，与表中 `primary=#0000FF` 同值。步骤 5：点 `Back` 后 stderr 追加 `view=overview index=4`，全览恢复、该格按钮文字回到 `Zoom`。步骤 6：`style=KDE scheme=Dark`、`roles surface=#202326 container=#202326 primary=#3DAEE9 on_primary=#FCFCFC`、`body = 10.0 pt family = Noto Sans`。步骤 7：KDE 大图正文肉眼小于 Material 大图正文，与 10.0 / 14.0 pt 的标定比一致，两者均完整成字形 | | `Zoom`→`Back` 的完整往返在 macOS/Light 格上验证（`view=enlarged index=4` → `view=overview index=4`）；Material/Light 格验证了进大图与整张表值。大图的 `body` 行同时是 TC-THEMING-010 的族名读数来源 |
| TC-THEMING-008 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：底色 (243,243,243)、两枚实况按钮 (0,90,158)。步骤 3：面板五枚选项 Material / Windows / macOS / GNOME / KDE，收起态下拉显示 `Windows`。步骤 4：选 KDE 后底色 (239,240,241)、按钮 (61,174,233)，stderr 追加 `set_theme_style(KDE)`。步骤 5：选 Dark 后底色 (32,35,38)、按钮仍 (61,174,233)，stderr 追加 `set_os_color_scheme(Dark)`。步骤 6：选回 Unspecified 后底色回到 (239,240,241)，与步骤 4 逐值相同，stderr 追加 `set_os_color_scheme(Unspecified)`。步骤 7：切换前后十格角标与取色逐格不变 | | 四组像素值与对应档标定表逐值相等（`#F3F3F3`/`#005A9E`、`#EFF0F1`/`#3DAEE9`、`#202326`），故「整窗重绘」不是观感印象而是同解验证；色值与缩放无关，150% 下取的是物理像素。步骤 6 是「`Unspecified` 不折平成第三列、而是解到明色列」这条口径的目视证据 |
| TC-THEMING-009 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：Windows 档 `Yes` 左、`No` 右横排于卡片左下，`Yes` 带黑色焦点外框，stderr `dialog opened: style=Windows scheme=Light order=AcceptFirst`。步骤 4：GNOME 档 `No` 左、`Yes` 右横排于卡片右下，`order=AcceptLast`。步骤 6：macOS 档 `Yes` 上、`No` 下竖排右对齐，`order=VerticalRightAligned`。步骤 7：遮罩下页面文字仍可辨，卡片为不透明底面、深色标题 `Dialog conventions` 清晰 | | 三次均以点 `No` 关闭，stderr 对应三行 `dialog result=no`，关闭路径同批得到验证。卡面由载体自绘（`Dialog` 只铺遮罩 + 居中内容，见 §1.8 局限 3），故本例不判卡片形态；三档卡底色实测 `#F3F3F3` / `#FAFAFB` / `#ECECEC`，差异细微，未列入判据 |
| TC-THEMING-010 | 2026-11-03 | Qoder Agent | PASS | | 步骤 2：十格正文逐格完整，26 字母与 0-9 全在，无空格。步骤 3：五枚声明族名逐档读出——`sans-serif` / `Segoe UI Variable Text` / `SF Pro Text` / `Adwaita Sans` / `Noto Sans`。步骤 4：本机系统字体目录与字体注册表键（`HKLM` 下的 `CurrentVersion\Fonts`）登记的族名为 `Segoe UI`、`Segoe UI Variable`、`Noto Sans SC`、`Noto Serif SC`（用户级另有 `Ubuntu Mono`），四枚平台族名零命中。步骤 5：无空框、无豆腐块、无缺笔画，Material 最大 / KDE 最小的字号分级保持。步骤 6：四档缺失族的字形彼此无可见差别，与 Material 档同为副兜底面 | | 「某档字形像不像该平台」在本机不可判，已就地记入 §1.8 局限 5 与本例预期 6；`Noto Sans SC` 与 `Noto Sans` 是同形异名的另一枚族，不构成 KDE 档族名命中 |
| TC-THEMING-011 |  |  |  |  |  |  |  |
| TC-THEMING-012 |  |  |  |  |  |  |  |
