#pragma once

// command_listing.h — 命令枚举/解析逻辑（aurora_mcp 服务器与其集成测试共用）。
//
// 命令工具是**无状态**的：调用方随每次请求传入命令描述符（宿主 `CommandRegistry::to_json()` 的产物），
// 因此下面的逻辑是纯 JSON 入 / JSON 出，无需运行中的进程即可被直测。检索复用库的
// `command_fuzzy_score` 与「得分降序、标题升序」排序，保证 AI 检索与命令面板给出同样的次序。

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "aurora/commands.h"
#include "aurora/widget/props_io.h"

namespace aurora::tools {

/// @brief 从工具入参中取出命令描述符数组。
/// 接受 `CommandRegistry::to_json()` 产出的自描述信封 `{"commands":[...]}`，也接受裸数组。
/// @return 指向 `arg` 内部的数组；形态不识别时返回 nullptr。
[[nodiscard]] inline auto command_descriptors(const Json &arg) -> const Json * {
    if (arg.is_array()) {
        return &arg;
    }
    if (arg.is_object()) {
        const auto it = arg.find("commands");
        if (it != arg.end() && it->is_array()) {
            return &(*it);
        }
    }
    return nullptr;
}

/// @brief 读取可选的布尔成员；缺失或类型不符时返回 fallback（不抛异常）。
[[nodiscard]] inline auto command_bool_field(const Json &obj, const char *key, bool fallback) -> bool {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

/// @brief 命令枚举结果：命中的描述符下标（对应入参数组）与参与过滤的描述符总数。
struct CommandListing {
    std::vector<std::size_t> indices;  ///< 按（得分降序、标题升序）稳定排序后的下标
    std::size_t considered = 0;  ///< 形态合法、参与过滤的描述符数量
};

/// @brief 按查询过滤并按（得分降序、标题升序）稳定排序命令描述符。
/// @param items 描述符数组（`command_descriptors` 的返回值解引用）
/// @param query 模糊查询串（空 = 全部匹配）
/// @param include_disabled 是否保留 `enabled` 为假的描述符
[[nodiscard]] inline auto list_commands(const Json &items, const std::string &query, bool include_disabled)
    -> CommandListing {
    struct Hit {
        std::size_t index = 0;
        std::string title;
        int score = 0;
    };
    std::vector<Hit> hits;
    CommandListing out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Json &item = items[i];
        if (!item.is_object()) {
            continue;
        }
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string()) {
            continue;
        }
        ++out.considered;
        if (!include_disabled && !command_bool_field(item, "enabled", true)) {
            continue;
        }
        const auto title_it = item.find("title");
        const std::string title = (title_it != item.end() && title_it->is_string()) ? title_it->get<std::string>()
                                                                                    : id_it->get<std::string>();
        const int score = command_fuzzy_score(query, title);
        if (score < 0) {
            continue;
        }
        hits.push_back(Hit{.index = i, .title = title, .score = score});
    }
    std::ranges::stable_sort(hits, [](const Hit &a, const Hit &b) -> bool {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.title < b.title;
    });
    out.indices.reserve(hits.size());
    for (const Hit &hit : hits) {
        out.indices.push_back(hit.index);
    }
    return out;
}

/// @brief 命令的远程可调用性判定结果。
enum class CommandStatus {
    NotFound,  ///< 描述符中不存在该标识
    Disabled,  ///< `enabled` 为假（启用条件不满足）
    NotInvocable,  ///< `invocable` 为假（命令无动作体）
    Invocable,  ///< 可调用
};

/// @brief 判定结果的稳定标识（工具面输出用）。
[[nodiscard]] inline auto command_status_name(CommandStatus status) -> const char * {
    switch (status) {
        case CommandStatus::NotFound:
            return "not-found";
        case CommandStatus::Disabled:
            return "disabled";
        case CommandStatus::NotInvocable:
            return "not-invocable";
        case CommandStatus::Invocable:
            return "invocable";
    }
    return "not-found";
}

/// @brief 按 `id` 解析命令描述符并判定可调用性。
/// @param status 输出：判定结果（未命中时为 NotFound）
/// @return 命中的描述符；未命中返回 nullptr
[[nodiscard]] inline auto resolve_command(const Json &items, const std::string &id, CommandStatus &status)
    -> const Json * {
    for (const Json &item : items) {
        if (!item.is_object()) {
            continue;
        }
        const auto it = item.find("id");
        if (it == item.end() || !it->is_string() || it->get<std::string>() != id) {
            continue;
        }
        if (!command_bool_field(item, "enabled", true)) {
            status = CommandStatus::Disabled;
        } else if (!command_bool_field(item, "invocable", false)) {
            status = CommandStatus::NotInvocable;
        } else {
            status = CommandStatus::Invocable;
        }
        return &item;
    }
    status = CommandStatus::NotFound;
    return nullptr;
}

}  // namespace aurora::tools
