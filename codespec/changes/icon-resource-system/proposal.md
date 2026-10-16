# CHG-019 图标组件支持：`Icon` / `IconButton` 与 `IconData` 值语义（图标资产由应用提供，库不内置图标库）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-019 |
| 提出日期 | 2026-10-15 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.WIDGET.FLAT-COMPONENTS.001 |
| 影响面 | 新增公共模块 `icon/`（命名空间 `aurora::icon`；公共头 `icon.h` 与 `icon_svg.h` + `src/aurora/icon/` 实现）；新增公共控件 `Icon` 与 `IconButton`（第 75、76 个 widget，`aurora_api.json` 与 `umbrella_manifest.txt` 同步收录）；新增应用侧语义名注册表 `IconRegistry`（库不预置任何条目）；`Widget` 既有 `accessibility_label()` 虚接口承载图标语义标注（`Icon` 覆写之）；`Chip` 头像槽增 `IconData` 入口（保留既有字符串入口）；`codespec/errors.toml` 新增 `icon-*` 错误组；手动走查新增图标卷。**不内置任何图标字体或 SVG 资产**：`third_party/` 不新增组件、`THIRD_PARTY_LICENSES.md` 不动、无子集化管线、无生成映射头、无编译开关。不改 `Painter` / `Surface` / 布局 / 事件既有语义，不改 CSD 标题栏逐位视觉契约 |

## 动机

1. **缺口是公共 API 面的真实缺口，不是体验优化**：`aurora_api.json` 现有 74 个 widget（自 `Badge` 到 `WebView`），其中**没有** `Icon` 也没有 `IconButton`，全库零处 `Icon` 字面量。图标能力目前以点状实现存在：CSD 窗口控制钮在 `title_bar_painter.cpp` 以 SDF 原语逐个手绘（minimize / maximize / close 三槽，几何由 `TitleBarGeometry` 按声明序钉死）；应用图标经 `set_title_bar_icon` 以 RGBA 位图注入后走 `draw_image`；`Chip` 的头像槽 `set_avatar` 只接受字符串（业务侧以 emoji 或首字母近似）；托盘与通知沿用平台原生资产。结果是业务侧只能自造图标方案，明暗主题与跨平台观感无法一致，AI 生成界面时也没有统一的图标槽可填。

2. **库只提供机制，资产由应用提供——这是本次的范围决策**：图标的**风格、许可、版本、体积**是应用的品牌与法务决策，不该由 GUI 库代持。内置一套图标字体意味着库要长期承担子集化、码位冻结、许可登记（Apache-2.0 的 NOTICE 与专利声明、MIT 与 ISC 的版权声明，义务各不相同）、版本升级与体积增长——这些都不是渲染库的职责。取而代之：库提供 `IconData` 值语义 + `Icon` / `IconButton` 控件 + 语义名注册机制，字体与 SVG 资产由应用经既有 `FontEngine::register_font` / `register_font_from_memory` 或字节注入提供。**代价是「开箱即有图标」不再成立**——未注册任何图标资产的应用，图标槽返回结构化错误而非静默画空；这一点已写入非目标与验收判据。

3. **底座已就绪，主通道无需改动渲染内核**：`FontEngine::register_font_from_memory` 接受字节向量注册并具备 `GlyphAtlas` 字形图集缓存（`include/aurora/render/font_engine.h`、`include/aurora/render/glyph_atlas.h`），应用注册的图标字体可直接经既有文本管线绘制，天然支持任意字号与 `Color` 取色。补充通道的 `load_image_svg(buf, target_w, target_h)`（`include/aurora/core/image.h`）提供内置 SVG 子集全矢量光栅化的**内存字节入口**——本变更只走该入口而不走 `load_svg(path, …)`：后者依赖文件系统（wasm 目标不具备），且编解码注册表只支持固有尺寸。

4. **不内置清单，就不需要巨型枚举——顺带消掉一处门禁冲突**：若内置一套 200~400 个语义名的图标集，`IconName` 须以 `enum class` 暴露，而 `check_naming_conventions.py` 硬性要求「枚举值 PascalCase（`TopLeft`、`Center`…），仅 `AURORA_*` 常量豁免」，既有枚举（`Alignment`、`ToastPosition`、`TextOverflow`）无一例外——草案形态 `search` / `chevron_right` 会直接红灯，且要在 `known_enums.h`、`itest_known_enums.cpp`、`aurora_api.json` 枚举段同步 400 项。改为「库不预置清单」后，`IconData` 只携带（字体族 + 码位）或 SVG 源，语义名由应用注册，**巨型枚举连同其命名冲突与约 1,700 token 的枚举段开销一并消失**。

5. **业界对标：这正是 Flutter 的分工，且 WinUI 的枚举只用于内置集**：

| 框架 | 库侧提供 | 应用侧提供 | 对本变更的结论 |
|:---|:---|:---|:---|
| Flutter | `IconData` **值类型**（`codePoint` + `fontFamily` + `fontPackage` + `matchTextDirection`）+ `Icon` 控件 | 字体在 `pubspec.yaml` 声明；内置 `Icons` 清单是**可选的 Material 依赖**，自定义图标字体由应用自己声明并手写 / 生成常量 | **同构**：库给值与控件，应用给字体和清单。本变更取此分工，但不附带内置清单 |
| Material Symbols（Flutter 移植） | 静态成员 `Symbols.add_task` 等 | 按名取码位的 `get(String)` 是**可选导入**，官方明示「为免拖入整张 name→codepoint 表」 | 印证「名字→码位表应是可选层」，与本变更的 `IconRegistry` 定位一致 |
| WinUI | `FontIcon`（码位）/ `SymbolIcon`（`Symbol` **枚举**）/ `PathIcon` / `BitmapIcon` / `ImageIcon` | Segoe Fluent Icons 之外的字形由应用引入字体（如 FluentSystemIcons-Regular.ttf） | **枚举只用于库内置的那套系统集**；库不内置 ⇒ 无枚举，逃逸口留给码位 |
| SwiftUI | `Image(systemName:)` 字符串键 | SF Symbols 由系统提供 | 系统供给资产、库只给入口，与本变更同构 |

   结论：`IconData` 取 Flutter 的值语义，`IconRegistry` 承担「可选的名字层」（Material Symbols 的 `get.dart` 那种可选导入），不引入 `IconName` 枚举。

6. **位图变体不纳入**：WinUI 官方选型指引写「单色或字体图标不要用 `ImageIcon`」，Flutter 同样把位图 / SVG 划给 `Image` 控件（徽标插画用 `Image`，界面小符号用 `Icon`）。本库已有 `Image` 控件承担该职责，再建模一份是重复抽象。`IconData` 只取 `Glyph` 与 `Svg` 两变体；若后续 `Painter` 落地按色着色原语，届时以 `BitmapIcon` 形态单独提案，不与本次同批。

7. **预算口径需纠正，且本变更的增量更小**：`aurora_api.json` 现为 168,237 B，按 `check_api_budget` 的 4 B/token 口径约 42,059 tokens，对 `TOKEN_BUDGET = 75000` 的余量约 33,000 tokens（约 44%），不是数千。分段实测：widgets 段 68,835 B、error_codes 段 32,897 B、enums 段 2,969 B（26 个枚举，最大者 `KeyCode` 78 项）。本变更增量：两个 widget 描述符约 2~3 KB、`Kind` 两项枚举可忽略、3 条 `icon-*` 错误码约 1 KB——**合计约 1,000 tokens**，占余量不足半成（内置方案原估约 2,700）。

8. **两处在途交叠必须先声明，否则返工**：其一，`app/menu.h` 的 `MenuItem` 正被另一提案（菜单模型抽取）改动，本变更的「菜单项接入 IconData」须排在其后或以该提案落定后的形态接线；其二，`CommandPalette` 虽在 `command_palette.h` 中定义，但**不在** `aurora_api.json` 的 74 个 widget 清单内，其「结果行接入图标」不产生 schema 变更，判据也 accordingly 不同。

## 变更内容

### 立骨批次：icon 域类型地基与主通道最小闭环

1. 新增公共模块 `icon/`：`icon.h`（域伞头，导出 `IconData`、`IconRegistry`、`IconData::Kind`）、`icon_svg.h`（`SvgIconSource` 与光栅缓存）与实现目录 `src/aurora/icon/`。
2. `IconData` 为值语义描述符，**两变体互斥**：`Glyph` 变体持字体族名与码位（`char32_t`）；`Svg` 变体持 SVG 源字节与可选的源标识（供缓存键与诊断使用）。可拷贝、可比较、无虚函数。
3. `IconRegistry` 是**应用侧的可选名字层**：应用可注册「语义名 → `IconData`」映射（语义名一律 ASCII snake_case，表达含义而非位置或尺寸），库**不预置任何条目**。查询未注册的语义名返回结构化 `Error`，并把已注册名作为候选回给诊断——这让 AI 与 `validate_ui` 有一条可枚举、可纠错的引用路径，同时库不背资产。
4. 新增控件 `Icon`（`LeafWidget` 派生，第 75 个 widget）：属性 `icon`（语义名串或 `IconData` 字面）、`size`、`color`、`semantic_label`；默认色取 `ColorRoles::on_surface_variant`，显式 `color` 覆盖优先（沿用控件既有两态取色语义，与 `include/aurora/theming/color_roles.h` 的 `std::optional` 口径一致）；尺寸档位对齐 `TypeScale`。**`set_size` 必须经 `Widget::mark_size_intent_changed()` 触发父侧重排**——定尺寸控件天然是重排边界，`mark_needs_layout_impl` 对边界短路、不向父侧传播，只标自身会导致父侧几何停留在旧值、`window_bounds()` 返回过期盒。
5. 交付 `utest_icon.cpp` 单测与 `demo_icon_gallery.cpp` 演示（演示自带一枚应用侧字体与若干 SVG，仅作示例资产，不进库）。

### 接线批次：交互完整性

1. 新增控件 `IconButton`（第 76 个 widget）与状态机：悬停取 `on_surface`、按下取 `primary`、禁用沿用既有禁用态透明度规则、焦点环复用 `ColorRoles::focus_ring`；取色两态语义在全部四态下均须成立。
2. 尺寸档位与光学补偿：提供 16 / 20 / 24 / 32 px 基准档；跨两档以上跨度的字形绘制引入光学中心垂直补偿（按字体度量校正字形基线与视觉中心的偏差），补偿量参数化而非硬编码。
3. 品牌扩展复用 `ThemeExtension<IconSet>`（`include/aurora/theming/theme_extension.h`）：应用可注入自有图标集覆盖默认映射，未注册时回退空集，零字符串键。
4. 无障碍接线：`Widget` **既有**虚接口 `accessibility_label()`（含 `set_accessibility_label()` 与显式声明回退链，`include/aurora/widget/widget.h`）作为语义标注载体，`Icon` 覆写返回 `semantic_label`。纯装饰图标须保持 `accessibility_is_semantic()` 返回 false，仅当设置了语义标注时才进无障碍树。
5. 交付 `itest_icon_theme.cpp` 等集成测试，覆盖状态迁移、主题切换重绘与 DisplayList 命令复用。

### 迁移批次：既有点位 IconData 化与补充通道

1. `Chip` 头像槽增 `IconData` 入口，**保留**既有 `set_avatar(std::string)`，两入口互斥、默认观感不变（既有 `avatar` 属性键与序列化行为不动，避免 `check_api_schema_sync` 之外的隐性破坏）。
2. `SvgIconSource` 落地：按（内容哈希、目标尺寸、目标色）三元组为键调用 `load_image_svg` 重光栅化，LRU 按字节封顶、满时淘汰并写诊断日志；命中后复用 `draw_image`。**不与 `ImageCache` 合并**——既有 `ImageCache`（`include/aurora/render/image_cache.h`）以路径字符串为键、经 `Image::load` 解码（固有尺寸），既无法表达尺寸与着色维度，合并还会污染其 64 MB 预算口径与命中统计；淘汰实现可复用其链表手法。
3. SVG 入库前按特性白名单校验（内置光栅化只支持 SVG 子集），不满足者拒绝并返回结构化诊断，不静默降级。
4. 菜单项接入排在菜单模型抽取决案之后，按落定形态接线；`CommandPalette` 结果行接入不产生 schema 变更，出口判据按非 widget 口径单独写。
5. **CSD 标题栏只评估不实施**：其 SDF 手绘与 `TitleBarGeometry` 的逐位视觉契约保持稳定，真机走查成本高而收益低。

### 收束批次：文档、数据与治理闭环

1. `codespec/errors.toml` 新增 `icon-*` 错误组（语义名未注册 / 字体族未注册或码位缺失 / SVG 特性不支持三类），经生成器重建 `error_codes.gen.h`、`codespec/ERROR_CATALOG.md` 与 `aurora_api.json` 的 error_codes 段。
2. 重建 `aurora_api.json`（不手改）；`IconData::Kind` 若作为属性类型暴露则登记进 `known_enums.h` 与 `itest_known_enums.cpp`（仅两项）；`umbrella_manifest.txt` 对新增公共头分类（`icon.h` / `icon_svg.h` 进 `[direct]`）。
3. 语义名治理**由应用自持**：库侧只约定形态（ASCII snake_case、表达含义而非位置或尺寸）与「未注册即报错」的行为；应用侧图标集的版本冻结、许可合规与改名迁移均不在本库范围内。
4. 性能基线采集与预算复核；本提案状态流转为已归档。

### 非目标边界

- 不内置任何图标字体或 SVG 资产，不提供开箱即用的图标集；未注册资产时返回结构化错误，不静默绘制占位图形。
- 不引入图标资产子集化管线与生成映射头，不新增编译开关。
- 不引入 GPU 路径，不改软件栅格架构；不改动 CSD 标题栏既有逐位视觉契约。
- 不重建平台原生图标桥（托盘、通知、原生菜单继续用宿主资产）。
- 首期不做动效图标（Lottie / APNG），多色仅限静态 SVG。

## 验收判据

1. **零内置资产（独立基准）**：`third_party/` 不新增任何图标字体或 SVG 组件，`THIRD_PARTY_LICENSES.md` 无改动，构建产物中不含图标字体字节数组。**变异自证**：在库内塞进一枚内置图标字体字节表并让默认集非空，判据必须转红。
2. **值语义与两变体互斥**：单测钉死 `IconData` 的构造、拷贝、比较与两变体互斥（设 `Glyph` 后 `Svg` 字段不可读，反之亦然），序列化往返无损。
3. **未注册即报错，不静默**：未注册语义名时查询返回结构化错误并回带已注册名候选；字体族未注册或码位缺失时同理。**变异自证**：把「未注册」改成「静默返回空 `IconData` 并绘制空白」，用例必须转红——静默回落会让缺资产退化成难以定位的空白渲染。
4. **可枚举性**：应用注册 N 个语义名后，`validate_ui` 对拼错的语义名报结构化错误且候选列表含正确名；`aurora_api.json` 的 widget 计数由 74 升为 76。
5. **命名门禁**：`check_naming_conventions` 绿——`Kind` 成员为 PascalCase（`Glyph` / `Svg`），全部新增属性键为 snake_case，两个新控件名为 PascalCase。**变异自证**：把 `Kind` 成员改回 snake_case，门禁必须转红。
6. **两态取色**：单测钉死「未显式设色时取 `on_surface_variant`、显式设色时取显式值」在明 / 暗两套 `ColorRoles` 下均成立，且 `IconButton` 四态（悬停 / 按下 / 禁用 / 焦点环）各自取色正确。**变异自证**：把默认取色改写成常量色，用例必须转红。
7. **几何不回退**：`Icon` 改尺寸后父侧几何同步更新——断言 `window_bounds()` 在 `set_size` 之后返回新盒。**变异自证**：把 `set_size` 的 `mark_size_intent_changed()` 换成 `mark_needs_paint()`，用例必须转红（只标脏 paint 时父侧几何停留旧值）。
8. **缓存键与淘汰**：单测覆盖（内容哈希、尺寸、色）三元组——同键命中、任一维变化即未命中、超字节上限按 LRU 淘汰并写诊断日志。**变异自证**：把键降维成「仅内容哈希」，多尺寸用例必须转红。
9. **预算判据**：重建后 `check_api_budget` 实测 ≤ 45,000 tokens（现值约 42,059，预期增量约 1,000）；widget 计数 76，enum 计数按 `Kind` 是否作为属性类型暴露相应变化，error_codes 条目数按新增组数递增且序不变。
10. **门禁与全量**：`check_api_schema_sync`、`check_naming_conventions`、`check_doc_comments`、`check_umbrella_header`（R8 要求新增头必须被分类）、`check_arch_module_map`（`icon/` 域登记进模块映射）、`check_core_layer_boundary`、`check_code_doc_sync`、`check_change_proposals`、`check_codespec_xref` 全绿；`check_no_cjk_literals` 与 `check_no_hardcoded_paths` 绿（图标语义名与资产标识一律 ASCII、不含本机绝对路径）；lint（仅改动相关 TU）与 format-check 绿；全量 `ctest -j16` 绿，并覆盖 wasm 与 Linux 两个实现（`load_image_svg` 的内存入口保证 wasm 无文件系统依赖）。
11. **图像基线**：`demo_icon_gallery` 在明 / 暗两主题下的渲染基线入库（复用 `tests/golden/` 与 `tests/framework/golden.h` 的既有机制），既有基线零差异。**变异自证**：把光学补偿量改错，基线比对必须转红。
12. **手工走查**：图标卷用例按 `TC-ICON-001` 起连续编号、预期与实测逐字对齐，执行记录表一用例一行；覆盖 Windows、GLFW、X11、Wayland 四个后端的明暗主题走查，并含一条「未注册资产时的错误提示」人工确认项。

## 回写落点

- `codespec/ARCHITECTURE.md`（§4 模块映射增 `icon/` 域一行，与 `webview/` 的登记范式对齐；`check_arch_module_map` 据此处校验）
- `codespec/specification/04-widget.md`（`Icon` / `IconButton` 控件规约：属性表、状态机取色表、属性到主题角色的映射）
- `codespec/specification/03-layout-render.md`（字形直绘路径、SVG 光栅与缓存键行为、尺寸档位与光学补偿约定）
- `codespec/specification/08-tooling.md`（aurora_api.json 的两个新 widget 描述符与 `IconData::Kind` 口径；自描述发现 API 表同步）
- `codespec/CODING_STANDARDS.md`（图标语义名的命名规范：ASCII snake_case、表达含义而非位置或尺寸；语义名由应用注册、库不预置条目）
- `codespec/errors.toml`（新增 `icon-*` 错误组，经生成器重建三项产物）
- `codespec/manual-test/`（新增图标卷：多平台明暗主题走查用例，按既有编号与格式规约编写）
