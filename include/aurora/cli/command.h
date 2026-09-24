#pragma once

// ============================================================================
// command.h — 命令/选项声明表与其派生视图（需求规格：specification/09-cli.md §3、§7）
// ----------------------------------------------------------------------------
// Schema-first 的「声明」那一半：调用方写一棵不可变的 `CommandSpec`（选项表 + 位置参数
// 表 + 子命令），同一份声明同时驱动 解析 / 帮助文本 / 用法行 / JSON schema —— 对标 clap
// 的「单一声明源派生一切」，刻意避开 CLI11 的 `add_option(&var)` 引用耦合与 argparse 的
// 字段名反射（二者都让 schema 不可枚举）。
//
// 变量绑定与回调一律没有：解析结果按长名从 `aurora::cli::Arguments` 取值（见 args.h）。
// ============================================================================

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/core/result.h"

namespace aurora::cli {

/// @brief 库内 JSON 别名（同 `aurora::storage::Json` 做法，不依赖 widget 头以保持低耦合）。
using Json = nlohmann::json;

/**
 * @brief 单个选项的自描述声明（长名 / 短名 / 类型 / arity / 取值域 / 默认值）。
 *
 * 用指定初始化器书写即成「声明表」：
 * @code
 * OptionSchema{
 *     .long_name = "width", .short_name = 'w', .kind = ValueKind::Int,
 *     .help = "输出宽度", .default_text = "800", .minimum = 1, .maximum = 8192,
 * }
 * @endcode
 * @note Thread: thread-safe（纯值类型）
 * @note Side-effects: none
 */
struct OptionSchema {
    std::string long_name;  ///< 长名，不含 `--`，kebab-case（如 "output-dir"）；必填且命令内唯一
    char short_name = '\0';  ///< 短名（不含 `-`）；'\0' 表示无短名；`h`/`V` 为内建保留
    ValueKind kind = ValueKind::String;  ///< 值类型（决定字面量转换）
    Arity arity{};  ///< 值个数区间；缺省 `{1,1}`（单值选项），Bool 类应为 `flag()`
    std::string help;  ///< 一行说明，出现在 --help
    std::string value_hint;  ///< 占位符名（如 "DIR"）；空则由 long_name 大写推导
    std::string default_text;  ///< 默认值的**字面量**（空串 = 无默认）；帮助里以 `[default: ...]` 呈现
    bool required = false;  ///< 必填：未出现且无默认值 → `cli-missing-required`
    std::vector<std::string> choices;  ///< 取值域词表（`kind == Enum` 时必填且非空）
    std::optional<double> minimum;  ///< 数值闭区间下界（Int/Double/Duration 适用）
    std::optional<double> maximum;  ///< 数值闭区间上界
    std::vector<std::string> conflicts_with;  ///< 互斥选项长名列表
    std::string group;  ///< 帮助文本分组名；空 = "Options"（末组固定为 "Options"/"Help"）
    bool hidden = false;  ///< 隐藏：仍可解析，但不出现在 --help 与 schema_json
};

/// @brief 位置参数声明（按数组顺序消费 token；变长项必须置于末位）。
struct PositionalSchema {
    std::string name;  ///< 显示名（usage 里以 `<NAME>` 呈现）；必填且命令内唯一
    ValueKind kind = ValueKind::String;
    Arity arity = Arity::exactly_one();
    std::string help;
    std::string default_text;  ///< 缺省字面量（该槽未被消费时物化）
    std::vector<std::string> choices;  ///< 取值域词表
};

/**
 * @brief 一条命令（可含子命令，构成一棵树）的完整声明。
 *
 * `--help` / `-h` 与 `--version` / `-V` 为库内建，无需也不得声明（短名 `h`/`V` 一经占用，
 * `validate` 即报 `cli-spec-invalid`）；`version` 非空则该层自动支持 `--version` / `-V`。
 */
struct CommandSpec {
    std::string name;  ///< 命令名；根命令留空则取 argv[0] 的 basename
    std::string about;  ///< 一行摘要（help 标题下的第一行）
    std::string description;  ///< 长说明（help 正文段落，可空）
    std::vector<OptionSchema> options;
    std::vector<PositionalSchema> positionals;
    std::vector<CommandSpec> subcommands;
    std::string version;  ///< 非空 → 内建 `--version` 输出该文本
    std::string epilog;  ///< help 末尾附注（可多行）
    bool subcommand_required = false;  ///< 是否禁止裸跑父命令（要求存在子命令）

    /// @brief 按长名查本命令声明的选项（内建 help/version 不在其列）；未找到返回 nullptr。
    [[nodiscard]] auto find_option(std::string_view long_name) const -> const OptionSchema *;
    /// @brief 按短名查本命令声明的选项；未找到返回 nullptr。
    [[nodiscard]] auto find_short(char short_name) const -> const OptionSchema *;
    /// @brief 按名查直接子命令；未找到返回 nullptr。
    [[nodiscard]] auto find_subcommand(std::string_view sub_name) const -> const CommandSpec *;
};

// ---------------------------------------------------------------- 校验

/**
 * @brief 静态校验一棵命令树（不解析 argv），启动期一次性挡住声明表里的拼写/形态错误。
 *
 * 检查项：长/短名与位置参数名非空、合法且命令内唯一、不得占用内建长名 `help`/`version` 或内建
 * 短名 `-h`/`-V`；
 * `Enum` 必须有词表且默认值落在词表内；`Bool` 必须是零值 arity；非 `Bool` 的 `min >= 1`；
 * 默认值字面量可按 `kind` 解析且落在取值域内；`conflicts_with` 指向已存在的长名；
 * 变长位置参数不得非末位；`subcommand_required` 要求存在子命令。递归覆盖全部子命令。
 *
 * @return 成功返回被检查的命令总数（含根）；失败返回首个违规的 `cli-spec-invalid`。
 */
[[nodiscard]] auto validate(const CommandSpec &root) -> Result<int>;

// ---------------------------------------------------------------- 派生视图

/**
 * @brief usage 单行：`prog render [OPTIONS] <FILE> [-- ARGS...]`（不含换行）。
 * @param spec 目标命令。
 * @param path 命令链（根在前，如 {"aurora_cli","render"}）；空则只用 spec.name。
 */
[[nodiscard]] auto usage_line(const CommandSpec &spec, const std::vector<std::string> &path = {}) -> std::string;

/**
 * @brief 渲染帮助文本（两列对齐、分组、占位符/取值域/默认值、子命令清单、epilog）。
 *
 * 纯函数：不写任何流。调用方用 `AURORA_LOG_RAW` 输出到 stdout。
 */
[[nodiscard]] auto help_text(const CommandSpec &spec, const std::vector<std::string> &path = {}) -> std::string;

/// @brief `--version` 文本：`<program> <version>\n`；未声明 version 时返回空串。
[[nodiscard]] auto version_text(const CommandSpec &spec, std::string_view program_name = {}) -> std::string;

/**
 * @brief 命令树 → JSON schema（自描述发现：可直接喂 MCP `tools/list` 与 LSP）。
 *
 * 形态：`{"name","about","usage","version","options":[{long,short,type,arity,min,max,
 * required,choices,default,minimum,maximum,group,hidden,conflicts_with,help}],
 * "positionals":[...],"subcommands":[...]}`。内建 help/version 一并列出，便于 AI 枚举全量。
 */
[[nodiscard]] auto schema_json(const CommandSpec &spec) -> Json;

}  // namespace aurora::cli
