# CHG-006 `CommandPalette` 的上屏文案改走 i18n 词条表（占位符与空态提示）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-006 |
| 提出日期 | 2026-10-08 |
| 当前状态 | 实施中 |
| 关联需求 | 无 |
| 影响面 | `widget/command_palette.h` 的两处上屏文案来源、新增的属性读写通道（`describe_static` / `serialize_props` / `deserialize_props`）、绘制期对 `default_string_table()` 的查表面 |

> 目录名说明：本仓提案编号按目录名字典序升序分配且**不重排**（见 `codespec/changes/README.md` §3.1），
> 既有五个目录均为 `widget-` 前缀且字典序末位为 `widget-window-bounds-popup-reachability`（CHG-005）。
> 故本提案目录取 `widgets-` 前缀以保证字典序排在其后、取得 CHG-006 而不动任何既有编号与交叉引用。

## 动机

**`CommandPalette` 的两处上屏文案是硬编码英文字面量，没有任何 i18n 查表路径。**

| # | 位置 | 现状 |
|:--|:--|:--|
| 1 | 构造函数内 `field->set_placeholder("Type a command...")` | 搜索框占位符 |
| 2 | `on_paint` 内 `p.draw_text(..., "No matching commands", ...)` | 无匹配结果时的空态提示 |

库自身**已有**完整的 i18n 机制（`i18n/localized_string.h` 的 `LocalizedString::tr(key)` +
`i18n/string_table.h` 的 `default_string_table()` + `Locale`，查表失败回退 `ls.text`），
并且已有控件级先例：`ReorderableListView::announce_text(key, args, fallback)`
（`widget/reorderable_list.h`）就是「按 key 查 `default_string_table()`、未登记时回退英文字面量」的同一口径；
`Text` / `Button` 则在绘制期用 `ctx.environment<Locale>()` 解析。

于是形成一处**内部不自洽**：同一棵控件树里，`Text` 的 `"Save"` 换语言会变，紧挨着的命令面板搜索框
`"Type a command..."` 换语言不变。中文界面下搜索框与空态仍是英文，直接违反
`CODING_STANDARDS.md` §14 的例外规则——字面量一律 ASCII 英文这条规则的**例外**是「上屏文案须走词条表」，
而这两处正是上屏文案却走了字面量。

**宿主当前无法自行修正。** 这两串文本的写入点在构造与绘制内部，宿主既拿不到查表入口，
也换不掉 key——只能忍着，或自己重画一个面板。

## 变更内容

### 文案来源：字面量 → 词条查表 + 兜底

新增两个库内定的默认 key 常量（与 `aurora.reorder.*` 同命名空间风格）：

| 常量 | 值 | 兜底文本（查表失败时） |
|:--|:--|:--|
| `AURORA_DEFAULT_PLACEHOLDER_KEY` | `"command_palette.placeholder"` | `"Type a command..."` |
| `AURORA_DEFAULT_EMPTY_MESSAGE_KEY` | `"command_palette.no_results"` | `"No matching commands"` |

内部以 `LocalizedString` 持有（key + 兜底 `text`），**不在库内预置任何语言的词条**——
`default_string_table()` 只由宿主登记译文，库不往里写。这样「我没翻译」与「库自带英文」在宿主侧可区分，
且因 `StringTable` 无删除接口，库侧不写入就不会给宿主留下不可逆污染。

### 解析时机：绘制期带 `ctx` 的 `Locale`

`on_layout` / `on_paint` 内按 `ctx.environment< Locale >()` 解析（缺失回落 `Locale{}`），
与 `Text::resolved_text` / `Button` 完全同口径。占位符在解析后同步给内置 `TextInput`。
运行时切 locale 立即生效；构造函数不再固化文本。

### 公共面新增（纯增量，无破坏性）

| 入口 | 作用 |
|:--|:--|
| `set_placeholder(const std::string &)` | 覆盖最终占位符文本；调用后该串**不再查表**（与 `TextInput::set_placeholder` 同口径） |
| `set_placeholder_key(const std::string &)` | 换词条 key；换 key 后**仍走 i18n**，支持同一进程内多个面板挂不同词条 |
| `set_empty_message(const std::string &)` | 覆盖最终空态提示文本 |
| `set_empty_message_key(const std::string &)` | 换空态提示的词条 key |
| `placeholder()` / `empty_message()` | 读取当前生效文本（便于测试与宿主核对） |

优先级写死在注释里：**文本覆盖 > 按 key 查表 > 兜底字面量**。

### 属性通道补齐

`CommandPalette` 此前只有 `describe_static()`，**没有 `serialize_props` / `deserialize_props`**，
也**未注册进 `WidgetRegistry`**（故不在 `aurora_api.json` 的 73 个 widget 内）。本提案一并补齐前两项：
四个新属性进 `describe_static()` 的属性表，并实现 `serialize_props` 与 `deserialize_props`，
避免造出「描述里有、读写通道没有」的半吊子状态。注册进 `WidgetRegistry` 属另一个独立缺口，不在本提案范围。

### 破坏性说明

**纯新增 + 一处语义收窄，无破坏性变更。** 不删不改任何既有入口签名，既有调用点零改动。
唯一行为变化是：宿主登记了这两个 key 的译文后，占位符与空态提示从恒为英文变为按 locale 出译文——
这正是本提案的目的。未登记译文时输出与改动前**逐字节相同**（兜底串即原字面量）。

## 验收判据

1. `placeholder_defaults_to_lookup_result_or_fallback`：未登记译文时占位符**非空**且等于兜底串
   `"Type a command..."`；用 `ScopedStringTable` 守卫登记中英两条译文后，默认 locale 下等于英文译文、
   `Locale{"zh"}` 经 `ctx` 注入后等于中文译文。
2. `empty_message_defaults_to_lookup_result_or_fallback`：同上，空态提示对应
   `"No matching commands"` / 中文译文，且在 `results()` 为空时确实被用于绘制路径。
3. `custom_text_overrides_lookup`：`set_placeholder` / `set_empty_message` 的覆盖值优先于查表结果，
   且 `placeholder()` / `empty_message()` 读回同一串。
4. `custom_key_still_localizes`：`set_placeholder_key` / `set_empty_message_key` 换成自定义 key 后，
   面板**仍走 i18n**（登记该 key 的译文后输出随之变化，而非恒等于覆盖文本）。

**变异自证**（证明断言有牙齿，非恒真）：

- 把源码里的查表 key 改错一位（如 `command_palette.placeholder` → `command_palette.placeholdr`）
  ⇒ 判据 1 / 2 的「等于译文」段转红——查表落空回退兜底英文，与中文译文不等。
- 解析时机改回构造期固化 ⇒ 判据 1 的「`ctx` 注入 locale 后等于中文译文」段转红。
- 优先级写反（查表 > 文本覆盖）⇒ 判据 3 转红。

## 回写落点

- `codespec/specification/04-widget.md`（`CommandPalette` 控件行：登记两处文案走 i18n 词条表及四个新属性）
- `codespec/specification/07-environment-modifier.md`（§6 国际化：登记 `command_palette.*` 两个词条 key 与
  「控件内置文案的 key 可配」这一宿主用法）
