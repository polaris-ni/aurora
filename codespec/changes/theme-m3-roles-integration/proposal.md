# CHG-011 主题体系接入：Material 3 角色化重构 + 默认根注入 + 控件分批迁移

| 字段   | 值                                                                                                                                                                                                                                                                                                                                                          |
| :--- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 变更编号 | CHG-011                                                                                                                                                                                                                                                                                                                                                    |
| 提出日期 | 2026-10-13                                                                                                                                                                                                                                                                                                                                                 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.API.STRONG-TYPES.001                                                                                                                                                                                                                                                                                                                                  |
| 影响面  | `include/aurora/theming/` 六个公共头的重构与部分废除（`Theme` 扁平字段与命名令牌表、`StyleProps`/`TokenOr`、`csd.*` 令牌层）、`include/aurora/window/window.h` 根环境注入、`include/aurora/theming/csd_style_bridge.h` 派生通道实现、约 60 个控件的取色口径、`aurora_api.json`、`codespec/specification/07-environment-modifier.md` §5、`codespec/specification/04-widget.md`、`codespec/manual-test/13-theming.md` |

## 动机

主题体系的能力面已完整（令牌表、明暗双档、作用域、派生通道、OS 探测），但运行时接入只到一层，且现存结构有两个必须靠重构才能消除的问题：

1. **四条链路未接通生产**：根上下文不注入 `Theme`（无 `ThemeScope` 时静默回落 `Theme::light()`）；`csd_style_bridge.h` 的两个派生函数只有测试调用方；`os_color_scheme()` 无任何调用方，「跟随系统深浅色」无承载；`StyleProps`/`TokenOr` 无控件使用。
2. **双入口漂移风险**：扁平字段（`background` 等）与命名令牌表（`tokens`/`dark_tokens`）是两套并存的读写入口，没有任何机制保证二者一致；已播种的语义令牌（`surface`/`hover`/`pressed`/`accent`/`border`/`danger`）目前零消费方——登记了但没人读。
3. **stringly 令牌的三个结构性缺陷**：
   - **静默回退**：令牌名拼错不报错，静默走 fallback（`focus.ring` 拼错即回落 `primary`），此类缺陷不可观测；
   - **热路径堆分配**：`token()` 每次查找构造 `std::string(name)` 再哈希——超过小串优化长度的令牌名（`csd.title_bar.bg.active` 等全部）每次查找都堆分配，落在渲染热路径上与渲染性能专项直接冲突；
   - **无编译期检查**：令牌改名不破坏编译，只破坏观感。

本提案以「对标主流框架 + 允许破坏性修改」为前提，一次性解决上述三点。

## 对标分析

| 框架                            | 主题模型                                                                     | 扩展机制                                       | 明暗机制                                  | 对 Aurora 的启示                                                 |
| :---------------------------- | :----------------------------------------------------------------------- | :----------------------------------------- | :------------------------------------ | :----------------------------------------------------------- |
| Material 3（Compose / Android） | `ColorScheme` 角色集（约 30 个色角色）+ 排版 15 档                                    | 动态取色生成整套角色                                 | 明暗两列、角色名一套                            | 「一套名字、两列取值」正是现有 `dark_tokens` 设计的类型化升级方向                     |
| Flutter                       | `ThemeData`（强类型 `ColorScheme` 字段 + `textTheme` + `ThemeExtension<T>` 列表） | `ThemeExtension<T>`：类型安全、`copyWith`/`lerp` | `ThemeData.light/dark` + `Brightness` | **强类型角色 + 类型安全扩展是最优组合**；stringly token map 只属于设计工具导出层，不属于运行时 |
| Fluent 2                      | alias tokens（ramp 派生）                                                    | 设计令牌 JSON + 平台适配                           | light/dark 两套 alias                   | 纯软件渲染无 ramp 生成需求，取别名表即可，不必引入派生链                              |
| SwiftUI                       | `@Environment` 值传递 + 自定义 `EnvironmentKey`                                | `EnvironmentKey`（类型键）                      | `colorScheme` 环境值                     | 环境注入即值传递，与 `Provider`/`inherit_theme` 机制同构，验证现有注入机制无需改动      |
| Qt                            | `QPalette`（`ColorRole` 枚举 → 颜色）                                          | `QStyle` / `QProxyStyle`                   | palette 双份                            | 枚举角色穷举困难、应用无法扩展，是反面教材                                        |
| CSS                           | custom properties + 层叠                                                   | 任意自定义属性                                    | `prefers-color-scheme`                | 完全 stringly：命名污染、静默回退、无类型检查——正是要规避的前车之鉴                      |

**结论**：主体取 Flutter 的「强类型角色 + 类型安全扩展」，角色命名与两列取值取 Material 3 的公共子集，CSS 式 stringly 只允许存在于序列化边界（AI/codegen 面），不允许出现在渲染路径。

## 变更内容

### 类型化色角色层 `ColorRoles`（计划/待建：include/aurora/theming/color_roles.h）

一个纯值 struct，字段即 Material 3 色角色全集（snake_case）：

- 主色四组：`primary` / `on_primary` / `primary_container` / `on_primary_container`，`secondary` / `on_secondary` / `secondary_container` / `on_secondary_container`，`tertiary` / `on_tertiary` / `tertiary_container` / `on_tertiary_container`，`error` / `on_error` / `error_container` / `on_error_container`
- 表面族：`surface` / `surface_dim` / `surface_bright` / `surface_container_lowest` / `surface_container_low` / `surface_container` / `surface_container_high` / `surface_container_highest` / `on_surface` / `on_surface_variant` / `surface_variant`
- 描边族：`outline` / `outline_variant`
- 反转族：`inverse_surface` / `inverse_on_surface`
- 其他：`scrim` / `shadow`
- Aurora 扩展角色（Material 3 无对应、本库显式需要，逐字段注明理由）：`focus_ring`（焦点环取极性色，与主色拉开，light 纯黑 / dark 纯白，承接现 `focus.ring` 令牌）、`hover` / `pressed`（交互态叠色，Material 3 由组件按 alpha 现算，本库既有控件已消费显式值，固化为角色）

默认取值口径：**角色值即预设值的唯一来源**——`ColorRoles::light()` / `ColorRoles::dark()` 的基准值取现行 `Theme::light()`/`dark()` 与已播种令牌的数值（蓝主色、白面、深色底 rgb 32,33,36 等），保证 18 个现有消费控件机械迁移后观感逐位不变。

每个 `on_*` 角色与其配对的容器角色须满足 WCAG AA 对比度 ≥ 4.5:1，由单测断言（见验收判据 1）；不满足的角色在立骨批即修正取值。

### 排版角色层 `TypeScale`（计划/待建：include/aurora/theming/type_scale.h）

Material 3 排版 15 档（`display` / `headline` / `title` / `body` / `label` 各 large/medium/small）为 `Font` 字段。现 `Theme::font` 的语义由 `type_scale.body_medium` 承接（图表族、`command_palette` 等读 `theme.font` 的调用点机械迁移至此）。`TypeScale` 结构在立骨批定义，控件侧迁移随文字族批次落地。

### 类型安全扩展 `ThemeExtension<T>`（计划/待建：include/aurora/theming/theme_extension.h）

对标 Flutter `ThemeExtension`：应用自定义令牌面不经字符串表，而以类型为键注册：

- 概念基类 + 模板模型（克隆 = 值语义深拷贝，与 `Theme` 随控件回调网被值拷贝的现状一致）；
- `Theme::extension<T>()` 返回 `const T*`，未注册返回 `nullptr`（调用方显式判空，不存在字符串拼写 silently 回落）；
- 典型用途：图表色板（现 `chart.palette.<i>` 字符串令牌的替代——注册一个 `ChartPalette` 扩展即可，不再新增 8 个角色字段）。

扩展不参与 JSON 自动序列化（与 Flutter `ThemeExtension` 需自行 `toJson` 同构）；`Theme` 的序列化边界只覆盖库内角色全集，角色名即 snake_case 字段名。

### `Theme` 重构（`include/aurora/theming/theme.h`）

```cpp
struct Theme {
    ColorRoles roles;            ///< 色角色全集（明暗两列由本实例持一列，见 scheme）
    TypeScale type_scale;        ///< 排版 15 档
    ColorScheme scheme = ColorScheme::Unspecified;  ///< 明暗归属档（三级优先链中间层，语义不变）
    // ThemeExtension<T> 注册表（类型键，值语义）
};
```

- `light()` / `dark()` 工厂签名不变，实现改为播种角色与字阶；
- `with_defaults()` 保留，语义改为「light 基准角色 + 无扩展」（裸主题不再是无色可用的空壳）；
- **废除面**：扁平字段 `background` / `primary` / `on_primary` / `text` / `font`；`TokenValue`；`tokens` / `dark_tokens` 及全部 `token*` / `set_token` 成员函数；`focus.ring` 与 `csd.*` 字符串令牌层。`csd.*` 的替代见 §5；
- `ThemeScope` / `inherit_theme` / `resolve_theme` / `resolve_color_scheme` / `os_color_scheme` / `Platform::color_scheme` 签名与语义全部保留。

### 为何不保留扁平字段、也不保留字符串令牌表（对「不考虑成本的最优解」的回答）

- **保留扁平字段作快捷位**：两套入口并存需要额外门禁长期防漂移，且新消费方会继续分流到两套 API，角色化名存实亡；
- **纯字符串令牌表**（现方向的延伸）：stringly 三缺陷全保留，见动机第 3 条；
- **强类型角色 + 类型安全扩展**：角色字段有编译期检查与补全，扩展有类型键，无静默回退；渲染路径零哈希查找零堆分配（直接服务渲染性能专项）；`Theme` 保持可值拷贝（角色 struct + 扩展深拷贝），不破坏现有注入与序列化链路。

### 默认根主题注入与 OS 跟随（`include/aurora/window/window.h`）

1. `Window` 新增主题状态与 `set_theme(const Theme&)`（链式/直接），应用显式设置即最高优先；
2. `prepare_context` 每帧向根 `BuildContext` 注入 `Theme`（与 `MediaQuery` / `WindowState` / `WindowMode` / `WindowChrome` 同址注入，机制完全对称）；
3. 应用未显式设置时，默认值按三级优先链解析：显式 `set_theme` > `os_color_scheme()` > `CsdPreset::AdwaitaDark` 对应档位；
4. OS 偏好读取口径：注册表/配置文件访问有副作用，**不在每帧直读**——窗口创建时读一次缓存；Win32 在 `WM_SETTINGCHANGE` 到达时重读（该消息已在宿主消息环内，只需挂探测点）；Linux 无系统信号，文档化为「窗口激活时重读 + 应用可随时 `set_theme` 显式接管」；macOS 按 `os_color_scheme()` 既有契约（未验证面不变）；
5. 换肤路径不变：`ThemeScope{std::make_shared<State<Theme>>(...)}` 值变化即子树重渲染（现有机制，本次补默认注入后开箱可用）。

### CSD 派生通道（typed 版契约）

`title_bar_style_from_theme(theme)` / `window_frame_style_from_theme(theme, base)` 签名保留，实现从「字符串令牌覆写」改为「角色派生」：

- 基座仍按 `theme.scheme` 取 `CsdPreset::AdwaitaLight` / `AdwaitaDark`（几何、显隐、度量字段不参与派生，属定制面——维持 CHG-004 口径）；
- 配色字段从角色派生：`bg_active` ← `surface`、`fg_active` ← `on_surface`、`bg_inactive` ← `surface_dim`、`fg_inactive` ← `on_surface_variant`、`close_hover` ← `error`、投影色 ← `shadow`；`hover` / `pressed` ← 交互态角色；
- **不变量**：`title_bar_style_from_theme(Theme::light())` 与 `from_preset(CsdPreset::AdwaitaLight)`、`title_bar_style_from_theme(Theme::dark())` 与 `from_preset(CsdPreset::AdwaitaDark)` 的配色字段逐值相同——由「角色基准值取预设 pinned 值」保证，单测钉死（延续 CHG-010 契约，类型化版）；
- 应用要逐字段定制：直接改派生结果的 `TitleBarStyle` 字段（或直接 `set_title_bar_style`），不再经 `csd.*` 字符串令牌；
- CHG-008 / CHG-009 钉住的原生机度与字形口径**全部不动**（本提案只改配色的取值来源）。

### 控件取色两态与角色映射

控件颜色属性的解析语义统一为两态（显式 > 主题角色），不再有第三态「内置硬编码默认参与运行时解析」：

1. **显式属性值**：用户调了 `set_xxx_color(...)` 或属性 JSON 给了值——优先；
2. **主题角色**：未显式设置时读 `inherit_theme(ctx).roles.<role>`（或对应字阶），角色必有值，不存在回落分支；
3. `*Props` 聚合里的颜色成员默认值从「硬编码色」改为「不设置即 std::nullopt」（`std::optional<Color>` 已是多数控件的现状模式，`ButtonProps::color` 等硬编码默认的改为 nullopt + 角色回退）；
4. **控件不引入 stringly 两态类型**（`TokenOr` 随之废除）；`describe_static()` 的 `default_value` 元数据保留 `"theme.surface"` 式字符串——它是给 AI/codegen 面的文档值，不参与运行时；
5. 每个控件的「颜色属性 → 角色」映射表随迁移批增量回写 `codespec/specification/04-widget.md`（该表同时是 codegen 描述符 default_value 字符串的唯一事实来源，两处由 `check_code_doc_sync` 类门禁对齐）。

### 破坏性变更清单与迁移路径

| 现存面                                                              | 处置                                  | 迁移路径                                                                                                   |
| :--------------------------------------------------------------- | :---------------------------------- | :----------------------------------------------------------------------------------------------------- |
| `Theme::background` / `primary` / `on_primary` / `text` / `font` | 废除                                  | `roles.surface` / `roles.primary` / `roles.on_primary` / `roles.on_surface` / `type_scale.body_medium` |
| `Theme::tokens` / `dark_tokens` 及全部 `token*` / `set_token`       | 废除                                  | 角色字段；应用自定义面注册 `ThemeExtension<T>`                                                                      |
| `TokenValue`                                                     | 废除                                  | 无替代（三态值容器随令牌表一同退场）                                                                                     |
| `focus.ring` 字符串令牌                                               | 废除                                  | `roles.focus_ring`                                                                                     |
| `csd.*` 字符串令牌层                                                   | 废除                                  | typed 派生（§5）+ 直接改 `TitleBarStyle`                                                                      |
| `StyleProps` / `TokenOr<T>` / `ResolvedStyle`                    | 废除（无生产消费方，仅 `utest_style_props` 覆盖） | 声明式场景直接写具体值                                                                                            |
| `*Props` 硬编码颜色默认（`ButtonProps::color = Color::blue()` 等）         | 改 nullopt + 角色回退                    | 观感变化即主题化目的，逐批 golden 重刷                                                                                |
| 默认观感（无 `ThemeScope` 的应用）                                         | 破坏性变化：随 OS 档位                       | 应用显式包 `ThemeScope` 或 `Window::set_theme` 固定；demo/测试全部显式固定                                              |
| `ThemeProvider` 序列化                                              | 无变化（值本就不参与序列化，`reg_no_props` 口径不变）  | —                                                                                                      |

`aurora_api.json` 须在本提案实施后刷新收录角色集与扩展 API（`check_api_schema_sync` 门禁把关）；表示形式取紧凑方案（主题节按角色名集中列出，不为每个角色建控件级条目），避免击穿 token 预算（见风险 1）。

### 分批落地计划

四批，批内可独立验证、批间可回滚；每批的门禁集合按 `AGENTS.md` §3.1 取（公共 API 批必跑 `check_api_schema_sync` / `check_naming_conventions` / `check_api_budget` / `check_doc_comments` / `lint` / `format-check`；文档批必跑 `check_codespec_xref` / `check_code_doc_sync` / `check_manual_test_format` / `check_change_proposals`）。

**立骨（类型与机械迁移，观感不变）**：落地 `color_roles.h` / `type_scale.h` / `theme_extension.h` 与 `Theme` 重构；18 个现有消费控件（11 个 primary 兜底 + 图表族 + `command_palette` + 焦点环）机械迁移到角色字段；删 `utest_style_props`、新增 `utest_theme_roles` / `utest_theme_extension`；`demo_theme` / `demo_provider` 适配。出口判据：全量 ctest 绿 + 上述控件在明暗两作用域下的 golden 与迁移前逐位相同（角色基准值 = 旧默认值的不变量由单测直接断言）。

**接线（默认注入与 OS 跟随，观感随系统）**：`Window` 主题状态 + `set_theme` + `prepare_context` 根注入 + OS 偏好缓存与 Win32 `WM_SETTINGCHANGE` 探测点；CSD typed 派生替换字符串令牌实现；`aurora_api.json` 刷新；`manual-test/13-theming.md` 增补默认注入与 OS 跟随用例。出口判据：验收判据 2–5。

**迁移（控件分批主题化，观感随主题）**：按「基础输入族（Button / TextInput / Checkbox / RadioSpin / Switch / Slider / SegmentedControl）→ 反馈族（Progress / Toast / Skeleton / Chip）→ 容器导航族（Dialog / Drawer / MenuBar / Toolbar / TabBar / BottomNavBar / Card）→ 图表与复杂族」推进，每族一批：角色映射表回写 `04-widget.md` + 单测/golden + codegen 描述符同步。出口判据：验收判据 6，且每族迁移后全量 ctest 绿。

**收束（旧面清除与记录）**：确认无残留引用后删除旧符号的全部文档表述；`CHANGELOG.md` 记破坏性条目与迁移路径；API 预算复核；手工测试全量执行一遍。出口判据：验收判据 7–9。

## 验收判据

1. `utest_theme_roles`：`ColorRoles::light()` / `dark()` 的每个 `on_*` 角色与配对容器角色 WCAG 对比度 ≥ 4.5:1；dark 全集表面族亮度显著低于 light 全集（明暗取向相反）。
2. 派生契约：`title_bar_style_from_theme(Theme::light())` 与 `from_preset(CsdPreset::AdwaitaLight)`、`Theme::dark()` 与 `from_preset(CsdPreset::AdwaitaDark)` 配色字段逐值相同（延续 CHG-010 判据，类型化版）。
3. 默认注入可观测：无 `ThemeScope` 的探针树经 `Window` 渲染，探针控件记录到的 `Theme` 非空且 `scheme` 为 OS 档位对应档；**变异自证**：打桩 `os_color_scheme()` 返回反转档位后探针必须读到反转后的角色（background 由浅变深或反之）——否则判据结构性不可观测，不给假绿。
4. OS 刷新：Win32 探测点在设置变更后使下一帧注入的主题档位翻转（探针或单测打桩验证，不依赖真机改系统设置）。


1. 换肤：`ThemeScope{State<Theme>}` 改值后，子树下一帧按新角色重绘（既有 `utest_theme_scope` 的迁移版，保留原判据强度）。
2. 迁移控件两态语义：未显式设色的 `Button` 在 light / dark 两个作用域下背景分别为对应档 `roles.primary`；显式 `set_xxx_color` 的对照组不被主题覆盖。
3. 无 stringly 热路径：`src/` 与 `include/` 中渲染相关源码对 `Theme::token(` 的调用为零（新增/复用的静态检查或 grep 门禁）；`Theme` 不含无序容器成员。
4. 门禁全绿：全量 `ctest --preset ninja-test`（含 `check_api_schema_sync`、`check_doc_comments`、`check_naming_conventions`、`check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_arch_module_map`、`lint`、`format-check`）；`check_api_budget` 不超预算或随本提案完成预算上调并注明理由。
5. 破坏性记录完整：`CHANGELOG.md` 顶部版本内记入破坏性条目与逐项迁移路径；所有 demo 与测试显式固定主题后观感与迁移前一致。

## 风险与未决项

1. **API token 预算**：现状约 66645/75000，余量 8355。角色集与扩展 API 入 schema 采用紧凑表示（主题节集中列出角色名与类型）；若紧凑表示仍超支，须上调 `tools/check/check_api_budget.py` 的 `TOKEN_BUDGET` 并在此提案补记理由——属 `SPEC.PERF.API.TOKEN-EFFICIENCY.001` 的预算调整，不静默放宽。**已收束**：紧凑表示后实测约 68896/75000，余量 6104，未上调预算。
2. **对比度自检可能回改角色现值**：若某 `on_*`/容器对不达 4.5:1，立骨批修正该角色取值会连带改变使用它的控件观感——此类变化须在 CHANGELOG 点名，不允许悄改。**已收束**：自检通过（dark primary/on_primary 为 7.73:1），未回改角色现值。
3. **OS 偏好刷新时机**：Win32 有 `WM_SETTINGCHANGE` 可挂；Linux/macOS 无对等信号，策略为「创建时读 + 激活时重读 + 应用可显式接管」，文档化而非假装实时。**已按策略落地**：Win32 侧 `WM_SETTINGCHANGE` 已接线（真机翻转验证留待人工走查）；Linux/macOS 维持「创建时读 + 激活时重读 + 应用可显式接管」文档化口径。
4. **未决：扩展插值**：`ThemeExtension<T>` 是否需要 `lerp`（主题切换动画）留待渲染性能专项一并决策，本提案不建半成品。
5. **未决：高对比度**：宿主设置里的 `high_contrast` 位与主题体系的关系（换色策略）属既有待拍板项，本提案不触碰，仅在文档标注边界。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§5「主题」整节重写：角色集与 Aurora 扩展角色、`TypeScale`、`ThemeExtension<T>`、`Theme` 新结构、默认根注入与 OS 缓存口径、三级优先链、CSD typed 派生契约、废除面清单）。
- `codespec/specification/04-widget.md`（控件取色两态语义与各控件「属性 → 角色」映射表，随迁移批增量回写）。
- `codespec/manual-test/13-theming.md`（载体基线更新：默认注入、OS 跟随、角色集视觉明暗与对比度判定用例）。
- 发布记录 CHANGELOG.md（破坏性条目与迁移路径）——该文档不属 codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际随各批同步。
- 机器可读 API 数据 aurora_api.json 刷新——同属根目录数据文件、不属 codespec 树，不列为回写落点，随接线批重建。
