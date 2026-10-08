# CHG-007 快捷键可重绑的两个框架入口（组合键文本反解 + 命令绑定 setter）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-007 |
| 提出日期 | 2026-10-08 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `event/keycode.h`（键名反查）、`app/shortcuts.h`（组合键文本反解）、`commands.h` / `src/aurora/commands.cpp`（命令绑定 setter） |

> 目录名说明：本仓提案编号按目录名字典序升序分配且**不重排**（见 `codespec/changes/README.md` §3.1），
> 字典序末位现为 `widgets-command-palette-i18n`（CHG-006）。故本提案沿用 `widgets-` 前缀取
> `widgets-shortcut-rebind-entries`，排在其后取得 CHG-007，不动任何既有编号与交叉引用。

## 动机

**框架已具备快捷键的存储、匹配、注册表三条腿，唯独缺「启动时按外部覆盖表重放绑定」的入口。**

| 腿 | 位置 | 现状 |
|:--|:--|:--|
| 存储 | `Command::default_binding`（`std::optional<KeyCombo>`） | 一条命令的绑定源在此 |
| 匹配 | `KeyCombo::matches(KeyEvent)` | 只比可按住的修饰位（见 `06-app-platform.md` §8.4） |
| 注册表 | `ShortcutRegistry` + `CommandRegistry::bind_shortcuts` | 后者把 `default_binding` **逐条投射**进快捷键表 |
| **重放** | —— | **缺**：无「改绑定而不动元数据」的 setter，也无「文本 → 键位」的反解 |

`KeyCombo::to_string()` 是**单向**的：宿主能把组合键渲染成 `"Ctrl+Shift+P"` 给用户看，却无法把用户改完的
那串文本读回 `KeyCombo`。宿主若自己写一份反解，就得自己维护一份键名表——那张表与 `key_name`
（`event/keycode.h`）**同源却分叉**，新增键位时两边漂移，且框架侧无任何门禁能发现；这等于在宿主侧
自造第二真值源。

`CommandRegistry` 同侧的缺口对称：改一条命令的绑定，目前只有 `add(Command)` 一条路，而它是
**整条覆盖**语义（同 `id` 覆盖旧定义）。宿主想「只换快捷键」就得把 `title` / `icon` / `action` /
`enabled` 谓词 / `when_label` 全部重述一遍——漏抄一个字段就是静默的能力丢失（`enabled` 谓词尤其
无法从外部读出后重述，因为它是 `std::function<bool()>`，**取得到、复制不了语义**）。

于是「快捷键可重绑」这件在宿主侧只差一腿的事，在框架侧卡在两个入口上：文本 → `KeyCombo`，
以及已注册命令的绑定改写。二者任缺其一，宿主的重放一腿即通不了。

## 变更内容

### 入口 1：`key_code_from_name`（`event/keycode.h`，与 `key_name` 同源）

```cpp
[[nodiscard]] auto key_code_from_name(std::string_view name) -> std::optional<KeyCode>;
```

反查**不另列新表**：遍历 `KeyCode` 的取值空间、取 `key_name()` 的输出逐个比对，命中即返回该键码。
这样新增键位只需改 `key_name` 一处，反查自动跟上，不存在第二张表可漂移。未命中（含 `Unknown`
这一占位键名）返回 `std::nullopt`。

`Unknown` 之外的取舍：`Unknown` 是「后端映射表未收录」的占位值，**不是可绑定的键名**，
故反查不接受它——与「解析失败不得回落假有效值」同口径，避免宿主把「解析不出来」当成
「绑定到 Unknown 键」。

### 入口 2：`KeyCombo::from`（`app/shortcuts.h`）

```cpp
[[nodiscard]] static auto from(std::string_view text) -> std::optional<KeyCombo>;
```

- 解析形态**就是 `to_string()` 目前输出的那些**（`"Ctrl+Shift+P"` / `"Alt+Tab"` / `"F5"`），
  不额外支持别的写法——不形成第二显示串规范。修饰次序**不要求**与 `to_string()` 同序
  （`"Shift+Ctrl+P"` 与 `"Ctrl+Shift+P"` 同解），但产出走同一份修饰位与主键。
- 畸形输入一律 `std::nullopt`：空串、只有分隔符、未知键名、`"Ctrl+"` 尾巴、修饰位重复、
  非 ASCII 主键名——**绝不**回落成「无修饰 + 空主键」之类的假有效值。
- 修饰位只认 `to_string()` 输出的那四个字面量（`Ctrl` / `Shift` / `Alt` / `Meta`）；
  不认 `Control` 等键名形态（那是主键名，不是修饰位字面量）。

### 入口 3：`CommandRegistry::set_binding`（`commands.h` / `src/aurora/commands.cpp`）

```cpp
auto set_binding(const std::string &id, KeyCombo combo) -> bool;
```

只改指定 `id` 那条命令的 `default_binding`，**不动** `title` / `icon` / `action` / `category` /
`scope` / `enabled` 谓词 / `when_label`，也不改变注册次序；`id` 不存在返回 `false`。
已 `bind_shortcuts` 过时连带同步该条在 `ShortcutRegistry` 里的绑定（先撤后建），
避免两个投影各自持有不同的绑定真值；未投影过则纯数据变更，由下一次 `bind_shortcuts` 生效。

### 破坏性说明

纯增量：不改名、不改签名、不改既有派发路径，`to_string()` / `matches()` / `bind_shortcuts()` 语义逐位不变。

## 验收判据

1. **往返等值**：对 `key_name` 覆盖的全部键名与 `Ctrl` / `Shift` / `Alt` / `Meta` 的全部修饰子集，
   `KeyCombo::from(combo.to_string())` 与原始 `combo` **逐位相等**（修饰位、主键），且
   `combo.matches(自身构造的 KeyEvent)` 为真。
2. **畸形输入拒绝**：空串 / 只有分隔符 / 未知键名 / `"Ctrl+"` 尾巴 / 修饰位重复 / 非 ASCII 主键名，
   各类**均**回 `std::nullopt`，无一类静默产出「Ctrl + 空主键」式假有效值。
3. **绑定 setter 不动元数据**：`set_binding(id, combo)` 之后 `find(id)` 给出的 `Command`，
   除 `default_binding` 外**逐字段与调用前相等**（`title` / `icon` / `category` / `scope` /
   `action` 可执行性 / `enabled` 谓词求值结果 / `when_label`），`all()` 的注册次序不变；
   未注册 `id` 返回 `false` 且不入表。
4. **覆盖表重放**：注册表 + 覆盖表（`command id → 组合键文本`）混合灌入 `ShortcutRegistry` 后，
   最终 dispatch 语义按覆盖表生效——被覆盖的命令用新组合键触发、未被覆盖的仍用 `default_binding`，
   畸形覆盖条目不影响其余条目。

## 回写落点

- `codespec/specification/06-app-platform.md` §8.4（补「快捷键可重绑」段：`set_binding` 的重放口径、`KeyCombo::from` 的往返契约与畸形拒绝口径）
- `codespec/specification/05-event-navigation.md` §2.2（补「键名反查」段：`key_code_from_name` 与 `key_name` 的同源关系、`Unknown` 不予接受）
