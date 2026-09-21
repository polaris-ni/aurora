#include "aurora/commands.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace aurora {

namespace {

/// @brief ASCII 大小写折叠（非 ASCII 字节原样返回，按整字节参与子序列匹配）。
constexpr auto fold_ascii(char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// @brief 词边界：命令标题/标识中的分隔符，命中其后视为「新词起点」（加权）。
constexpr auto is_word_boundary(char c) -> bool {
    return c == ' ' || c == '.' || c == '/' || c == '-' || c == '_' || c == ':';
}

/// @brief 间隔惩罚上限（避免长间隔把得分压到哨兵值 -1 附近）。
constexpr int AURORA_MAX_GAP_PENALTY = 3;

/// @brief 标题长度惩罚分母：标题越长，同分下越靠后（促使短标题优先）。
constexpr std::size_t AURORA_LENGTH_PENALTY_DIVISOR = 16;

}  // namespace

auto command_fuzzy_score(const std::string &query, const std::string &text) -> int {
    if (query.empty()) {
        return 0;  // 空查询匹配全部
    }

    int score = 0;
    std::size_t qi = 0;
    std::size_t prev_match = std::string::npos;
    for (std::size_t ti = 0; ti < text.size() && qi < query.size(); ++ti) {
        if (fold_ascii(text[ti]) != fold_ascii(query[qi])) {
            continue;
        }
        score += 1;  // 基础分
        if (prev_match != std::string::npos) {
            if (ti == prev_match + 1) {
                score += 2;  // 连续命中
            }
            const std::size_t gap = ti - prev_match - 1;
            score -= static_cast<int>(std::min<std::size_t>(gap, AURORA_MAX_GAP_PENALTY));  // 间隔惩罚
        }
        if (ti == 0 || is_word_boundary(text[ti - 1])) {
            score += 3;  // 首字符或词边界后命中
        }
        prev_match = ti;
        ++qi;
    }

    if (qi < query.size()) {
        return -1;  // 非子序列：不匹配
    }
    score -= static_cast<int>(text.size() / AURORA_LENGTH_PENALTY_DIVISOR);  // 轻微长度惩罚
    return std::max(0, score);  // 有效匹配恒 ≥ 0，与「不匹配 = -1」不混
}

auto CommandRegistry::add(Command cmd) -> int {
    if (cmd.id.empty()) {
        return -1;  // 空标识非法：不入表
    }
    const int reg_id = next_reg_id_;
    ++next_reg_id_;
    if (const auto it = index_.find(cmd.id); it != index_.end()) {
        cmds_[it->second] = std::move(cmd);  // 覆盖：保持注册序
    } else {
        index_.emplace(cmd.id, cmds_.size());
        cmds_.push_back(std::move(cmd));
    }
    return reg_id;
}

auto CommandRegistry::remove(const std::string &id) -> bool {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    const std::size_t pos = it->second;
    if (const auto bound = shortcut_of_.find(id); bound != shortcut_of_.end()) {
        if (shortcuts_ != nullptr) {
            shortcuts_->remove(bound->second);  // 连带撤销快捷键绑定
        }
        shortcut_of_.erase(bound);
    }
    cmds_.erase(cmds_.begin() + static_cast<std::ptrdiff_t>(pos));  // 保持注册序
    index_.erase(it);
    for (auto &kv : index_) {  // 其后元素下标整体前移
        if (kv.second > pos) {
            --kv.second;
        }
    }
    enabled_overrides_.erase(id);
    return true;
}

auto CommandRegistry::clear() -> void {
    if (shortcuts_ != nullptr) {
        for (const auto &kv : shortcut_of_) {
            shortcuts_->remove(kv.second);
        }
    }
    shortcut_of_.clear();
    enabled_overrides_.clear();
    index_.clear();
    cmds_.clear();
}

auto CommandRegistry::find(const std::string &id) const -> const Command * {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return nullptr;
    }
    return &cmds_[it->second];
}

auto CommandRegistry::enabled_of(const Command &cmd) const -> bool {
    bool on = true;
    if (const auto it = enabled_overrides_.find(cmd.id); it != enabled_overrides_.end()) {
        on = it->second;
    }
    if (on && cmd.enabled) {
        on = cmd.enabled();
    }
    return on;
}

auto CommandRegistry::is_enabled(const std::string &id) const -> bool {
    const Command *cmd = find(id);
    return cmd != nullptr && enabled_of(*cmd);
}

auto CommandRegistry::set_enabled(const std::string &id, bool on) -> bool {
    if (!contains(id)) {
        return false;
    }
    enabled_overrides_[id] = on;
    return true;
}

auto CommandRegistry::invoke(const std::string &id) const -> bool {
    const Command *cmd = find(id);
    if (cmd == nullptr || !cmd->action || !enabled_of(*cmd)) {
        return false;
    }
    cmd->action();
    return true;
}

auto CommandRegistry::search(const std::string &query, bool only_enabled) const -> std::vector<const Command *> {
    struct Scored {
        const Command *cmd = nullptr;
        int score = 0;
    };
    std::vector<Scored> hits;
    hits.reserve(cmds_.size());
    for (const Command &cmd : cmds_) {
        if (only_enabled && !enabled_of(cmd)) {
            continue;
        }
        const int score = command_fuzzy_score(query, cmd.title);
        if (score < 0) {
            continue;
        }
        hits.push_back(Scored{.cmd = &cmd, .score = score});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Scored &a, const Scored &b) -> bool {
        if (a.score != b.score) {
            return a.score > b.score;  // 得分降序
        }
        return a.cmd->title < b.cmd->title;  // 同分按标题升序（稳定，注册序兜底）
    });

    std::vector<const Command *> out;
    out.reserve(hits.size());
    for (const Scored &hit : hits) {
        out.push_back(hit.cmd);
    }
    return out;
}

auto CommandRegistry::to_json() const -> Json {
    Json envelope = Json::object();
    Json items = Json::array();
    for (const Command &cmd : cmds_) {
        Json item = Json::object();
        item["id"] = cmd.id;
        item["title"] = cmd.title;
        if (!cmd.icon.empty()) {
            item["icon"] = cmd.icon;
        }
        if (!cmd.category.empty()) {
            item["category"] = cmd.category;
        }
        if (!cmd.when_label.empty()) {
            item["when"] = cmd.when_label;
        }
        item["enabled"] = enabled_of(cmd);
        item["invocable"] = static_cast<bool>(cmd.action);
        if (cmd.default_binding.has_value()) {
            item["default_binding"] = cmd.default_binding->to_string();
        }
        items.push_back(std::move(item));
    }
    envelope["commands"] = std::move(items);
    return envelope;
}

auto CommandRegistry::bind_shortcuts(ShortcutRegistry &sr) -> void {
    for (const auto &kv : shortcut_of_) {  // 幂等：先撤销上次产生的绑定
        sr.remove(kv.second);
    }
    shortcut_of_.clear();
    shortcuts_ = &sr;
    for (const Command &cmd : cmds_) {
        if (!cmd.default_binding.has_value()) {
            continue;
        }
        const std::string cid = cmd.id;  // 按值捕获，避免悬垂
        const int bound =
            sr.add(*cmd.default_binding, [this, cid]() -> void { (void)this->invoke(cid); }, cmd.scope, cmd.title);
        shortcut_of_.emplace(cid, bound);
    }
}

auto CommandRegistry::to_menu_items() const -> std::vector<MenuItem> {
    std::vector<MenuItem> out;
    out.reserve(cmds_.size());
    for (const Command &cmd : cmds_) {
        MenuItem item;
        item.label = cmd.title;
        item.icon = cmd.icon;
        item.enabled = enabled_of(cmd);
        if (cmd.default_binding.has_value()) {
            item.shortcut_text = cmd.default_binding->to_string();
        }
        const std::string cid = cmd.id;
        item.on_click = [this, cid]() -> void { (void)this->invoke(cid); };
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace aurora
