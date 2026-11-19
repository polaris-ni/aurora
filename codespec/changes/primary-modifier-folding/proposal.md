# CHG-087 应用内组合键的主修饰键折算（macOS 上 ⌘ 与 Ctrl 同位）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-087 |
| 提出日期 | 2026-11-14 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `include/aurora/event/event.h`（新增两枚纯函数）；`include/aurora/app/shortcuts.h`（`KeyCombo::matches` 的匹配算式）；`include/aurora/widget/text_input.h`、`src/aurora/widget/rich_text_edit.cpp`、`src/aurora/widget/text.cpp`（编辑判定的修饰键来源）；`aurora_api.json` 重生成；`tests/unit/utest_shortcuts.cpp` / `utest_text_input.cpp` / `utest_rich_text_edit.cpp` 扩例；`codespec/manual-test/` 一条真机用例；`codespec/specification/05-event-navigation.md` §2.2、`06-app-platform.md` §8.2 / §8.4 / §8.6、`codespec/GUIDELINE.md` 快捷键示例注释 |

## 动机

1. **macOS 上应用内组合键整层哑火，且断点已被实证在消费侧，不是猜的。** 三个自绘文本控件（`TextInput` / `RichTextEdit` / `Text`）的 ⌘C / ⌘X / ⌘V / ⌘A 全无反应；`app.shortcuts()` 与 `CommandRegistry::bind_shortcuts` 里以 `ModifierKey::Control` 申报的组合（`GUIDELINE.md` 与 `application.h` 头注释给出的正是这种写法）在 macOS 上恒不命中。采集侧是对的：`src/aurora/window/detail/macos_keymap.h` 把 AppKit 的 Command 位折算为 `ModifierKey::Meta`，`macos_surface.cpp` 的 `keyDown:` 照此发出 `KeyEvent`，并且**刻意**让 Command 组合跳过 `interpretKeyEvents:`——即框架自己已判定「这是快捷键而非文本」。断点在消费侧三处：控件写 `(e.modifiers & ModifierKey::Control) != 0`，而 `KeyCombo::matches` 是两侧摘掉锁定位后的**等式**比较，Meta 位永远不等于 Control 位。
2. **缺的不是一个 macOS 分支，而是一层没有落点的口径。** `ModifierKey` 建的是**物理按住位**，`KeyCombo` 建的是**申报的组合**，二者之间「平台主修饰键」（Windows/Linux 的 Ctrl、macOS 的 ⌘）这一层语义在框架里没有任何名字。于是同一个判断在六个位置各写一遍 `& ModifierKey::Control`，六处可各自漂移。散补（在每处加一条 `|| Meta`）会让第六处与第一处的写法分叉，且 `matches` 的等式语义根本容不下就地加条件。必须先定口径、给概念命名。
3. **业界分两档，本框架取「申报位折叠」而非「物理位改写」。** JavaFX `Modifier.SHORTCUT`、Electron 加速器串里的 `CmdOrCtrl`、GTK 的 `<Primary>` 都是**同一个物理位在不同平台读作不同键**的折叠口径；Flutter 走另一档——每平台各写一套 `SingleActivator` 表（`macOS: meta` / `其余: control`），代价是同一份键位表要维护两份真值。取折叠档的理由：① 事件层的 Command→Meta 是物理真值，改成 Command→Control 等于让采集层说谎，而 `utest_macos_keymap` 逐值钉的就是这份真值；② 折叠只需要在「比较之前」落一次，不动 `to_string()` / `from()` 的逐字节往返契约（那是宿主覆盖表重放的入口，形态一变即静默丢条目），也不新增枚举位（`ModifierKey` 的按住态 / 锁定态两位集划分有自证用例，掺进逻辑位会污染掩码代数）；③ 折叠是**只增不减**：本批前后任何今天能命中的组合仍命中，不产生「某条绑定忽然失效」这种最难排查的回归形态。
4. **折叠的代价要如实写死，不许粉饰。** 折算后在 macOS 上 ⌘X 与物理 Ctrl+X 同解，`KeyCombo{Control, X}` 与 `KeyCombo{Meta, X}` 亦同解（注册序先命中者胜，`utest_shortcuts` 既有「first matching binding wins」用例已覆盖该次序）。即 macOS 上**无法再区分**物理 Ctrl 与 ⌘ 的应用内组合键。这与 Qt / GTK 的取舍同款，本框架接受它；要区分的场景（OS 级全局热键）本来就不走这层，见下条。
5. **两处同类缺口明确不入本批，各因其自身的口径未定，而非遗漏。**
   - **OS 级全局热键（`OsHotkeyRegistry`）**：macOS 腿 `to_macos_modifiers()` 现按物理位申报（Control→`controlKey`、Meta→`cmdKey`）。它交付给 OS 的是**系统全局键表里的一条登记**，折叠等于把今天确实能用的 Ctrl+Alt+Space 静默改成 ⌘ 组合，还会改变跨进程抢占冲突的判定对象。保持物理申报，并在文档写明「OS 级热键要 ⌘ 就写 `Meta`」。
   - **原生菜单的 ⌘ 显示与 `keyEquivalent`**：`MenuModel::shortcut_text` / `MenuItem::shortcut_text` 的公共契约是**「仅显示」的自由文本**，`macos_menu_bar.cpp` 据此只把文本用制表符追加到标题、明确不生成 `keyEquivalent`。折叠算式需要结构化组合键才能落到 `NSMenuItem` 的修饰掩码，而「要不要给菜单模型一枚 `KeyCombo`、`shortcut_text` 的显示职责是否随之改由 OS 绘制」是菜单数据模型的独立口径决定，另立提案，不在本批顺手改公共字段。
   - **系统拖放的修饰键翻译**（`system_drag::proposed_action_for`：Ctrl→Copy / Shift→Move，契约明写 Windows 惯例）：同族缺口的另一半，但它改的是拖放动作语义而非组合键匹配，归系统拖放批次；本批只登记，不动码。
6. **不建第二套快捷键 API。** 本批只新增两枚无副作用的纯函数，`KeyCombo` / `ShortcutRegistry` / `CommandRegistry` / 控件的既有公共签名与行为矩阵全不变；`matches` 仍是单一算式，档位、枚举参数、per-combo 标志一概不加。

## 变更内容

1. **折算层（`include/aurora/event/event.h`，纯 `constexpr`，零新增 include，编译期平台分支）**：
   - `fold_primary_modifier(ModifierKey mods) -> ModifierKey`：非 macOS 构建恒等返回；macOS 构建把 `Meta` 位并入 `Control` 位并清除 `Meta`（`Control | Meta` 同按时结果为 `Control`），`Shift` / `Alt` 与锁定态位逐位透传。本函数是「主修饰键」的**唯一折算点**。
   - `has_primary_modifier(ModifierKey mods) -> bool`：折算后 `Control` 位是否置位，即「此刻按着的是不是本平台主修饰键」。控件与宿主侧判定只经这一枚，不再手写位与。
   - 两枚均为公共 API 追加，落码后重生成 `aurora_api.json`。
2. **匹配算式（`include/aurora/app/shortcuts.h`）**：`KeyCombo::matches` 在既有「两侧先与 `AURORA_MODIFIER_PRESSABLE_MASK` 取子集」之后，各再经一次 `fold_primary_modifier`，然后**仍按等式**比较。文档注释里那三条口径（相等而非包含、注册侧锁定位不可表达、不加档位 / 字段）逐字保留并补一条 macOS 折叠后果；`to_string()` / `from()` 不改。
3. **控件编辑判定（消费方接线）**：`include/aurora/widget/text_input.h`、`src/aurora/widget/rich_text_edit.cpp`（`handle_control_shortcut` 与 Home / End 两处独立判定）、`src/aurora/widget/text.cpp` 里原先只认 `Control` 位的判定改走 `has_primary_modifier(e.modifiers)`。**只改判定来源**：每个键的行为、只读态降级（⌘C / ⌘X 仅复制、⌘V 忽略）、选区与光标语义、失败留痕路径一概不变。
4. **注释与文档**：`ModifierKey` 枚举头补「物理位建模 + 主修饰键折算另在本层两枚函数」的分工说明；`application.h` 的快捷键用法示例注释写明「`Control` 在 macOS 上读作主修饰键（⌘），OS 级热键另按物理位」。
5. **测试**：`utest_shortcuts.cpp` 加折算真值表（逐位幂等、锁定态不受染、非 macOS 恒等）、`matches` 在折叠后仍是等式而非包含（`{Control,O}` 不得命中 `{Control,Shift,O}`）、macOS 专属期望用编译期分支钉住；`utest_text_input.cpp` / `utest_rich_text_edit.cpp` 加「主修饰键 + C / X / V / A / Home / End」编辑路径用例；既有 NumLock 掩码划分自证用例保持绿。
6. **不落本批**：菜单 `keyEquivalent` 与 ⌘ 字形显示、OS 级热键折叠、拖放修饰键翻译、`KeyEvent::modifiers` 的任何形态改动。
7. **发布记录**：`CHANGELOG.md` 顶部当前版本段落 `### Fixed` 追加一条，含 macOS 上不可区分物理 Ctrl 与 ⌘ 这一代价的如实登记——该文档不属 codespec 树，按 `AGENTS.md` 硬规则 5 不列为回写落点。

## 验收判据

1. 非 macOS 构建上 `fold_primary_modifier` 逐位恒等，`matches` 与三处控件判定的行为与本批之前**逐字节等价**（既有全套用例不得改动断言）。
2. macOS 构建上：`{Control, O}` 命中 ⌘O 与 Ctrl+O；`{Control, Shift, O}` 不命中 ⌘O（等式未被改成包含）；`{Meta, O}` 与 `{Control, O}` 在 macOS 上同解、在非 macOS 上仍严格区分。
3. 三处文本控件在 macOS 上 ⌘A / ⌘C / ⌘X / ⌘V 生效，`RichTextEdit` 的 ⌘Home / ⌘End 跳到文档首尾；只读态与禁用态降级路径不变。
4. `has_primary_modifier` 成为控件侧唯一判定入口：全仓 `src/aurora/widget/` 与 `include/aurora/widget/` 的编辑类判定不再出现裸的 `& ModifierKey::Control`（OS 热键、拖放翻译、a11y 布尔三处按「不落本批」清单保留，逐处可解释）。
5. 真机人工用例在 macOS 上执行：⌘C 落系统剪贴板且能在外部应用粘出、⌘A 全选、注册的 ⌘O 快捷键动作被触发一次；结果按 `manual-test` 契约记入执行记录表（PASS / FAIL / SKIP 语义不放宽）。
6. `check_api_schema_sync`（先 `aurora_api_json`）/ `check_naming_conventions` / `check_api_budget` / `check_doc_comments` / `check_platform_macros` / `format-check` / `check_manual_test_format` / `check_change_proposals` 全绿，`lint` 经 CI 增量作业零新增命中，全量 `ctest` 无新增红灯。

## 回写落点

- `codespec/specification/05-event-navigation.md` §2.2：修饰键语义段补「物理位 vs 平台主修饰键」的分工与两枚折算函数。
- `codespec/specification/06-app-platform.md` §8.4：`KeyCombo::matches` 的匹配语义四条口径处补折叠；§8.2 控件层剪贴板快捷键表述由「Ctrl+C/X/V」改称主修饰键口径；§8.6 OS 级全局热键的分工表补「按物理位申报，折叠不适用」及理由。
- `codespec/GUIDELINE.md`：快捷键与命令 `default_binding` 示例处的注释。
- `codespec/manual-test/`：对应模块追加一条真机用例及其执行记录行。
