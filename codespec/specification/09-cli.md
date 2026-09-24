# 命令行参数解析（cli）

> 覆盖 `include/aurora/cli/args.h`（token → 强类型值）、`include/aurora/cli/command.h`（声明表与派生视图）与实现
> `src/aurora/cli/{args,command}.cpp`；模块私有实现头 `src/aurora/cli/literals.h`（不安装、不进 `aurora_api.json`）。
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
   文本基线由 golden 锁定（§9）。

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
| `version` | `std::string` | 非空 → 该层自动支持 `--version`（§4.6） |
| `epilog` | `std::string` | help 末尾附注 |
| `subcommand_required` | `bool` | `true` → 裸跑父命令报 `cli-missing-subcommand` |

查询成员：`find_option(long_name)` / `find_short(c)` / `find_subcommand(name)`，未命中一律返回 `nullptr`（不报错）。
三者是**本命令一层**的查找，不含祖先。

### 3.2 `OptionSchema`

字段顺序即指定初始化器书写顺序（`CODING_STANDARDS.md` §2 的「具名聚合优先」）：

| 字段 | 类型 | 语义 |
|:---|:---|:---|
| `long_name` | `std::string` | 不含 `--`，kebab-case；必填、命令内唯一、不得为 `help`/`version` |
| `short_name` | `char` | 不含 `-`；`'\0'` 表示无短名；`'h'`/`'V'` 被内建 help/version 占用 |
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

### 3.3 `PositionalSchema`

`name`（usage 里渲染为 `<NAME>`）、`kind`、`arity`（缺省 `exactly_one()`）、`help`、`default_text`、`choices`。
无 `required` 字段：**arity 即必填性**（`exactly_one` = 必填，`optional_one` / `zero_or_more` = 可选）。

### 3.4 `Arity` 与 `ValueKind`

`Arity{min, max}` 描述**消耗几个值 token**，`max == Arity::AURORA_UNBOUNDED`（`-1`）表示无上界。命名工厂优先于手写聚合：
`flag()` = `{0,0}`、`exactly_one()` = `{1,1}`、`optional_one()` = `{0,1}`、`at_least_one()` = `{1,∞}`、
`zero_or_more()` = `{0,∞}`、`exactly(n)`、`at_most(n)`。

`ValueKind` 是封闭词表，9 个取值：`Bool` `Int` `Double` `String` `Enum` `Length` `Color` `LogLevel` `Duration`。
可枚举性 API：`all_value_kinds()`（全集）、`to_string(kind)`（`"log-level"` 形态的线名）、
`value_kind_from_name(name)`（`noexcept` 往返，未命中返回空 optional）。三者构成 `--help` / `schema_json` / 错误文案
共用的唯一词表。

### 3.5 `validate(root) -> Result<int>`

静态门禁，不消费 argv；成功返回被检查的命令总数（含根），失败返回首个违规的 `cli-spec-invalid`（`{reason}` 携带
人话原因）。检查项：名称非空 / 合法 / 唯一、内建名未被占用、`-h` / `-V` 未被占用、`Enum` 有词表、`Bool` 与 arity 一致、
默认值可解析且落在域内、`conflicts_with` 指向存在的长名、变长位置参数在末位、`subcommand_required` 有子命令。
递归覆盖全部子命令。**应用应在 `main` 早期调用一次**（见 `examples/demos/demo_cli.cpp`），把声明表的拼写错误挡在
解析之前；`parse` 对非法默认值采取「静默跳过物化」策略，因此不依赖 `validate` 也不会崩溃，但会失去提示。

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
`-h` / `-V` 保留给内建 help / version，声明即 `cli-spec-invalid`。

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

### 4.6 内建 `--help` / `--version`

`-h` / `--help` 在**每一层**都可用；`--version` / `-V` 仅在该层 `version` 非空时存在（大小写与 clap 一致：
`-v` 留给调用方的 verbose，`-V` 专属版本）。二者是**结果**而非错误：
`Invocation::outcome` 取 `ParseOutcome::Help` / `Version`，预渲染文本在 `Invocation::display_text`。
`--help` 优先于「待报的必填/互斥错误」——用户要的是说明书，不是报错（`help_beats_pending_required_errors`）。
调用方负责打印并决定退出码（约定 §6.3）。

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
| `Int` | 十进制整数，可带 `-` | `int64_t` | `as_int()` 再做 `int` 窄化检查 |
| `Double` | 十进制小数、科学计数、`inf`/`nan` 拒绝（非有限值） | `double` | |
| `String` | 任意原文 | `std::string` | |
| `Enum` | 任意原文，随后按 `choices` **大小写敏感**过滤 | `std::string` | 越界 → `cli-choice-invalid` |
| `Length` | `12` / `12px` / `25%` / `fill` / `match_parent` / `auto` / `wrap` / `wrap_content` | `Length` | 负像素与 `>100%` 拒绝 |
| `Color` | `#rgb` `#rgba` `#rrggbb` `#rrggbbaa` / `rgb(r,g,b)` / `rgba(r,g,b,a)` | `Color` | 通道 >255、位数残缺拒绝 |
| `LogLevel` | 全称 / 三字母短标签 / `warn`+`warning`，共 13 词，大小写不敏感 | `LogLevel` | 与 `Logger` 标签双向可对 |
| `Duration` | `500`（=ms）`250ms` `5s` `2m` `1h` `1d` | `int64_t` 毫秒 | `minimum`/`maximum` 以毫秒计 |

**数值加宽**（跨类型读取，非隐式收窄）：`Int` ↔ `Double` 可互相加宽读取（`int64 → double` 无损、
`double → int64` 仅在**恰为整值**时放行，否则 `cli-range-violated`）；`as_int()` 越出 `int` 区间同样
`cli-range-violated`。其余跨类读取（如把 `Color` 当 `int`）返回 `cli-invalid-value`，绝不静默转换。

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
| `cli-missing-required` | `required` 项未出现且无默认（含 `Arguments::value()` 取缺失项） | `option`, `command` | error |
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
| `outcome == Ok` | `0` |
| `outcome == Help` / `Version`（打印 `display_text`） | `0` |
| `parse` 返回 `Error` | `2` |
| 应用自身逻辑失败 | `1` |

例外：`tools/verify/` 真机验收探针的用法错误退 `64`（sysexits `EX_USAGE`）而非 `2`——那批探针头注释里的
`2` 已固定表示「环境不可用」，复用会让脚本分不开「旗标写错」与「本机没环境」。理由与实测见 §10.1。

---

## 7 派生视图

全部为声明表的纯函数，同一输入必得同一输出（文本以 `\n` 结尾，便于直写 stdout）。

| API | 输出 | 要点 |
|:---|:---|:---|
| `usage_line(spec, path)` | `usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]` | `path` 为命令链（不含程序名） |
| `help_text(spec, path)` | 两列分块说明书 | 顺序：`about` → `usage` → `description` → 各分组 `Options` → `Arguments` → `Commands` → `epilog`；内建 `Help` 组恒末；`hidden` 项不出现；无短名者以 4 空格缩进对齐；右列带 `[range: ...]` / `[possible values: ...]` / `[default: ...]` / `[required]` |
| `version_text(spec, program_name)` | `aurora-render 1.2.3\n` | `spec.version` 为空则返回空串；`program_name` 缺省取 `spec.name` |
| `schema_json(spec)` | `Json` | 机器可读声明（含内建 help/version、arity 的 `min`/`max`、choices、bounds、子命令递归）；`hidden` 项不导出 |

`help_text` / `version_text` / `schema_json` 三个入口在 `--help`、`--version`、`--dump-schema` 路径上被
`Invocation::display_text` 预渲染，调用方只需 `AURORA_LOG_RAW` 落 stdout（`CODING_STANDARDS.md` §4.1）。

---

## 8 取值出口

### 8.1 `parse` 三个重载

```cpp
parse(root, std::span<const std::string_view>, program_name = {})
parse(root, const std::vector<std::string> &, program_name = {})
parse(root, int argc, const char *const *argv)   // 程序名取 argv[0] 的 basename
```

`program_name` 非空时覆盖推导结果（测试与多-call-name 场景）。`argc <= 0` / `argv == nullptr` 不算错误，回落根声明的
`name`。

### 8.2 `Arguments`

| 出口 | 语义 |
|:---|:---|
| `value(long)` | 单值；缺失 → `cli-missing-required`，多值 → `cli-arity-violated` |
| `values(long)` | 全部出现值（按出现顺序）；未出现且无默认 → 空表，**不算错误** |
| `get<T>(long)` | 等价 `value(long).as<T>()` |
| `flag(long)` | Bool 是否生效（含 `[--flag=false]` 显式关闭）；未声明的长名返回 `false` |
| `count(long)` | 出现次数（`-v -v -v` → 3）；默认值不计 |
| `explicitly_given(long)` | 区分「用户真写过」与「默认值物化」 |
| `positional(i)` / `positionals()` | 位置参数（含默认值物化后的完整序列） |
| `rest()` | `--` 之后的原始 token，未经任何转换 |
| `command_chain()` / `command_display()` / `matched_command()` / `program_name()` | 命令链信息 |

**默认值在解析结束时物化进槽位**，故 `value("width")` 即使用户没给也成功；要区分来源用 `explicitly_given()`。

### 8.3 `Value`

`kind()`（哪个变体分支）、`has_value()`、`raw()`（变体只读访问）、`raw_text()`（**永远可得的原文**，供错误回显与
脚本消费）、`as_bool/int/int64/double/string/length/color/log_level/duration_ms()`，以及 `get<T>` 风格的
`as<T>()`（`T` 不在支持集合内即编译期 `static_assert` 拒绝）。

### 8.4 生命周期契约

`Invocation` / `Arguments` 以**指针**借用声明表，不拷贝 `CommandSpec`、不持有用户变量。因此声明表必须比
`Invocation` 活得久（全局 / `static` / 同作用域栈对象）；把 `Invocation` 存进比声明表更久的容器是未定义行为。
`Value` 是纯值类型，可自由拷贝。

### 8.5 常见误用

- 用 `value()` 读可重复选项 → `cli-arity-violated`；改 `values()` + `count()`（`demo_cli.cpp` 的
  `report_option` 是正例）。
- 把「默认值」当「用户输入」 → 用 `explicitly_given()` 判。
- 在循环里 `as<int>()` 反复吞错误 → 解析期已成功，取值失败只可能是 §5 的跨类读取，应作为编程错误直接上报。

---

## 9 测试与验收

| 层 | 位置 | 覆盖 |
|:---|:---|:---|
| 单元（语法与取值） | `tests/unit/utest_cli.cpp` | §4 全部语法形态、§5 字面量、§6 错误码、命令链合并 |
| 单元（校验与派生文本） | `tests/unit/utest_cli_format.cpp` | §3.5 `validate` 各检查项、§7 四个视图、文本 golden |
| 共享夹具 | `tests/support/cli_fixture.h` | 宽松树 `spec()`（覆盖全部 `ValueKind` / arity / 取值域 / 互斥 / 子命令 / hidden）与严格树 `strict_spec()`（必填 + 强制子命令）；两文件同树，保证断言口径一致 |
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
| `examples/demos/demo_cli.cpp` | 本模块自身的示例载体（`validate` → `parse` → 取值 → 错误回显）|
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

接入的副作用是**短名让位**：`-h` / `-V` 为内建 help / version 保留，故 `aurora_cli` 的高度短名由 `-h` 改为
`-H`（长名 `--height` 不变），相关人工用例已同步（[`manual-test/06-render.md`](../manual-test/06-render.md)）。
工具链可执行文件清单见 [`08-tooling.md`](08-tooling.md) §7.4。


### 10.2 #12 机器可读 API Schema

`schema_json()` 让命令树本身可被 AI 工具链消费（选项、arity、词表、区间、必填、子命令），与 `aurora_api.json`
的 UI Schema 属同一「自描述」原则；`--dump-schema` 路径即其演示载体。

### 10.3 #4 强类型 + 单位标注

`ValueKind` 与 `Length` / `Color` / `LogLevel` / 毫秒 `Duration` 的打通，使 CLI 层不再出现「字符串 + 手工
`stoi`」的第二套词法，越界与跨类读取在 `Result` 上显式可见。
