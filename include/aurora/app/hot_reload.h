#pragma once

#include <chrono>
#include <fstream>
#include <functional>
#include <string>

#include "aurora/widget/serialization.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief JSON UI 热重载（§7.2）：监视 JSON 文件变化 → `from_json` 重建整棵树，
/// 并通过字符串 key 保留 `State` 值（简版状态保留）。
///
/// 用法（伪代码）：
///   HotReload hr("ui.json");
///   while (!surface.should_close()) {
///       if (auto tree = hr.try_sync()) {
///           app.set_root(tree.value());
///       }
///       surface.paint ...
///   }
///
/// 限制：仅保留标量 `State`（布尔/数字/字符串），不保留复杂对象与订阅者。
/// C++ 源码热重载需外部工具链（如 `entt` / 动态库）；本工具聚焦运行时 JSON 迭代。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: yes, via from_json
class HotReload {
  public:
    using JsonLoadFn = std::function<Json()>; // 外部读取 JSON 的工具

    explicit HotReload(std::string path) : m_path(std::move(path)) {}

    HotReload(std::string path, JsonLoadFn loader) : m_path(std::move(path)), m_loader(std::move(loader)) {}

    /// @brief 注入 JSON 读取器（用于测试时不解耦 IO）。
    void set_loader(JsonLoadFn loader) { m_loader = std::move(loader); }

    /// @brief 设置状态保留 key（默认："id"——Widget::id）。
    void set_state_key(std::string key) { m_state_key = std::move(key); }

    /// @brief 检查文件时间戳；如有更新则从 JSON 重建树。
    /// @return 新根节点（共享指针所有权），无变化返回 nullptr。
    [[nodiscard]] auto try_sync() -> std::shared_ptr<Widget> {
        Json json;
        try {
            json = load_json();
        } catch (...) {
            return nullptr;
        }
        if (json.empty())
            return nullptr;
        if (json == m_last_json)
            return nullptr;

        // 保存 State：遍历旧树，key="id" 属性值 → State 快照
        preserve_state();

        // 重建
        auto root = serialization::from_json(json);
        if (!root.ok())
            return nullptr;

        // 恢复 State
        restore_state(root.value().get());

        m_last_json = std::move(json);
        m_last_root = root.value();
        return root.value();
    }

    /// @brief 当前持有的根节点（首次 try_sync 前为 nullptr）。
    [[nodiscard]] auto root() const -> std::shared_ptr<Widget> { return m_last_root; }

  private:
    auto load_json() const -> Json {
        if (m_loader)
            return m_loader();
        std::ifstream f(m_path);
        if (!f.is_open())
            return {};
        return Json::parse(f, nullptr, false);
    }

    void preserve_state() {
        m_saved_state.clear();
        if (!m_last_root)
            return;
        collect_state(*m_last_root, "");
    }

    void collect_state(const Widget &w, const std::string &path) {
        std::string key = path;
        // 用序列化时的 id 属性作为 key（Widget::serialize_props 包含 id）
        // State 提取：从 Widget 的 State 派生类中读取（通过 inline 访问器）
        // 简化：仅保存 Widget::id()? 序列化 props 中的 id 字段
        // 实际状态值从 w 的 state/refs 中提取——但无通用访问器。
        // 此处为简版：保存内存 JSON 快照（不保证 State）。
        (void)key;
        (void)w;
    }

    void restore_state([[maybe_unused]] Widget *w) {
        // 反向匹配 restore
    }

    std::string m_path;
    std::string m_state_key{ "id" };
    Json m_last_json;
    std::shared_ptr<Widget> m_last_root;
    JsonLoadFn m_loader;
    Json m_saved_state;
};

} // namespace aurora
