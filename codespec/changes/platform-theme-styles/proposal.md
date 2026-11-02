# CHG-057 系统风格主题内置与平台默认解算：风格×明暗两轴正交

| 字段   | 值                                                                                                                                                                                                                                                                                                                                                                                    |
| :--- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 变更编号 | CHG-057                                                                                                                                                                                                                                                                                                                                                                             |
| 提出日期 | 2026-11-02                                                                                                                                                                                                                                                                                                                                                                          |
| 当前状态 | 已提议                                                                                                                                                                                                                                                                                                                                                                                |
| 关联需求 | SPEC.API.SENSIBLE-DEFAULTS.001                                                                                                                                                                                                                                                                                                                                                      |
| 影响面  | `include/aurora/theming/` 新增 `theme_style.h` 与形状/间距/度量/惯例四个令牌头、`theme.h` 聚合面扩字段与四枚静态工厂；`src/aurora/theming/` 新目录（探测 TU + 预设标定表，含桌面环境探测唯一实现体）；`include/aurora/window/platform.h` 新增 `desktop_environment()` 转发与 `os_color_scheme()` 口径注记、`include/aurora/environment/media_query.h` 的 `platform` 字段补「表后端不表 OS 家族」注释（无签名变更）；`include/aurora/window/title_bar_style.h` 的 `CsdPreset` 扩两档（连带 `TitleBarStyle::from_preset` 的穷举 `switch`、派生通道测试与 `examples/demos/demo_csd_gallery.cpp` 的预设循环）；`include/aurora/theming/csd_style_bridge.h` 基座改按风格映射；`include/aurora/window/window.h` 根注入解算链与 `set_theme_style`；首批控件默认值改读令牌；工具链登记面同步（known_enums 登记两枚新枚举并补登既缺位的 ColorScheme、其编译期锚点用例、伞头清单 `[direct]` 新头、`aurora_api.json` 重建）；`tests/unit/` `tests/integration/` 相应批次；`codespec/` 四篇规格（07 / 06 / 04 / 03）+ 四篇顶层文档（ARCHITECTURE / CODING_STANDARDS / SPECIFICATIONS / BUILD_OPTIONS）+ 手工用例；根级 CHANGELOG.md 记行为变更与迁移路径 |

## 动机

1. **主题只有一根轴，风格轴被写死成 Material**。`include/aurora/theming/theme.h` 的 `Theme` 是「Material 3 色角色集 + M3 排版 15 档 + CSD 角色组 + 明暗档」的聚合，内置档位只有 `light()` / `dark()` 两支；`include/aurora/window/window.h` 的 `resolved_root_theme()` 在应用未显式 `set_theme` 时只按 OS 明暗二选一取 `Theme::dark()` / `Theme::light()`。也就是说：**明暗是解算出来的，风格是编译期钉死的**。同一份声明式代码在 Windows、macOS、GNOME、KDE 上拿到的是同一套 Material 观感，框架没有「这台机器的设计语言」这个概念。
2. **换风格只能换色，换不了观感骨架**。形状与间距令牌尚不存在（`Theme` 无 `shapes` / `spacing` 字段），`Button` 的圆角 6、内边距 12/6、各控件的留白与控件基准高全部硬编码在控件公共头里（同一问题在 `codespec/changes/prop-accessor-theme-defaults/proposal.md` 动机第四条被登记为待 CHG-056 解决）。结果是「一套 Windows 风格」无法只靠改配色成立——圆角档位、控件基准高、密度、焦点环形状这些才是设计语言的骨架。
3. **平台原生观感已有真机标定资产，但没有和主题打通**。`include/aurora/window/title_bar_style.h` 已经按 libadwaita / Win11 DWM / macOS 标准窗口三套视觉语言钉过配色与度量（`CsdPreset` 六档 + 三条推导口径分治的理由都写在注释里），`include/aurora/theming/csd_style_bridge.h` 的派生通道却把基座恒取 `CsdPreset::AdwaitaLight` / `AdwaitaDark`——**当前框架在 Windows 上画的是 GNOME 标题栏**。已有的标定值是本提案最省成本的起点，也是「风格」概念在仓库里的既成事实。
4. **平台侧有明暗探测、没有风格探测**。`include/aurora/window/platform.h` 的 `os_color_scheme()` 三平台到位（Win32 注册表、macOS `NSApp.effectiveAppearance`、Linux GTK 配置启发式），`platform()` 给编译期 `PlatformKind`；但没有任何桌面环境概念。Linux 上同一二进制会跑在 GNOME 与 KDE 上，观感应当不同却无从区分——这一条决定了本提案的探测必须是**运行期**的，只靠 `AURORA_PLATFORM_*` 编译期宏不够。
5. **AI-first 视角下默认值错位**。SPEC.API.SENSIBLE-DEFAULTS.001 的口径是「默认值在声明处可见、运行时可查询、代码中可省略」。今天的现实是：省略主题声明的产物在任何平台上都得到 Material 观感，模型无法从「用户在 Windows 上跑」这个事实推出任何观感差异。`Theme::defaults()` 把「跟随这台机器的设计语言」变成零参数的默认路径，且风格枚举可入 `aurora_api.json`、可被代码生成如实枚举。

## 对标分析

| 框架 | 风格轴与明暗轴的分离方式 | 对本提案的启示 |
| :--- | :--- | :--- |
| Flutter | `ThemeData(platform:)` 携带 `TargetPlatform`，`Brightness` 独立；`cupertino.dart` / `material.dart` 双套自适应构造器 | 风格与明暗两轴分离是主流；**构造器按平台分派**是 defaults 的成熟形态 |
| SwiftUI | 设计语言由系统决定，`.preferredColorScheme(.dark)` 只管明暗轴；`Color(nsColor: .controlBackgroundColor)` 走语义色 | 风格轴不作为用户可写枚举时，观感差异由系统承担——本库无系统控件，必须自带标定表 |
| Compose Material 3 | `ColorScheme` 明暗两套 + `dynamicColorScheme` 从壁纸取色 | 「一套角色名、两列取值」与本库 `ColorRoles` 同构，验证现有结构无需改动 |
| WinUI / UWP | `ElementTheme` 折平为 `Light / Dark / HighDark` 三值 | **反面教材**：明暗折进枚举后，每出现第三种呈现就要补一个枚举值 |
| Qt 6 | `QStyleHints::colorScheme()` 管明暗，`QStyle` 按平台选（windows11 / fusion）；`QPalette` 双份 | 两轴分离 + 「风格 = 一套 palette + 一套 pixelMetric」的划分可直接借用 |
| libadwaita | `AdwStyleManager:color-scheme` 四态（含 `FORCED_*`），强调色 1.6+ 独立 | 明暗的「表态 / 强制 / 不表态」三态与 `ColorScheme::Unspecified` 同构，本库三态不可折平 |
| KDE Frameworks | `KColorScheme` 按 group 读配色，Breeze 提供 pixelMetric 组 | 度量组与配色组分层是 Breeze 自身的结构，支持本提案的 `ControlMetrics` 单列 |

## 变更内容

### 设计不变量（先定契约，再定接口）

1. **风格轴与明暗轴正交**。`ThemeStyle` 只表「用哪套设计语言」，其取值不含明暗位；一套风格内含明暗两列取值，与 `ColorRoles::light()` / `ColorRoles::dark()` 的「角色名一套、取值两列」同构。`Theme` 值对象继续持**一列**。
2. **明暗三态不得折平**。`ColorScheme::Unspecified` 是三级优先链的承重墙（显式 `set_theme` > OS 偏好 > 明色），「`Unspecified` ≡ 明色列」是全系统单一口径。把明暗折进风格枚举会让该态无处安放，并制造出「显式枚举名里的暗位」与「OS 报亮」相冲的裁决真空。
3. **`CsdPreset` 降为内部标定表**。`ThemeStyle × ColorScheme` 是对外唯一真相；`TitleBarStyle::from_preset` 的签名与行为保留（兼容既有调用面），但新增档只服务派生通道，不再充当对外风格入口。
4. **风格预设静态标定、零副作用**。`from_style()` 是纯函数（读的是编译期进制品的常量表）；运行期只允许探测「哪个风格」与「系统明暗」，**不允许**从系统抓取配色（强调色跟随属另一能力，另议提案，避免本提案混入四个平台的取色桥与无法 headless 验证的断言）。
5. **`Theme` 仍是值语义聚合、渲染路径零哈希零分配**。新增字段全为纯值结构体；探测只发生在 `defaults()` / `default_style()` 调用点，且结果进程内缓存一次，不进帧路径。
6. **默认收敛只改解算链，不改显式语义**。应用显式 `set_theme` 仍然整份原样注入（其 `scheme` 即最终档，行为逐字不变）。
7. **兜底三处必须同源**。主题的「无注入兜底」在仓库里有三处入口——`include/aurora/window/window.h` 的 `resolved_root_theme()`、`include/aurora/theming/theme_scope.h` 的 `inherit_theme(ctx)`（现兜 `Theme::light()`）、`include/aurora/theming/theme_query.h` 的 `resolve_theme(root, target)`（现兜 `Theme::with_defaults()`）。收敛后前者的默认档变成平台风格，后两者仍取 Material，若不裁决就会出现「挂在 `Window` 下得原生风、裸 `BuildContext` 或 `resolve_theme` 查询得 Material」的口径分裂，并直接破 `codespec/specification/07-environment-modifier.md` §5.2 的「单一口径」句子。**裁决**：兜底继续取 Material 基线，理由是这三处兜底都发生在「无宿主 / 无窗口 / 树内无 Provider」的语境，与风格探测的「无窗口后端即回落 Material」判据同源，语义自洽；`inherit_theme` 与 `resolve_theme` 的兜底**不改**，并在两处注释与 §5.2 各补一句「兜底档 ≡ Material 基线，平台风格只经根注入或显式主题进入」。
8. **确定性渲染不变量优先于观感跟随**。`codespec/ARCHITECTURE.md` §11 第 5 条钉的是「相同 widget 树 + 尺寸 → 相同像素输出」。本提案给风格档配的具体字体族名会把**本机安装的字体集合**引入解算路径：`src/aurora/render/font_discovery.cpp` 的口径是内建 face 只挂进 `""` 与 `sans-serif` 两个键、不产生族名，故现行默认档（`Font` 的 `family` 取 `sans-serif`）走的是确定性内建路径，而换成 `Cantarell` / `Segoe UI Variable Text` 一类具体族名即改走系统字体目录扫描结果，装有与未装有该字体的两台机器会量出不同文本度量。对策是把「测试与 golden 固定 Material 档」升为硬约束而非约定俗成：无头构建回落 `Material`（⇒ `sans-serif` 内建 face）、做绝对像素或布局断言的用例显式钉主题或风格档，风格档的真机差异只由手工用例目视核对，不进像素门禁。

### 风格枚举 `ThemeStyle`（计划/待建：include/aurora/theming/theme_style.h）

```
enum class ThemeStyle : std::uint8_t {
    Material,  ///< 现行基线（Material 3 角色与字阶），兼作无证据环境的回落档
    Windows,   ///< Windows 11 / Fluent 视觉语言
    MacOs,     ///< macOS / Aqua 视觉语言（拼写随 PlatformKind::MacOs）
    Gnome,     ///< GNOME / Adwaita（libadwaita）视觉语言
    Kde,       ///< KDE Plasma / Breeze 视觉语言
};
```

- 不设 `Unknown`：风格轴必须有值才能出主题，「探测无证据」的表达方式是**回落 `Material`**，而不是把一个不可用的枚举值塞进调用方的判空责任里。
- `MacOs` 拼法随 `PlatformKind::MacOs`（`SurfaceKind::MacOS` 是历史拼法，本头不引入第三种拼写，注释里写明取舍）。
- 同头新增 `enum class DesktopEnvironment : std::uint8_t { Unknown, Gnome, Kde, Other };`——它是风格解算的输入维度，不进 `PlatformKind`（那是平台家族，不是桌面外壳）。

### `Theme` 聚合面扩字段（`include/aurora/theming/theme.h`）

新增字段均带默认成员值，`Theme{}` 与既有指定成员初始化写法保持合法：

- `ThemeStyle style = ThemeStyle::Material;`——本实例所属风格（身份位，供 CSD 派生与 `describe` 面读取）。
- `ShapeScale shapes` / `SpacingScale spacing`——形状与间距档位令牌。**这两支从 CHG-056 中拆出、由本提案先行落地**（CHG-056 的属性表与写拦截部分留在原提案），初值逐位复现现行硬编码：`shapes.small = 6`、`spacing.large = 12`、`spacing.small = 6`，保证既有观感零变化。
- `ControlMetrics controls`——控件基准度量（新增）：`control_height` / `control_height_small` / `min_touch_target` / `menu_item_height` / `focus_ring_width` / `focus_ring_inset` / `indicator_size`（勾选/单选基准盒） / `scrollbar_thickness` / `separator_thickness` / `border_thickness` / `icon_default_size` / `content_padding`。单位一律逻辑 dp，受 `tools/check/check_dpi_single_source.py` 口径约束。
- `LayoutConventions conventions`——布局与交互惯例（新增，纯值组）：`dialog_button_order`（枚举：接受在前 / 接受在后 / 垂直右对齐）、`dialog_title_alignment`、`menu_bar_placement`（窗口内自绘 / 原生菜单栏优先）、`scrollbar_mode`（经典占位 / 覆盖式）、`focus_ring_style`（描边 / 外扩发光 / 无环）、`motion_scale`（动效时长倍率，只缩放既有曲线时长，不改动画引擎）。
- 既有 `roles` / `type_scale` / `csd` / `scheme` / `extensions` 语义不变；`TypeScale` 的 `Font::family` 由风格给族名（`Font` 已支持族名且后端缺失时回退平台默认族，故在无该字体的机器上安全降级）。

### 工厂与探测面（`include/aurora/theming/theme.h`）

- `[[nodiscard]] static auto from_style(ThemeStyle style, ColorScheme scheme = ColorScheme::Unspecified) -> Theme;`——纯函数，按 (风格, 档) 取整套令牌；`scheme == Unspecified` 时**填明色列且 `scheme` 字段保持 `Unspecified`**（不变量第二条的单一真相：不表态≠亮，只是本层没意见）。
- `[[nodiscard]] static auto default_style() -> ThemeStyle;`——运行期探测，声明 `Side-effects: reads process environment`（Linux 支读环境变量，其余支为编译期常量），首次调用读一次并缓存。
- `[[nodiscard]] static auto defaults() -> Theme;`——等价于 `from_style(default_style())`，**不替应用决定明暗档**：明暗是优先链解算结果而非探测结果，让 `defaults()` 在构造期直读注册表/配置文件会同时违反既有两条纪律（`codespec/specification/07-environment-modifier.md` §5.2「不在每帧直读、创建时读一次」与 `include/aurora/app/application.h` 的「构造期不隐式读 OS 偏好」）。需要一份「确定的暗色」时调用 `from_style(default_style(), os_color_scheme())`。
- `light()` / `dark()` / `with_defaults()` 保留，语义重述为 `from_style(Material, Light)` / `from_style(Material, Dark)` / `Theme{}`，**逐值不变**（由单测钉死，见验收判据第二条）。
- 命名分流出一个说明：`Theme::defaults()`（解算出的整套主题）与 CHG-056 规划的 `XxxProps::defaults()`（该控件属性快照）同名不同主，两者在 `codespec/specification/04-widget.md` 与 `codespec/CODING_STANDARDS.md` §11.5 各加一句区分，避免代码生成混用。

### 风格探测判据

| 环境证据 | `default_style()` 返回 |
| :--- | :--- |
| 构建内无任何窗口后端（`auto_detect_surface()` 为 `Headless`）或 WASM 目标 | `Material` |
| Windows 目标 | `Windows` |
| macOS 目标 | `MacOs` |
| Linux 目标且桌面环境证据为 KDE / Plasma | `Kde` |
| Linux 目标且桌面环境证据为 GNOME 及其它 GTK 外壳 | `Gnome` |
| Linux 目标且无任何桌面环境证据（裸 X11 / 自建合成器） | `Gnome`（Adwaita 基线，与既有 GTK 配置启发式同源） |
| `platform().kind == Unknown` 但有窗口后端（BSD / Android 等未映射进 `PlatformKind` 的目标） | 有 KDE / GNOME 外壳证据即取对应档，否则 `Material` |

- **判据表以 `platform()` 的取值为准，故 BSD 单独一行**：`include/aurora/core/platform.h` 有 `AURORA_PLATFORM_BSD` / `AURORA_PLATFORM_ANDROID` 宏，但 `platform()` 的 `kind` 只映射 Windows / macOS / Linux 三者，其余给 `PlatformKind::Unknown`。本提案**不**为风格解算扩 `PlatformKind` 取值（那是公共枚举扩值、且会牵动 `MediaQuery` 与既有断言），改用「外壳证据优先、无证据回落 `Material`」这一行覆盖；该行的存在也意味着 `Unknown` 不等于「无风格可用」，只是证据更少。

- 桌面环境证据口径：`XDG_CURRENT_DESKTOP`（冒号分隔，首个命中为准）→ `DESKTOP_SESSION` → 皆缺即 `Unknown`。`Kde` 认 `KDE` / `plasma`；`Gnome` 认 `GNOME` / `Unity` / `budgie` / `cinnamon` / `pantheon` / `deepin`；其余命中记 `Other` 并回落 `Gnome`。
- **实现体唯一**：解析环境变量的函数只在 `src/aurora/theming/detail/` 落一份（`detail::probe_desktop_environment()`）；`include/aurora/window/platform.h` 新增的公共 `desktop_environment()` 转发到它（依赖方向 window → theming 合法，且 `csd_style_bridge.h` 已确立 theming 依赖 window 层头的既有方向不可反向复用）。**不新增第二处 `getenv` 读取**，避免两条链各自漂移。
- 可测性：风格解算拆成纯函数 `detail::style_from(PlatformKind, DesktopEnvironment, bool has_window_backend) -> ThemeStyle`，判据表逐格单测；`default_style()` 本体只负责「取证据 + 转调」，不在 CI 上依赖真机外壳。
- **证据源必须是 `platform().kind`，不是 `MediaQuery::platform`**：同名 `PlatformKind` 在仓库里有两个来源——`MediaQuery::platform` 只在 Win32 后端填 `Windows`、其余恒 `Unknown`（`src/aurora/environment/media_query.cpp` 的现状，它表的是渲染后端而非 OS 家族），而 `platform()` 按编译期 `AURORA_PLATFORM_*` 宏给 `Windows` / `MacOs` / `Linux`。取错源即「Linux 桌面 + GLFW 后端 → 证据 `Unknown` → 解算退到回落档」，与判据表直接冲突。故 `default_style()` 读 `platform().kind` 与 `auto_detect_surface()`；同时给 `media_query.h` 那条注释补一句「表后端、不表 OS 家族」的口径注记，避免下一个消费者重踩。`PlatformKind` 类型定义取自 `include/aurora/environment/media_query.h`（theming → environment 的依赖方向已由 `theme_scope.h` 确立）。
- **缓存与测试的隔离纪律**：`default_style()` 的进程内缓存在语义上不可重入——若单测用 `setenv` 造外壳证据，首个用例设下的环境会粘住后续用例读到的缓存值，得到顺序依赖的不可复现结果。故纪律定为：单测只测纯函数，不得为测试改环境变量；`default_style()` 的真机读数与「证据只读一次」由手工用例核对。公共头注释按 `codespec/CODING_STANDARDS.md` §13 补 `@note Thread:`（静态局部量的首次初始化由语言保证线程安全，读取路径无锁，与单线程 UI 纪律不冲突）。
- `PlatformKind` 不足以区分 Linux 上的 GNOME 与 KDE（它表平台家族，不表桌面外壳），这正是 `DesktopEnvironment` 必须独立成枚举的原因；两条通道不合并、不互相替代。
- 无头回落的诚实边界：判据是「构建内后端可用性 + 运行期外壳证据」，**不是「是否真有显示器」**。因此无头 CI 的像素稳定性继续由既有约定承担（`codespec/specification/07-environment-modifier.md` §5.2 测试约定：做绝对色断言的用例必须显式钉主题），本提案落码时先全仓扫描并补齐未钉的绝对色断言用例。

### 根注入解算链收敛（`include/aurora/window/window.h`）

`resolved_root_theme()` 由「明暗二选一」改为两轴解算：显式 `theme_` 命中即整份原样返回（不变）；否则 `from_style(style_.value_or(Theme::default_style()), os_scheme_)`。

- 新增 `Window::set_theme_style(ThemeStyle)` 与 `theme_style()`——语义是**只定风格轴、明暗继续跟随 OS**，正是两轴正交模型在应用侧的表达（现有 `set_theme` 只能整份覆盖，表达不了「就要 Windows 那套，深浅随系统」）。
- `WindowHost` 推 OS 档的既有通路不变（`set_os_color_scheme`），运行期明暗切换仍然下一帧生效；**风格轴不随运行期环境变化**（外壳切换需重启应用，口径与 `os_color_scheme()` 的「创建时读一次」并列写进规格）。
- 行为变更面（唯一一条破坏性）：应用未显式设主题时，默认观感从 Material 基线变为平台原生档。迁移路径给足两条——`set_theme(Theme::light())` 取回逐位相同的旧观感，`set_theme_style(ThemeStyle::Material)` 保留平台跟随但锁风格。

### 五套风格的标定表（`src/aurora/theming/` 预设表）

每套风格给出六个维度：色角色两列、字阶族名与正文基准、形状档位、间距档位、控件度量、布局惯例。仓库内已有的真机标定值直接复用，未复用的先给候选值并就地标注入册：

| 维度 | Material（现行） | Windows | MacOs | Gnome | Kde |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 字体族 | `sans-serif` | Segoe UI Variable Text | SF Pro Text | Cantarell | Noto Sans |
| 正文基准 | 14pt（现值） | 12pt（候选） | 13pt（候选） | 11pt（候选） | 10pt（候选） |
| 控件基准高 | 现值不动 | 32dp（候选） | 28dp（候选） | 34dp（候选） | 30dp（候选） |
| 主控件圆角 | 6dp（现值，`shapes.small`） | 4dp（候选） | 7dp（候选） | 12dp（候选） | 3dp（候选） |
| 焦点环 | 描边、`focus_ring` 极性色 | 描边 + 内层对比环 | 外扩发光 | 描边双环 | 内嵌虚线 |
| 对话框按钮序 | 接受在后 | 接受在前（水平） | 接受在后（垂直右对齐） | 接受在后（`suggested-action` 填充） | 接受在后（水平） |
| 滚动条 | 经典占位 | 细经典（自动收窄） | 覆盖式 | 覆盖式 | 经典占位 |
| CSD 基座 | `CsdPreset::Adwaita*`（现状不变） | `CsdPreset::Windows*`（已有真机标定） | `CsdPreset::Mac*`（已有真机标定） | `CsdPreset::Adwaita*`（已有真机标定） | 新增 `CsdPreset::Breeze*` 两档 |
| 配色取值 | `ColorRoles::light()/dark()` 现值 | Fluent 中性面 + 系统蓝系强调 | Aqua 表面族 + systemBlue | Adwaita 调色板（`blue3` 系强调） | Breeze 中性面 + `#3daee9` 强调 |

- **取值口径分级**：凡本表标「已有真机标定」的档位直接复用 `include/aurora/window/title_bar_style.h` 里注释已注明出处的数值；标「候选」的数值**不得在无出处的情况下进公共头**——落码批要求逐值注明来源（官方 HIG / 上游 CSS 变量表 / 真机取色），并在代码注释里随值写死出处，口径与 `title_bar_style.h` 现有注释块一致。
- 每套风格的 `on_*` 角色与配对角色继续满足 WCAG AA ≥ 4.5:1（现有 `utest_theme_roles` 的断言面从 2 档扩到 10 档，见验收判据第六条）。
- `Kde` 的标题栏按钮布局先复用 `TitleBarButtonLayout::Adwaita` + Breeze 度量钉值；若标定批实测字形差异显著，再单列一枚布局值（单列即新增枚举值，属公共 API，须在该批一并回写）。

### CSD 派生通道按风格映射（`include/aurora/theming/csd_style_bridge.h`）

`title_bar_style_from_theme(theme)` 的基座选取由「恒 Adwaita」改为 `(theme.style, theme.scheme)` 映射 `CsdPreset`；配色字段仍从 `theme.csd` 逐字段派生（覆盖面不变）。`theme.style == Material` 时映射结果与现状逐值相同，故既有派生通道测试零 diff。`window_frame_style_from_theme` 同批改。

- **扩档的连带义务**：`CsdPreset` 增 `BreezeLight` / `BreezeDark` 两档后，`TitleBarStyle::from_preset` 的 `switch` 无 `default` 分支（现状逐档穷举），新增值即触发 `-Wswitch` 与漏档风险，须同批补 case 并让 `utest_theme_csd_bridge` 覆盖全部档数；`check_api_schema_sync` 与 `check_naming_conventions` 亦按新增公共枚举值复检。另有两处静默漏点必须同批改：`examples/demos/demo_csd_gallery.cpp` 的 `theme_preset()` 是 `idx % 6` + `default` 返回（扩档后新两档**永不出现**且无编译告警，目视核对通道直接失效），以及 `codespec/specification/07-environment-modifier.md` §4.2 的「三套视觉语言 × 明暗两档 = 六值」计数句与「度量口径三档分治」表（须改四套 × 两档 = 八值并补一行 Breeze 出处）。
- **既有契约保位**：`TitleBarStyle{}` 的默认成员值 ≡ `from_preset(CsdPreset::AdwaitaDark)` 这条契约（`codespec/specification/07-environment-modifier.md` §4.2 钉死）**不随本提案改变**——契约的承担者是 `TitleBarStyle` 的默认构造，不是根注入的风格档；`Theme::from_style(Gnome, Dark)` 的 `csd` 组继续与该档逐值相同，构成第二条逐值不变量。
- **风格对原生装饰无效，须如实声明**：Windows / macOS 走服务端装饰（非客户区由 OS 绘制，`include/aurora/window/surface.h` 的 `DecorationPolicy::ServerSide` 与 `include/aurora/window/window.h` 的 `content_inset()` 那条路径）时，`theme.csd` 与风格对应的标题栏档不参与绘制，风格差异只体现在自绘 CSD 兜底路径与内容区观感上。此条写进规格，避免「选了 Windows 风格却改不动 Windows 原生标题栏」被当成缺陷上报。
- **与 `DecorationPolicy` 的交叉**：GNOME 无 `xdg-decoration` 协议时框架兜底 CSD，此时风格解算给出的 `Gnome` 档标题栏正是真机观感；`Kde` 档下服务端装饰可用则标题栏由 KDE 绘制。两者的选择逻辑本次不改，只在规格里补一句「风格档不覆盖 `DecorationPolicy` 的决策」。

### 范围边界（评审会问的三件事，先答在前面）

- **不做风格持久化**：`include/aurora/preferences/preferences.h` 的分组键机制（其文档注释的示例即 `ui.theme`）是应用侧通道。框架只负责探测与解算，「用户手选风格 vs 跟随系统」谁赢属应用策略，落法是应用读自己的偏好后调 `Window::set_theme_style` 或 `set_theme`；框架若自带持久化会与该模块职责重叠。
- **不接系统无障碍开关**：高对比度、减少动效、强调色跟随都不在本提案（`motion_scale` 只是风格档里的静态倍率，不读系统开关）。`include/aurora/widget/a11y_tree.h` 与 Inspector 面对新令牌的暴露另议。
- **不新增错误码与能力位**：`codespec/errors.toml` 与 `include/aurora/window/platform.h` 的 `PlatformCapabilities` 均不扩——探测无失败态（无证据即回落 `Material`），风格解算不引入新的运行期失败路径，符合「能力位只表机制存在性」的既有口径。

### 序列化与 AI 面

- `Theme` 的 `to_json` / `from_json` 增 `style` / `shapes` / `spacing` / `controls` / `conventions` 键（键名即字段名，stringly 仍只存在于序列化边界）。
- **枚举的 AI 面不是自动的**：`aurora_api.json` 的 `enums` 表由手工维护的 `tools/include/known_enums.h` 单源提供（该头同时被 gen_api / aurora_mcp / aurora_cli / aurora_lsp 共享，取值须与真实枚举成员逐字一致，由 `tests/integration/itest_known_enums.cpp` 的编译期锚点把关）。故新增 `ThemeStyle` 与 `DesktopEnvironment` 必须同批登记进该头并补锚点，否则 AI 侧看不到风格轴的取值面——这是两轴正交模型在代码生成面上唯一的失效点，必须钉住。
- 口径校正（实测所得）：`aurora_api.json` 里**不含** `Theme` 的令牌面（`ColorRoles` / `TypeScale` / `CsdRoles` 在 schema 中零命中，仅 `ThemeProvider` 一名出现），因此本提案新增四个令牌结构体不改变 schema 规模；API 预算的增长来源只有 known_enums 登记的两枚枚举与 `Window` 新增的两枚成员描述。实测余量：当前 `aurora_api.json` 206103 字节 ≈ 51.5k token（按 `tools/check/check_api_budget.py` 的 4 字符/token 口径），预算上限 75000 token。
- **同批补登 `ColorScheme`（已定）**：明暗轴当前**不在** known_enums 登记表里，AI 侧只能枚举半根轴，与不变量第一条「两轴对等正交」不符。本批把三枚枚举一并登记（`ThemeStyle` / `DesktopEnvironment` / `ColorScheme`），纯登记、无行为变化，代价是 `itest_known_enums` 需补三组编译期锚点。
- 控件侧 `default_expr` 出现主题令牌名属 CHG-056 的属性表面，本提案不触碰其生成规则。
- 安装面无需改动：`cmake/AuroraInstall.cmake` 以 `install(DIRECTORY include/aurora ...)` 整目录装机，新公共头自动随制品分发。

## 验收判据

1. **两轴正交的可执行钉法**：`ThemeStyle` 枚举成员数恒为 5，且无任何成员名含明暗字样（`utest_theme_style` 以枚举名表断言）；同一风格的明暗两档由 `from_style(s, Light)` / `from_style(s, Dark)` 给出，二者字段名集合逐字段同构（同一 `ColorRoles` 类型，非两套结构）。
2. **零回归基线**：`Theme::light()` ≡ `from_style(Material, Light)`、`Theme::dark()` ≡ `from_style(Material, Dark)`、`with_defaults()` ≡ `Theme{}` ≡ `from_style(Material, Unspecified)` 四者逐位相等；`HeadlessSurface` 下 `tests/golden/` 全部 15 张基线零 diff（无需重标定）；`title_bar_style_from_theme(Theme::light())` ≡ `from_preset(CsdPreset::AdwaitaLight)` 的既有逐值不变量继续成立。
3. **探测矩阵全覆盖**：`detail::style_from(PlatformKind, DesktopEnvironment, has_window_backend)` 按判据表逐格断言（至少 Windows×无后端、macOS、Linux×Kde、Linux×Unknown 外壳、`Unknown`×有后端×Kde、`Unknown`×有后端×无证据、WASM、headless 八格），断言不读真机环境、不改环境变量（缓存不可重入，见变更内容里的隔离纪律）；`default_style()` 与纯函数在同一格上取值一致（真机证据只经手工用例核对）。
4. **根注入收敛**：`itest_theme_root_injection` 扩展三格——无显式主题时注入主题的 `style == Theme::default_style()` 且 `scheme` 由 OS 档解算；显式 `set_theme` 时整份原样（含其 `style` 与 `scheme`）；`set_theme_style(Windows)` + 打桩 OS 暗档 → 注入 `Windows × Dark`。打桩沿用既有 seam，不依赖 runner 真机档位。
5. **三态不塌**：`from_style(s, Unspecified)` 的 `roles` 逐字段等于该风格明色列、`scheme` 仍为 `Unspecified`；`resolve_color_scheme` 与 CSD 桥的「`Unspecified` ≡ 明色列」口径在十档组合上逐一致（`utest_theme_csd_bridge` 扩档）。
6. **对比度**：五风格 × 明暗两档共 10 组色角色，每对 `on_*` 与配对角色 WCAG AA ≥ 4.5:1 由 `utest_theme_roles` 断言；不达标者在标定批改取值，若必须保留则就地写豁免理由。
7. **惯例真实生效**：`Dialog` 的按钮排布在四套平台风格下各断言一次（顺序 + 对齐 + 命中盒），`MenuBar` 的 `menu_bar_placement` 与 `PlatformCapabilities::native_menu_bar` 既有口径不冲突；控件显式覆写值优先于惯例（两态语义不变）。
8. **令牌消费接线**：首批至少 `Button` / `Checkbox` / `Divider` / `Scroll` / `Dialog` 的默认形状、间距与度量改读 `theme.shapes` / `theme.spacing` / `theme.controls`；未显式设值的实例在切风格时随令牌变化，显式设值实例不变；`Button` 在 Material 档下的圆角与内边距实测仍为 6 与 12/6。
9. **零开销承诺**：`defaults()` / `default_style()` 每进程至多一次证据读取（缓存），渲染路径读令牌为直接字段访问，无哈希、无新增堆分配（既有 `Theme` 的两态取值链不变）。
10. **门禁矩阵全绿**：`check_api_schema_sync`（先 `aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_platform_macros`（新 TU 只用 `AURORA_*` 宏）、`check_core_layer_boundary`、`check_arch_module_map`（该门禁只校 `codespec/ARCHITECTURE.md` §4 模块映射行内反引号文件是否真实存在，故未落地的新头**不得提前写入 §4**，须在头文件落地的同批登记）、`check_umbrella_header`（新公共头同批登记进 tools/check/umbrella_manifest.txt 的 `[direct]` 清单并进 `aurora/aurora.h`）、`check_no_cjk_literals`（字体族名等字面量全 ASCII）、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`lint`、`format-check` 与 `ctest --preset ninja-test` 全量通过；`toggles` CI 矩阵覆盖「无任何窗口后端」变体以验证无头回落。
11. **落码批次划分**：令牌批（四组令牌 + `Theme` 扩字段，零观感变化）→ 标定批（五套预设值 + `CsdPreset` 扩档 + CSD 映射）→ 探测批（`DesktopEnvironment` + `default_style()` + 纯函数矩阵）→ 收敛批（根注入 + `set_theme_style` + 未钉主题的绝对色断言用例补齐）→ 接线批（首批控件消费）→ 回写批（文档与 CHANGELOG 收口）。每批独立可验证、独立可回滚，收敛批之前既有观感零变化；提案状态随批次从「已提议」推进到「实施中」，回写批完成后归档。
12. **兜底三点同源**：构造三类语境各断言一次——挂 `Window` 且未显式设主题（得 `default_style()` 档）、树内无 `ThemeProvider` 的 `resolve_theme(root, target)`（得 `with_defaults()` 即 Material 基线）、裸 `BuildContext` 的 `inherit_theme(ctx)`（得 `Theme::light()`）。后两者在收敛后**必须保持 Material 不变**，断言即钉住不变量第七条。
13. **AI 面枚举登记到位**：`ThemeStyle`、`DesktopEnvironment` 与 `ColorScheme` 三枚枚举同批出现在 `aurora_api.json` 的 `enums` 表（登记进 tools/include/known_enums.h 并补 `itest_known_enums` 的编译期锚点），既有的 `known_enums_historical_wrong_values_do_not_return` 用例继续通过；风格轴与明暗轴在 schema 里同时可枚举，正交模型不因半轴缺位而在代码生成面退化。
14. **字体确定性核对**：同一份声明式树在「装有该风格首选字体」与「未装」的两台机器上分别取文本度量，记录差异并证明该差异只出现在非 Material 档；Material 档（`sans-serif` 内建 face）在两台机器上度量逐位一致，`tests/golden/` 基线不因本提案改判据。此条守 `codespec/ARCHITECTURE.md` §11 第 5 条不变量。
15. **风格档不参与热路径分配**：`from_style()` 构造的 `TypeScale` 含 15 枚 `Font`（各持一个族名字符串），属**调用一次**的构造成本；断言帧路径内无 `from_style` / `defaults` 调用（以既有性能门禁与调用计数为准），并记录 `sizeof(Theme)` 的增长量供评审判断值拷贝链（`Provider` 转发的 `std::function` 深拷贝）影响是否可接受。

## 风险与未决项

1. **观感「像不像」无客观判据**。对策：以官方 HIG / 上游 CSS 变量表为唯一取值出处，逐值注源；同时提供一个 demo 页把五套风格 × 明暗两档并排呈现（examples/demos/ 下新增源文件，按 `AGENTS.md` 硬规则第七条须配套 `tests/unit/` 单测，或按 `codespec/CODING_STANDARDS.md` §3.1 的一次性示例口径显式标「无单测」），由人工目视核对入手工用例，不做像素近似断言。
2. **字号与控件基准高变更对既有布局测试的连锁影响**。候选值（如 Windows 12pt / 控件高 32dp）与现行 14pt 基线差距会牵动文本测量与布局断言。对策：接线批之前先在全部 demo 与布局用例上跑一遍实测，把「候选」升级为「实测」或回退到不改基准高的保守档；本提案不预设未实测的门槛。
3. **首选字体缺失即观感退化**。风格档写死 `Cantarell` 一类族名后，未装该字体的机器落到后端回退族，用户看到的是「不是该系统的默认字形」而非报错。可接受，但须在规格里如实写明「族名是偏好而非保证」，并保证回退路径不产生空字形或度量塌陷（`include/aurora/core/font.h` 的既有回退语义）。
4. **系统强调色 / 减少动效 / 高对比度等无障碍跟随不在本提案范围**。它们是「读取系统设置」的能力，需四个平台的桥与真机验证，另立提案；`motion_scale` 本批只取风格档里的静态倍率。a11y 面（include/aurora/widget/a11y_tree.h）与 Inspector 对新令牌的暴露同样另议。
5. **`Theme::defaults()` 与 `XxxProps::defaults()` 同名不同主**（前者出整套主题、后者出控件属性快照）。若评审倾向消歧，备选名 `Theme::platform_default()`；本提案默认保留 `defaults` 口径，靠 `codespec/specification/04-widget.md` 与 `codespec/CODING_STANDARDS.md` §11.5 各一句区分。
6. **与 CHG-056 的交界**。形状/间距令牌归属被本提案前移，CHG-056 落码时须复用而非重定义（其提案文档需同步一处依赖说明）；属性表与写拦截部分不受影响。两提案若同批落码，令牌结构的定义权归本提案、消费面归 CHG-056。
7. **风格跟随与用户手选的优先级由应用裁定**。框架提供两条通道（`defaults()` 探测、`set_theme_style` 显式）但不替应用做「用户偏好 vs 系统探测」的裁决；这条留白须写进规格里，避免后来者误加隐式偏好读取。
8. **对码事实校正项已定案**：`aurora_api.json` 的枚举面来自手工登记表而非自动反射（见「序列化与 AI 面」第二条），`ColorScheme` 的缺位随本批一并补齐；需求侧沿用 SPEC.API.SENSIBLE-DEFAULTS.001，不另立特性条目。

## 回写落点

- `codespec/specification/07-environment-modifier.md`：§5.1 增 `ThemeStyle` / `ShapeScale` / `SpacingScale` / `ControlMetrics` / `LayoutConventions` 字段与五套预设取值口径；§5.2 改写根注入解算链（两轴正交 + 无头回落判据 + 风格轴不随运行期变化 + 兜底三点同源）并补 `desktop_environment()` 转发与探测实现体唯一口径；§5.3 改 CSD 基座映射表；§5.4 补序列化新键；**§4.2 同步改档数**——现文钉「三套视觉语言 × 明暗两档 = 六值」与「度量口径三档分治」表，`CsdPreset` 扩 `Breeze*` 后须改为四套 × 两档 = 八值并为 Breeze 补一行度量取法与出处（该处计数与表行由 `check_code_doc_sync` 对码复核，漏改即文档-代码漂移）。
- `codespec/specification/06-app-platform.md`：§5 平台探测段增桌面环境证据口径（含 `PlatformKind` 未覆盖 BSD / Android 时的 `Unknown` 判据行）、探测实现体唯一性约束、`platform().kind`（表 OS 家族）与 `MediaQuery::platform`（表渲染后端）的取值分野，以及风格档对原生装饰无效的边界说明。
- `codespec/specification/04-widget.md`：首批控件默认值由字面量改为主题令牌表达式的落点清单，以及 `Theme::defaults()` 与 `XxxProps::defaults()` 的命名分流。
- `codespec/specification/03-layout-render.md`：字体族名偏好与回退口径对文本度量的影响（守 §10.1 的确定性渲染需求 SPEC.TEST.RENDER.DETERMINISTIC-SNAPSHOT.001）。
- `codespec/ARCHITECTURE.md`：§11 设计不变量段增「风格轴与明暗轴正交、明暗三态不可折平」「兜底三点同源」「确定性渲染优先于观感跟随」；§4 模块映射的主题行随新头/新目录同批登记（该行的反引号文件必须真实存在）；§14.6 门禁矩阵若新增校验点一并登记。
- `codespec/CODING_STANDARDS.md`：§11.5 合理默认值口径扩到「默认值 = 平台风格解算出的令牌」；§14 登记字体族名字符串字面量的 ASCII 口径；§3.1 补新增 demo 的配套单测要求。
- `codespec/SPECIFICATIONS.md`：SPEC.API.SENSIBLE-DEFAULTS.001 行的落地说明扩到「默认值 = 平台风格解算出的整套令牌」；**不新增特性条目**（评审已定沿用现有需求 ID），§5 模块域→文件映射同步 theming 新头。
- `codespec/BUILD_OPTIONS.md`：本提案不新增 `AURORA_*` 开关，如实登记「无新增开关/缓存变量」并说明探测无开关门控（若收敛批改为需门控则回写此条）。
- `codespec/manual-test/13-theming.md`：新增系统风格手工用例——五套风格 × 明暗两档并排目视核对、Linux 双外壳（GNOME / KDE）真机切换核对、无头回落核对、字体缺失回退核对，含用例编号与预期观感。
- 非 codespec 的同批登记面（不进本门禁反查）：tools/include/known_enums.h（两枚新枚举 + 补登既缺位的 ColorScheme）、tests/integration/itest_known_enums.cpp 的编译期锚点、tools/check/umbrella_manifest.txt 的 `[direct]` 新头清单、根级 CHANGELOG.md 的行为变更与两条迁移路径。
