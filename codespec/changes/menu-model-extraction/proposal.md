# CHG-014 菜单数据模型提取（MenuModel 单一真源 + 投影）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-014 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.NATIVE-MENU-BAR.001 |
| 影响面 | `app/menu.h`（新增 `MenuModel` 与投影函数、`MenuItem` 增 `id` 字段）、`widget/menu_bar.h`（`Menu` 增 `id` 字段）与其实现、`modifier/modifier_input.h` 的 `ContextMenuNode` 消费侧核对（零改动预期）、`app/system_tray.h` 与其 Win32 实现消费侧核对（零改动预期）、`codespec` 菜单相关章节、`tests/unit/utest_menu.cpp` / `utest_menu_bar.cpp` / `utest_modifier_input.cpp` / `utest_system_tray.cpp` / `utest_commands.cpp`（零改动预期，回归守护） |

## 动机

1. **菜单数据模型混装三层关注**：`app/menu.h` 的 `MenuItem` 把行为（`on_click`）、数据（`label` / `children` / `separator` / `checkable` / `checked` / `enabled`）、渲染提示（`shortcut_text` / `icon`——后者注释自陈「预留，当前未渲染」）装在同一结构里。`MenuBar`（`widget/menu_bar.h`，自绘）、`ContextMenuNode`（`modifier_input.h`）、`SystemTray`（`app/system_tray.h`）三方共用。
2. **原生桥的前置缺失**：`menu_bar.h` 的头注已自陈「Win32 原生 HMENU 映射作为后续增强（当前统一自绘保证行为一致）」。原生菜单桥（平台补齐程）需要消费同一数据模型的投影，但当前结构**没有稳定 id**——原生菜单项跨重建需要稳定标识路由点击，自绘路径靠指针身份即可、原生路径不行。
3. **单一真源范式已立**：`CommandRegistry::to_menu_items()`（`commands.h`）示范了「单一真源 + 多投影」（快捷键 / 菜单 / 命令面板三消费方）。若原生桥另造一套平行菜单 API，将出现自绘与原生两份真相——改菜单要改两处、状态同步无机制。把同一范式落到菜单数据本身：提取纯数据模型 `MenuModel`，自绘控件与原生桥都从它投影。
4. **立骨定位**：本提案只交付「模型 + 投影 + 测试」；平台原生渲染桥（Win32 窗口菜单 / macOS `NSMenu` / Linux appmenu 回落自绘）由平台补齐程提案落地，消费本提案的投影。

## 变更内容

1. `app/menu.h` 同址新增 `struct MenuModel`（纯数据，无回调、无渲染关注）：`id`（稳定标识，可空）、`label`、`enabled`、`checkable`、`checked`、`shortcut_text`、`separator`、`children`（`std::vector<MenuModel>`，非空即子菜单，与 `MenuItem::is_submenu()` 同口径）。不含 `on_click`（行为留在 `MenuItem`）与 `icon`（渲染提示留在 `MenuItem`）。**`MenuModel` 同时表达顶级菜单与菜单项**：顶级菜单的 `title` 映射 `label`、下拉项映射 `children`，二者共用一套模型——原生桥的顶级菜单同样需要稳定 id 路由点击，缺了它菜单栏就接不上（分隔符同时带 `children` 的组合语义未定义，投影仅保证保序与字段保真）。
2. 投影函数（`app/menu.h` 声明、`src/aurora/app/` 实现）：`to_menu_model(const MenuItem &) -> MenuModel`（递归转换，`id` 从 `MenuItem::id` 透传）；`to_menu_model(const Menu &) -> MenuModel`（顶级菜单：`title` → `label`、`id` 透传、`items` → `children` 递归）；`to_menu_models(const std::vector<Menu> &) -> std::vector<MenuModel>`（菜单栏整体投影，原生桥的主消费入口）；`from_menu_model(const MenuModel &) -> MenuItem`（回转，`on_click` 为空、`icon` 为空——供原生桥点击经 id 反查命令的场景）。`CommandRegistry::to_menu_items()` 产出侧不变；平台补齐程需要命令侧原生菜单时可组合 `to_menu_model` 得到模型，本提案**不新增命令侧投影 API**（避免 API 面扩大，与「不建平行菜单 API」同一理由）。
3. `MenuItem` 与 `Menu` 各增唯一字段增量：`std::string id`（默认空串；自绘路径忽略，声明侧可选填稳定 id）。`MenuItem` 其余字段、默认值、`separator_item()` / `is_submenu()` 行为逐位不变；`Menu` 的 `title` / `items` 语义与 `MenuBar` 构造、绘制、命中逻辑逐位不变（`MenuBar` 存 `std::vector<Menu>`，新增字段不参与其自绘任何分支）。
4. `Menu` / `MenuBar` / `ContextMenuNode` / `SystemTray` 的既有字段、构造与绘制行为零改动；`MenuModel` 投影为新增消费路径，不改自绘路径任何一行逻辑。
5. 原生桥不落本提案：平台补齐程落地时消费 `to_menu_model` 投影（Win32 `HMENU` / macOS `NSMenu` / Linux appmenu 无全局菜单协定时回落自绘 `MenuBar`），自绘与原生「二显一存」。
6. 既有菜单测试（`utest_menu` / `utest_menu_bar` / `utest_modifier_input` / `utest_system_tray` / `utest_commands`）零改动，作为回归守护；新增投影单测。

## 验收判据

1. 投影往返保真单测：`MenuItem` → `MenuModel` → `MenuItem` 对数据字段（label / enabled / checkable / checked / shortcut_text / separator / children 嵌套）逐字段保真；`id` 透传；空 `children` 不产生子菜单语义（`is_submenu()` 口径一致）。顶级菜单投影：`Menu` → `MenuModel` 的 `title` → `label`、`items` → `children`、`id` 透传，且 `to_menu_models` 对菜单栏顺序逐位保真（菜单栏顶级项顺序即投影顺序）。**变异自证**：投影漏传 `id`、children 判读错误、或顶级 `title` 误映射，往返用例必须转红。
2. `MenuItem` 唯一字段增量的零回归：既有菜单用例（`utest_menu` / `utest_menu_bar` / `utest_modifier_input` / `utest_system_tray` / `utest_commands`）全绿且不改动；默认构造的 `MenuItem` 新增 `id` 为空（既有默认构造断言不受影响）。
3. 投影纯度：`to_menu_model` / `from_menu_model` 为无副作用纯函数（不触碰全局状态、可重入），单测以多次调用结果一致断言。
4. API 面治理：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移；`check_api_budget` 增量评估通过（新增一个结构 + 两个自由函数，无新门面）。
5. 门禁与测试：`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md`（§8 平台 Shell：新增原生菜单桥节的数据模型契约——MenuModel 单一真源、投影方向、自绘 / 原生二显一存；原生渲染实现留平台补齐程）
- `codespec/specification/04-widget.md`（MenuBar 控件节：菜单数据模型单一真源与投影表述）
- `codespec/SPECIFICATIONS.md`（需求行已于本提案同批落定）
