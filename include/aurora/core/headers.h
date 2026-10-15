#pragma once

/// @brief HTTP 头集合值类型（CHG-018 §12：大小写不敏感、重复头保序的多值映射）。
/// @file core/headers.h
///
/// `Headers` 表达一组「字段名 → 值」，是 HTTP/SSE/WebSocket 握手与 Inspector 共用的基础件。
/// 三条来自 RFC 9110 §5 的语义约束决定了它不能用 `std::map<string,string>` 顶替：
///
/// - **字段名大小写不敏感**：`Content-Type` 与 `content-type` 是同一个头；内部保留**首次写入
///   时的原始拼写**（HTTP/1.1 线上形态需按书写者意图发送），比较时才折叠大小写。
/// - **重复头保序且不折叠**：同名字段可出现多次且顺序有意义（`Set-Cookie` 每条独立生效），
///   故存储是有序条目表而非按键去重的字典；`get()` 只取首个匹配，`values()` 返回全部。
/// - **ASCII 折叠口径**：只在 ASCII 字母范围内折叠大小写，不做 locale 相关的 Unicode 折叠——
///   HTTP 字段名本身就是 ASCII 令牌（RFC 9110 §5.1），非 ASCII 名字按「不相等」处理即可。
///
/// 值类型：不含句柄、无共享状态，可在任意线程拷贝与比较；等值比较**逐条目**比较，
/// 名字折叠大小写、值严格比较。
///
/// @code
/// aurora::Headers headers;
/// headers.set("Content-Type", "application/json");
/// headers.append("Set-Cookie", "a=1");
/// headers.append("Set-Cookie", "b=2");          // 两条各自保留，不折叠
/// (void)headers.get("content-type");            // "application/json"（大小写不敏感）
/// (void)headers.values("set-cookie");           // {"a=1", "b=2"}（保序）
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora {

/// @brief 有序、大小写不敏感、允许同名重复的 HTTP 头集合。
/// @note Thread: thread-safe（拷贝、比较与读写均无共享状态）
/// @note Side-effects: none
class Headers {
  public:
    /// @brief 单个头部条目：保留写入时的原始名字拼写与值。
    struct Entry {
        std::string name;  ///< 字段名（原始拼写；比较时按 ASCII 大小写不敏感）
        std::string value;  ///< 字段值（不做 trim 之外的加工；严格比较）
    };

    /// @brief 置入单值字段：先移除全部同名字段，再把新条目追加到末尾。
    ///
    /// 追加到末尾而非原地覆盖，是为了让「同一个 `Headers` 序列化到线上」的字段次序与调用
    /// 时序一致——HTTP/1.1 不依赖次序，但 Inspector 时间线与测试断言依赖它稳定。
    /// @param name 字段名；保留原始拼写用于输出。
    /// @param value 字段值。
    auto set(std::string_view name, std::string_view value) -> void {
        erase_matching(name);
        entries_.push_back(Entry{.name = std::string(name), .value = std::string(value)});
    }

    /// @brief 追加同名字段：保留已有条目，新条目置于末尾（多值字段如 `Set-Cookie` 的正确形态）。
    /// @param name 字段名；保留原始拼写用于输出。
    /// @param value 字段值。
    auto append(std::string_view name, std::string_view value) -> void {
        entries_.push_back(Entry{.name = std::string(name), .value = std::string(value)});
    }

    /// @brief 取首个匹配字段的值。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 首个匹配条目的值；无匹配时为 `nullopt`。
    [[nodiscard]] auto get(std::string_view name) const -> std::optional<std::string> {
        for (const auto &e : entries_) {
            if (name_equal(e.name, name)) {
                return e.value;
            }
        }
        return std::nullopt;
    }

    /// @brief 取同名字段的全部值，按插入顺序返回且**绝不折叠**。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 匹配到的值序列（可能为空）；顺序与写入顺序一致。
    [[nodiscard]] auto values(std::string_view name) const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto &e : entries_) {
            if (name_equal(e.name, name)) {
                out.push_back(e.value);
            }
        }
        return out;
    }

    /// @brief 查询是否存在某字段。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 存在至少一个同名条目时为 true。
    [[nodiscard]] auto contains(std::string_view name) const -> bool {
        return std::any_of(entries_.begin(), entries_.end(),
                           [name](const Entry &e) { return name_equal(e.name, name); });
    }

    /// @brief 移除某字段的全部条目。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 实际移除的条目数（0 表示不存在，操作幂等）。
    auto remove(std::string_view name) -> std::size_t {
        const std::size_t before = entries_.size();
        erase_matching(name);
        return before - entries_.size();
    }

    /// @brief 清空全部条目。
    auto clear() -> void { entries_.clear(); }

    /// @brief 条目总数（同名重复按多条计）。
    /// @return 当前条目数。
    [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

    /// @brief 是否为空集合。
    /// @return 无任何条目时为 true。
    [[nodiscard]] auto empty() const -> bool { return entries_.empty(); }

    /// @brief 按插入顺序查看全部条目（序列化与遍历的唯一真值来源）。
    /// @return 条目序列的只读引用；名字为写入时的原始拼写。
    [[nodiscard]] auto entries() const -> const std::vector<Entry> & { return entries_; }

    /// @brief 逐条目等值比较：名字折叠 ASCII 大小写，值严格比较，顺序敏感。
    /// @param other 参与比较的头集合。
    /// @return 条目数相同且逐项名字/值相等时为 true。
    [[nodiscard]] auto operator==(const Headers &other) const -> bool {
        if (entries_.size() != other.entries_.size()) {
            return false;
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!name_equal(entries_[i].name, other.entries_[i].name) || entries_[i].value != other.entries_[i].value) {
                return false;
            }
        }
        return true;
    }

  private:
    /// @brief ASCII 大小写不敏感地比较两个字段名（非 ASCII 字节按原值比较，不做 Unicode 折叠）。
    /// @param a 左操作数。
    /// @param b 右操作数。
    /// @return 长度相等且逐字节（字母折叠后）相等时为 true。
    [[nodiscard]] static auto name_equal(std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
                return false;
            }
        }
        return true;
    }

    /// @brief 单字节 ASCII 小写化；非大写字母原样返回（避免 locale 与负值 `char` 相关的 UB）。
    /// @param c 原始字节。
    /// @return 折叠后的字节。
    [[nodiscard]] static auto to_lower_ascii(char c) -> char {
        const auto raw = static_cast<unsigned char>(c);
        if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
            return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
        }
        return c;
    }

    /// @brief 删除全部同名字段条目（`set` 与 `remove` 共用的收尾动作）。
    /// @param name 字段名；ASCII 大小写不敏感。
    auto erase_matching(std::string_view name) -> void {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (name_equal(it->name, name)) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<Entry> entries_;  ///< 有序条目表：插入顺序即线上顺序，同名不去重
};

}  // namespace aurora
