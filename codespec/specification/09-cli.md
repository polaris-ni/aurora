# 命令行参数解析（cli）

> 覆盖 `include/aurora/cli/args.h`（token → 强类型值）、`include/aurora/cli/command.h`（声明表与派生视图）与实现
> `src/aurora/cli/{args,command}.cpp`；模块私有实现头 `src/aurora/cli/literals.h` 与 `src/aurora/cli/spec_lookup.h`
> （内建注入判定与树内查找，不安装、不进 `aurora_api.json`）。
> 本文件是 argv 语法、字面量词法与 `cli-*` 错误码的**唯一权威**。
> `Result` / `Error` 契约见 [`01-core.md`](01-core.md) §3；工具链可执行文件（`aurora_cli` 等）见
> [`08-tooling.md`](08-tooling.md) §7；强类型 `Length` / `Color` / `LogLevel` 见 [`01-core.md`](01-core.md) §2.2、§5。

---

## 1 模块范围

| 关注点 | 头文件 / 目标 |
|:---|:---|
| 声明表与静态校验 | `cli/command.h`（§3） |
| 词法与语法（argv → token 归类） | `cli/args.h`（§4） |
| 字面量 → 项目强类型 | `cli/args.h`（§5） |
| 失败上报（12 条 `cli-*`） | `cli/args.h`（§6） |
| 派生视图（usage / help / version / schema） | `cli/command.h`（§7） |
| 取值出口（`Arguments` / `Value`） | `cli/args.h`（§8） |
| 入口聚合 | `aurora/aurora.h` 直接 `#include "cli/args.h"` + `"cli/command.h"`，随静态库交付 |

命名空间 `aurora::cli`（推荐 `au::cli`）。本模块**纯逻辑、零渲染、零 I/O**：不读 `std::cin`、不写 stdout、不抛异常，
因此可被 `tools/servers/*`、`tests/framework`、demo 与最终用户程序共用同一份实现。

---

## 2 设计定位与业界对照

对照样本取自 clap 4、Python argparse、cobra、getopt_long、Boost.Program_options、lyt::args 六个主流库的公开文档与用法密度。

| 对照库 | 采纳 | 不采纳（及理由） |
|:---|:---|:---|
| clap 4（derive） | 声明即自描述、`Did you mean` 建议、`--help` 分组两列、`value_hint` 占位符 | derive 宏（本项目无 proc-macro 等价物，且宏会遮蔽可枚举性） |
| argparse | `nargs='+'` 的贪心 span 语义、`choices` 词表、`[default: ...]` 回显 | 前缀缩写（`--wid` → `--width`：静默歧义，AI 生成串不稳） |
| cobra | git 式命令树（选项属于命中链的最深命令）、内建 `--help`/`--version` | 环境变量/配置文件回退链（属配置层职责，见 §4.7） |
| getopt_long | `--name=value`、`-wvalue`、短名集群 `-vf`、`--` 终止符 | `optind`/`optarg` 全局状态、`+`/`-` 排列约定（POSIX 扩展旗标） |
| Boost.Program_options | 强类型值语义 | `notify()` 回调与 `variables_map` 字符串弱类型中转 |
| lyt::args | 编译期/静态校验前置 | lambda 绑定到引用参数（会产生悬垂绑定，且不可序列化） |

五条不可让步的内核：

1. **Schema-first**：唯一的输入是 §3 的声明表；无绑定目标、无回调、无反射。声明表本身可被 `validate` 检查、被
   `schema_json` 导出（需求 #12 / #17）。
2. **零异常**：一切失败经 `Result<T>` + `Error`（`cli-*` slug）返回，`noexcept` 边界内不做分配（见 §5 的
   `value_kind_from_name`）。
3. **强类型打通**：`ValueKind` 的 9 个取值中有 4 个直接落到库内既有类型（`Length` / `Color` / `LogLevel` /
   `Duration`→毫秒 `int64_t`），不存在「先取字符串再手工 parse」的第二套词法。
4. **GNU/POSIX 全集**：§4 列出的形态全部支持，且**只有**这些形态；未列入的（缩写、`+x`、连字符合并长名）一律按
   未知选项处理，行为可枚举、可预测。
5. **派生视图一等公民**：`usage_line` / `help_text` / `version_text` / `schema_json` 全部从同一声明表纯函数派生，
   文本基线由 golden 锁定（§9）；其中三份文本还能被旗标短路成**一等结果**预渲染到 `Invocation::display_text`，
   不与错误码争优先级（§4.6）。

---

## 3 声明表

### 3.1 `CommandSpec`（一条命令 = 树上一个节点）

| 字段 | 类型 | 语义 |
|:---|:---|:---|
| `name` | `std::string` | 命令名；根命令留空时取 `argv[0]` 的 basename（Windows `\` 与 POSIX `/` 都算分隔符） |
| `about` | `std::string` | 一行摘要（help 标题行） |
| `description` | `std::string` | 长说明段落，可空 |
| `options` | `vector<OptionSchema>` | 本命令的选项（**不继承**父命令选项，见 §4.5） |
| `positionals` | `vector<PositionalSchema>` | 位置参数，按数组顺序消费；变长项必须末位 |
| `subcommands` | `vector<CommandSpec>` | 子命令，构成树 |
| `version` | `std::string` | 非空 → 该层**惰性注入**内建 `--version`（§4.6） |
| `epilog` | `std::string` | help 末尾附注 |
| `subcommand_required` | `bool` | `true` → 裸跑父命令报 `cli-missing-subcommand` |
| `builtins` | `Builtins` | 内建 `--help` / `--version` 的注入开关，默认三项全开（§4.6） |

树内查找（按长名 / 短名 / 子命令名，只查**本命令一层**、不含祖先）是解析与渲染的内部设施，
位于私有头 `src/aurora/cli/spec_lookup.h` 的 `detail::` 下，不作为公共出口；外部需要枚举声明时走
`schema_json()`（§7）或直接遍历 `options` / `positionals` / `subcommands` 三个数组。

### 3.2 `OptionSchema`

字段顺序即指定初始化器书写顺序（`CODING_STANDARDS.md` §2 的「具名聚合优先」）：

| 字段 | 类型 | 语义 |
|:---|:---|:---|
| `long_name` | `std::string` | 不含 `--`，kebab-case；必填、命令内唯一。可以叫 `help` / `version`——声明即令同名内建让位（§4.6） |
| `short_name` | `char` | 不含 `-`；`'\0'` 表示无短名。可以占用 `h` / `V`，对应内建自动降级为仅长名（§4.6） |
| `kind` | `ValueKind` | 值类型，决定 §5 的字面量转换 |
| `arity` | `Arity` | 值 token 个数区间；缺省 `{1,1}`，`Bool` 必须 `flag()` |
| `help` | `std::string` | 一行说明 |
| `value_hint` | `string` | 占位符（`FILE`）；空则由 `long_name` 大写推导 |
| `default_text` | `string` | 默认值的**字面量原文**（空串 = 无默认），须能按 `kind` 解析并落在取值域内 |
| `required` | `bool` | 未出现且无默认 → `cli-missing-required` |
| `choices` | `vector<string>` | 词表；`kind == Enum` 时必填非空 |
| `minimum` / `maximum` | `optional<double>` | 数值闭区间（仅 `Int` / `Double` / `Duration` 适用，见 §5） |
| `conflicts_with` | `vector<string>` | 互斥长名，须指向本命令已声明的选项 |
| `group` | `string` | help 分组名；空 = `Options` |
| `hidden` | `bool` | 仍可解析，但不进 help / schema |
| `early_view` | `EarlyView` | 非 `None` → 出现即短路：渲染该视图进 `Invocation::display_text` 并停止业务（§4.6）。必须是 `Bool` + `flag()` |

### 3.3 `PositionalSchema`

`name`（usage 里渲染为 `<NAME>`）、`kind`、`arity`（缺省 `exactly_one()`）、`help`、`default_text`、`choices`。
无 `required` 字段：**arity 即必填性**（`exactly_one` = 必填，`optional_one` / `zero_or_more` = 可选）。

### 3.4 `Arity` 与 `ValueKind`

`Arity{min, max}` 描述**消耗几个值 token**，`max == Arity::AURORA_UNBOUNDED`（`-1`）表示无上界。命名工厂优先于手写聚合，
5 个即覆盖常用形态：`flag()` = `{0,0}`、`exactly_one()` = `{1,1}`、`optional_one()` = `{0,1}`、
`at_least_one()` = `{1,∞}`、`zero_or_more()` = `{0,∞}`。非标准区间（如 `{2,4}`）直接写聚合，不再为它加工厂。
辅助只读出口：`allows_no_value()`（能否作无值 flag）、`fixed_span()`（定长消耗几个 token）、`max_text()`（上界的可读
文本，无界渲染为 `∞`）、`help_placeholder(hint)`（`<VALUE>` / `<VALUE>...` / 空串）。

`ValueKind` 是封闭词表，9 个取值：`Bool` `Int` `Double` `String` `Enum` `Length` `Color` `LogLevel` `Duration`。
可枚举性 API：`all_value_kinds()`（全集）、`to_string(kind)`（`"log-level"` 形态的线名）、
`value_kind_from_name(name)`（`noexcept` 往返，未命中返回空 optional）。三者构成 `--help` / `schema_json` / 错误文案
共用的唯一词表。

### 3.5 `validate(root) -> Result<int>`

静态门禁，不消费 argv；成功返回被检查的命令总数（含根），失败返回首个违规的 `cli-spec-invalid`。`{reason}` 是一条
带**命令路径**的人话原因，形态为 `Invalid command spec: <命令链> <符号>: <毛病>`（如
`aurora_cli render --height: default value is above maximum`），与其余 `cli-*` 的 `command` 参数同口径，深树里可直接定位。
检查项：长名 / 短名 / 位置参数名非空、合法且命令内唯一、位置参数不与选项撞名、`Enum` 有词表、`Bool` 与 arity 一致
（`Bool` 必须 `flag()`、取值型必须 `min >= 1`）、标了 `early_view` 的是零值 arity 的 `Bool`（§4.6）、默认值字面量可按
`kind` 解析且落在词表 / 区间内、`conflicts_with` 指向本层存在的长名、变长位置参数在末位、`subcommand_required` 有子命令、
空声明节点。递归覆盖全部子命令。

内建 `help` / `version` 的长名与短名**不再是检查项**：声明同名即让位（§4.6），故 `validate` 不会拒绝「把 `-h` 用作
`--height`」。

**应用必须在 `main` 早期调用一次**（见 `examples/demos/demo_cli.cpp`）：`parse` 自身不调 `validate`，且对非法默认值
采取「静默跳过物化」策略（不崩溃、不报错），所以跳过这道门禁的调用方会**无声失去**该选项的默认值 —— 声明表错误只有
`validate` 能报出来。

---

## 4 语法（token 归类）

### 4.1 token 分类

按**从左到右**逐 token 归类，无重排：`--` 前是选项区，之后全部原样进 §8 的 `rest()`。判定顺序：

1. `--` 本身 → 其后所有 token 进 `rest()`，并停止位置参数消费（溢出位同样落 `rest()`）。
2. `--name` / `--name=value` → 长名选项。
3. `-abc`（多字符）→ 短名集群，逐字符展开；集群里第一个需要值的短名吞掉其余部分作值（`-vw80` = `-v -w 80`）。
4. `-x` / `-1.5` 中以数字或 `.` 开头的整体 → **数值 token，不是选项**（`--scale -1.5` 成立）。
5. 其余裸 token → 先试精确子命令名（§4.5），再试位置参数槽。

### 4.2 长名

`--width 1024`、`--width=1024` 等价。`Bool` 选项允许 `--force=false` 显式关闭（`true/1/yes` / `false/0/no`，
大小写不敏感，见 §5）；`Bool` 不带值时按出现次数计数（§8 的 `count()`）。

### 4.3 短名

`-w 1024`、`-w1024`、`-w=1024` 三者等价；`-vf`（两个 flag 集群）与 `-vv`（同一 flag 两次，计数 2）成立。
`-h` / `-V` **不是保留名**：默认归内建 help / version，本层声明了同名短名就让位给声明（内建降级为仅长名，§4.6）。

### 4.4 多值与 arity

单值选项（`{1,1}`）重复出现 → `cli-arity-violated`。`max > 1` 或无上界的选项按 **贪心 span** 消费后续裸 token，
直到遇到下一个 `-x` / `--x` 形态的 token 为止（argparse `nargs='+'` 语义）。因此位置参数须写在变长选项**之前**，
或把位置参数放到 `--` 之后。出现次数超过 `max` → `cli-arity-violated`。

### 4.5 命令树

git 式：**选项属于当前所在命令**。

- 进入子命令后，父命令的选项不再可用（`aurora-render --format json render` 合法，
  `aurora-render render --format json` 报 `cli-unknown-option`）。
- 裸 token 若**精确命中**子命令名，优先下钻，即使父命令有变长位置参数槽。
- `command_chain()` 记录命中链（根在前），`matched_command()` 指向叶命令声明。
- 父子声明同名长名时按「**最深显式给出者胜出**」合并；叶层仅有默认值时**不得**抹掉父层用户真写过的取值。
- `subcommand_required == true` 且未下钻 → `cli-missing-subcommand`。

### 4.6 提前展示通道（`EarlyView`）与内建 `--help` / `--version`

「某个旗标出现 → 直接给一份文本、不进业务」是一等**结果**而非错误，用一条通道表达：

```cpp
enum class EarlyView : std::uint8_t { None = 0, Help, Version, Schema };  // 封闭词表，新增一项即多一种出口
```

`parse` 命中该通道时返回**成功**的 `Invocation`：`view` 置为对应项，`display_text` 放已渲染文本，
`shows_display()` 为真；调用方只需 `AURORA_LOG_RAW` 落 stdout 并按 §6.3 退出，不必进业务。
`early_view_to_string(view)` 给线名（`None` 渲染为 `"ok"`），供日志 / schema 消费。

三条渲染规则：`Help` → `help_text(命中层, 命令链)`；`Version` → `version_text(命中层, 程序名)`；
`Schema` → `schema_json(根).dump(2)` 再补一个 `\n`（整棵树，不按命中层截断）。

**入口有两种，走同一条通道**：

1. **调用方自标**：任意 `OptionSchema` 把 `early_view` 设成非 `None`（须是 `flag()` 的 `Bool`，§3.5），
   如 `demo_cli` 的 `--dump-schema`。它与其他旗标一样先按声明表解析，命中即在 `store()` 里短路。
2. **库内建**：`--help` / `--version` 无需声明即存在，但是**惰性注入**的糖 —— 解析、`help_text`、`schema_json`
   三方共用私有头 `src/aurora/cli/spec_lookup.h` 的 `detail::builtin_plan(spec)` 判定实际注入形态，
   杜绝「解析认 `-h` 而帮助文本不列」的三方漂移。

内建的让位与关闭口径（`CommandSpec::builtins`，逐层独立生效）：

| 情形 | 结果 |
|:---|:---|
| 本层声明了长名 `help` / `version` | 对应内建**不存在**（自标 `early_view` 即接管，否则就是个普通旗标） |
| 本层把 `-h` / `-V` 给了别的选项 | 对应内建**降级为仅长名**——帮助文本随之不再谎报短名 |
| `builtins.help = false` / `version = false` | 该层完全不注入（自建 `help` 子命令的场景） |
| `builtins.take_shorts = false` | 注入但不占短名，只认 `--help` / `--version`（Click 口径） |
| 该层 `version` 为空 | 无 `--version` 可注入（`version_text` 也返回空串） |

`-v` 始终留给调用方的 verbose，`-V` 专属版本（与 clap 一致）。`--help` 优先于「待报的必填 / 互斥错误」与
后随的非法字面量——用户要的是说明书，不是报错（`help_beats_pending_required_errors` /
`help_view_beats_a_failing_literal_that_follows_it`）。

### 4.7 明确不支持

前缀缩写、`+x` 旗标、连字符长名的部分匹配、`~` 展开、环境变量 / 配置文件回退、shell 补全脚本生成、
反应式绑定（把值写回 `Signal`）。「不支持」同样是可枚举契约：以上形态一律按未知选项 / 位置参数处理。

---

## 5 字面量（token → 强类型）

转换在 `detail::convert_literal` 单点完成（`args.cpp` 与 `command.cpp` 共用，杜绝「validate 认为合法而 parse 拒收」
的双实现漂移）。失败一律 `cli-invalid-value`，携带 `{option, value, kind}` 与原文。

| `kind` | 接受的写法 | 内部存储 | 备注 |
|:---|:---|:---|:---|
| `Bool` | `true` `1` `yes` / `false` `0` `no`（大小写不敏感） | `bool` | 集群中出现即 `true` |
| `Int` | 十进制整数，可带 `-` | `int64_t` | `as<int>()` 再做 `int` 窄化检查 |
| `Double` | 十进制小数、科学计数、`inf`/`nan` 拒绝（非有限值） | `double` | |
| `String` | 任意原文 | `std::string` | |
| `Enum` | 任意原文，随后按 `choices` **大小写敏感**过滤 | `std::string` | 越界 → `cli-choice-invalid` |
| `Length` | `12` / `12px` / `25%` / `fill` / `match_parent` / `auto` / `wrap` / `wrap_content` | `Length` | 负像素与 `>100%` 拒绝 |
| `Color` | `#rgb` `#rgba` `#rrggbb` `#rrggbbaa` / `rgb(r,g,b)` / `rgba(r,g,b,a)` | `Color` | 通道 >255、位数残缺拒绝 |
| `LogLevel` | 全称 / 三字母短标签 / `warn`+`warning`，共 13 词，大小写不敏感 | `LogLevel` | 与 `Logger` 标签双向可对 |
| `Duration` | `500`（=ms）`250ms` `5s` `2m` `1h` `1d` | `int64_t` 毫秒 | `minimum`/`maximum` 以毫秒计 |

**数值加宽**（跨类型读取，非隐式收窄）：`Int` / `Duration` / `Double` 之间可无损加宽（`int64 → double`）；
`double → int64` 仅在**恰为整值且无损**时放行，有损（小数、超出 `int64` 精确表示范围）一律
`cli-invalid-value`；只有 `int64 → int` 越出 `int` 区间才是 `cli-range-violated`（窄化是本模块唯一的
「范围」错误源）。其余跨类读取（如把 `Color` 当 `int`）返回 `cli-invalid-value`，绝不静默转换。
完整口径与断言见 §8.3。

---

## 6 错误码

### 6.1 目录

12 条，全部 `category = validation`，声明源 `codespec/errors.toml`，全量清单见生成的
[`ERROR_CATALOG.md`](../ERROR_CATALOG.md)。

| slug | 触发 | 关键参数 | 严重度 |
|:---|:---|:---|:---|
| `cli-spec-invalid` | `validate` 检查项任一违规（§3.5） | `reason` | error |
| `cli-unknown-option` | 未声明的 `--x` / `-x`（含下钻后的父级选项） | `option`, `command` | error |
| `cli-unknown-subcommand` | 该层有子命令但裸 token 未命中且位置参数槽已尽 | `subcommand`, `command` | error |
| `cli-missing-subcommand` | `subcommand_required` 且未下钻 | `command` | error |
| `cli-missing-value` | 选项在行尾缺值 | `option` | error |
| `cli-invalid-value` | 字面量不符合 §5 词法，或跨类读取 | `option`, `value`, `kind` | error |
| `cli-choice-invalid` | `Enum` 值不在 `choices` | `option`, `value` | error |
| `cli-range-violated` | 数值越出 `[minimum, maximum]`，或窄化不无损 | `option`, `value`, `min`, `max` | error |
| `cli-arity-violated` | 单值选项重复 / 出现次数超 `max` / 取值时多值 | `option`, `min`, `max`, `actual` | error |
| `cli-missing-required` | `required` 项未出现且无默认（含 `Arguments::get<T>()` 取缺失项） | `option`, `command` | error |
| `cli-too-many-positionals` | 位置参数溢出且无变长槽 | `command`, `value` | error |
| `cli-conflict-violated` | `conflicts_with` 两侧同时生效 | `option`, `conflict` | error |

### 6.2 建议文本

`cli-unknown-option` 与 `cli-unknown-subcommand` 的 `Error::suggestion` 会给出编辑距离最近的候选
（`Did you mean --width?` / `Available: render, serve`），无候选时退化为候选清单。这是 `auto_fixable = true` 的
机器可读依据。

### 6.3 退出码约定（调用方职责）

库本身不 `exit`。约定与 clap / git 对齐，`examples/demos/demo_cli.cpp` 即为参考实现：

| 情形 | 退出码 |
|:---|:---|
| `view == EarlyView::None` 且业务成功 | `0` |
| `shows_display()` 为真（`Help` / `Version` / `Schema`，打印 `display_text` 即返回） | `0` |
| `parse` 返回 `Error` | `2` |
| 应用自身逻辑失败 | `1` |

例外：`tools/verify/` 真机验收探针的用法错误退 `64`（sysexits `EX_USAGE`）而非 `2`——那批探针头注释里的
`2` 已固定表示「环境不可用」，复用会让脚本分不开「旗标写错」与「本机没环境」。理由与实测见 §10.1。

---

## 7 派生视图

全部为声明表的纯函数，同一输入必得同一输出（文本以 `\n` 结尾，便于直写 stdout）。

| API | 输出 | 要点 |
|:---|:---|:---|
| `usage_line(spec, path)` | `usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]` | `path` 为命令链（不含程序名）；`[OPTIONS]` 只在本层真有选项**或**注入了内建时出现（`builtins` 全关且无自有选项的层没有它）；末尾恒带 `[-- ARGS...]`，提示终止符可用 |
| `help_text(spec, path)` | 两列分块说明书 | 顺序：`about` → `usage` → `description` → 各分组 `Options` → `Arguments` → `Commands` → `epilog`；内建 `Help` 组恒末，其成员按 `detail::builtin_plan` 的**实际注入形态**列出（让位后不再有 `-h`，无版本层不列 `--version`）；`hidden` 项不出现；无短名者以 4 空格缩进对齐；右列带 `[range: ...]` / `[possible values: ...]` / `[default: ...]` / `[required]` |
| `version_text(spec, program_name)` | `aurora-render 1.2.3\n` | `spec.version` 为空则返回空串；`program_name` 缺省取 `spec.name` |
| `schema_json(spec)` | `Json` | 机器可读声明（含惰性注入的内建 help/version、arity 的 `min`/`max`、choices、bounds、子命令递归）；条目带 `"early_view"` 键（仅非 `None` 时），AI 可据此分辨短路出口；`hidden` 项不导出 |

`help_text` / `version_text` / `schema_json` 正是 §4.6 三条渲染规则的底层实现：命中 `--help` / `--version` /
自标 `early_view` 的旗标时，`Invocation::display_text` 里放的就是它们之一的产物，调用方只需 `AURORA_LOG_RAW`
落 stdout（`CODING_STANDARDS.md` §4.1）。`usage_line` 不单列通道——它已经是 `help_text` 的第二行。

---

## 8 取值出口

### 8.1 `parse` 两个重载

```cpp
parse(root, const std::vector<std::string> &tokens, program_name = {})
parse(root, int argc, const char *const *argv)   // 自动跳过 argv[0]，程序名取其 basename
```

`tokens` **不含**程序名（调用方自行去掉）；`program_name` 非空时覆盖推导结果（测试与多 call-name 场景）。
`argc <= 0` / `argv == nullptr` 不算错误，回落根声明的 `name`，再空则 `program`。
两个重载是全部入口形态：不再有 `std::span<const std::string_view>` 版本（`vector` 版本可承接同一份数据，
少一个重载就少一处口径漂移）。

### 8.2 `Arguments`

| 出口 | 语义 |
|:---|:---|
| `get<T>(long)` | 强类型取**单值**：缺失 → `cli-missing-required`，多值 → `cli-arity-violated`，跨类读取失败 → §5 口径 |
| `values(long)` | 全部出现值（按出现顺序）；未出现且无默认 → 空表，**不算错误** |
| `flag(long)` | Bool 是否生效（含 `[--flag=false]` 显式关闭）；未声明的长名返回 `false` |
| `count(long)` | 出现次数（`-v -v -v` → 3）；默认值不计 |
| `explicitly_given(long)` | 区分「用户真写过」与「默认值物化」 |
| `positional(i)` / `positionals()` | 位置参数（含默认值物化后的完整序列） |
| `rest()` | `--` 之后的原始 token，未经任何转换 |
| `command_chain()` / `command_display()` / `matched_command()` | 命令链信息（根在前 / 空格拼接的调用名 / 叶声明） |

**默认值在解析结束时物化进槽位**，故 `get<int>("width")` 即使用户没给也成功；要区分来源用 `explicitly_given()`。
单值读取没有独立出口（原 `value()` 已收进私有 `one()`，只服务 `get<T>()`）——「先拿到 `Value` 再决定怎么读」
必然诱导二次 `as<>` 猜测，强类型出口一步到位。程序名不在 `Arguments` 上暴露，它只属于渲染层（`version_text` 的入参）。

### 8.3 `Value`

三个公共出口，别无其他：

| 出口 | 语义 |
|:---|:---|
| `kind()` | 该值由哪个 `ValueKind` 转换而来 |
| `raw_text()` | **永远可得**的 token 原文，供错误回显与脚本消费 |
| `as<T>()` | 唯一的强类型取值，`T` ∈ {`bool`, `int`, `std::int64_t`, `double`, `std::string`, `Length`, `Color`, `LogLevel`}，其余编译期 `static_assert` 拒绝 |

跨读规则（§5 的「数值加宽」在取值侧的完整表述，由 `named_value_accessors_cover_the_public_entry_directly` 钉住）：
无损加宽可取（`Int`/`Duration` → `double`）；`double` → 整型只在**恰为整值**时放行，否则 `cli-invalid-value`；
类型不符一律 `cli-invalid-value`；只有 `int64 → int` 的窄化越界才是 `cli-range-violated`。
`ValueKind::Duration` 没有对应 C++ 类型，用 `as<std::int64_t>()` 取毫秒。

### 8.4 生命周期契约

`Invocation` / `Arguments` 以**指针**借用声明表，不拷贝 `CommandSpec`、不持有用户变量。因此声明表必须比
`Invocation` 活得久（全局 / `static` / 同作用域栈对象）；把 `Invocation` 存进比声明表更久的容器是未定义行为。
`Value` 是纯值类型，可自由拷贝。

### 8.5 常见误用

- 用 `get<T>()` 读可重复选项 → `cli-arity-violated`；改 `values()` + `count()`（`demo_cli.cpp` 的
  `report_option` 是正例）。
- 想「保留 `-h` 又不声明选项」而给内建 `--help` 改名 → 用 `builtins`（§4.6），不必再猜哪个短名被占用。
- 把「默认值」当「用户输入」 → 用 `explicitly_given()` 判。
- 在循环里 `as<int>()` 反复吞错误 → 解析期已成功，取值失败只可能是 §5 的跨类读取，应作为编程错误直接上报。

---

## 9 测试与验收

| 层 | 位置 | 覆盖 |
|:---|:---|:---|
| 单元（语法与取值） | `tests/unit/utest_cli.cpp` | §4 全部语法形态、§5 字面量、§6 错误码、命令链合并 |
| 单元（校验与派生文本） | `tests/unit/utest_cli_format.cpp` | §3.5 `validate` 各检查项、§7 四个视图、文本 golden |
| 共享夹具 | `tests/support/cli_fixture.h` | 三棵共用树：宽松树 `spec()`（覆盖全部 `ValueKind` / arity / 取值域 / 互斥 / 子命令 / hidden）、严格树 `strict_spec()`（必填 + 强制子命令）、让位树 `displacement_spec()`（`-h`/`-V` 被自有选项占用 + 自标 `early_view` 的 `--help`/`--dump-schema` + `builtins` 全关的子命令）。两个单元文件同树，保证断言口径一致 |
| Golden | `tests/golden/cli_snapshots.json` | 7 段文本 + `schema`；`AURORA_UPDATE_GOLDEN=1` 经 `aurora_test_runner --run=utest_cli_format` 重生成，勿手改 |
| 集成 | `tests/integration/itest_cli.cpp` | `tools/servers/aurora_cli.cpp` 端到端（真实 argv、退出码） |
| 示例 | `examples/demos/demo_cli.cpp` | `validate` → `parse` → 取值 → 错误回显的完整载体，含退出码约定 |

跑法：`ctest --preset ninja-test -R cli`（三个 cli 相关条目）。

---

## 10 需求规格

### 10.1 #17 LSP / MCP Server / CLI 工具链

本模块是 #17 的**共用底座**：仓库内一切「人敲进来的 argv」都收敛到同一份声明表驱动的解析器，从而保证
`--help` 文本、`schema_json` 与错误 slug 跨工具一致。已接入的载体：

| 载体 | 接入形态 |
|:---|:---|
| `tools/servers/aurora_cli.cpp` | 10 个子命令的 `CommandSpec` 树（`build_spec()`），`subcommand_required` |
| `tools/gen/gen_api.cpp`（`gen_api_tools`） | 单位置参数 `<OUT>`，`default_text = "-"` 表达「缺省写 stdout」 |
| `tools/bench/bench_scroll.cpp` | 场景 / 格式用 `ValueKind::Enum` + `choices` 承载词表 |
| `examples/demos/demo_core.cpp` | `--level` 用 `ValueKind::LogLevel` 直取 `au::LogLevel`，`--strict` 用 flag |
| `tests/framework/test_main.cpp` | runner 全部旗标（`--run=` / `--filter=` / `--shuffle[=<seed>]` / `--selftest` / 死亡测试内部旗标 `hidden`）|
| `examples/demos/demo_cli.cpp` | 本模块自身的示例载体（`validate` → `parse` → 取值 → 错误回显），`--dump-schema` 自标 `early_view = Schema` 演示短路通道 |
| `tools/verify/*_live_probe.cpp`（14 处探针） | 经共用入口 `tools/verify/verify_args.h` 的 `aurora_verify::parse_interactive()`，各探针只声明 `--interactive` 与自身专属旗标 |

`aurora_lsp` / `aurora_mcp` 只走 stdio 线协议、不消费 argv，故无需接入。两处**故意不接入**：
`gen_error_codes` 与 `gen_debug_api` 是错误码 / debug 门面头自身的生产者，按「先有生成物才链得上库」的
鸡生蛋约束刻意不链接 `aurora`（见 [`08-tooling.md`](08-tooling.md) §7.4），其入口保持手写 argv 读取。

探针侧的共用入口 `tools/verify/verify_args.h`（header-only，不入库、不入 `aurora_api.json`）把「帮助 / 版本 /
用法错误」三类出口统一在一处：`--help` 与 `--version` 打印后退 `0`，未知或非法旗标按 `cli-*` 口径报
`消息 — 建议` + `Run with --help for the accepted flags.` 后退 `64`，只有真正拿到 `Arguments` 才进入探针主体。

探针侧用法错误**刻意不复用**工具链惯用的 `2`：14 份探针头注释的退出码表里 `2` 早已表示
「环境不可用」（无 DISPLAY / 无合成器 / 建窗失败），复用会让脚本分不开「旗标写错」与「本机没环境」
（前者是人的失误、后者应记 SKIP），故取 sysexits 的 `EX_USAGE = 64`，与各探针既有的 0/1/3/4/5/6/7 全不重叠。
工具链侧（`aurora_cli` / 测试 runner / `bench_scroll`）仍按 §6.3 的 `0/1/2` 约定，实测 `--bogus` 退 `2`。
相比替换前的手写循环，行为差异是**拼错的旗标不再被静默忽略**。⚠️ `tools/verify/` 随平台条件构建
（`AURORA_BUILD_VERIFY_TOOLS` 默认 OFF）且**不进 CTest**，无头 CI 无法守住它，改动前须按各探针所属平台实机复验。

短名归属由**让位规则**决定（§4.6），不再有「内建保留名」：`aurora_cli` 的 `render` 把 `-h` 声明为
`--height`，该层内建 help 于是降级为仅 `--help`，实测 `aurora_cli render -h 300` 收的是高度值、
`aurora_cli render --help` 的 `Help:` 组只列长名。根层无此冲突，仍显示 `-h, --help` / `-V, --version`。
相关人工用例同步用 `-h` / `--height`（[`manual-test/06-render.md`](../manual-test/06-render.md)）。
工具链可执行文件清单见 [`08-tooling.md`](08-tooling.md) §7.4。


### 10.2 #12 机器可读 API Schema

`schema_json()` 让命令树本身可被 AI 工具链消费（选项、arity、词表、区间、必填、子命令、内建注入形态与
`early_view` 短路出口），与 `aurora_api.json` 的 UI Schema 属同一「自描述」原则；`EarlyView::Schema`
（`demo_cli --dump-schema`）即其演示载体——导出的 schema 里能读回每个出口，不需要额外约定。

### 10.3 #4 强类型 + 单位标注

`ValueKind` 与 `Length` / `Color` / `LogLevel` / 毫秒 `Duration` 的打通，使 CLI 层不再出现「字符串 + 手工
`stoi`」的第二套词法，越界与跨类读取在 `Result` 上显式可见。
