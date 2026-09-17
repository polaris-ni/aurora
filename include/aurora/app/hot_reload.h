#pragma once

#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief JSON UI 热重载（specification/08-tooling.md §2.7）：监视 JSON 变化 → `from_json` 重建整棵树，
/// 并**按树路径**保留上一棵树里 JSON 未显式声明的属性值。
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
/// **状态保留的键为什么是「树路径」而不是 id**：`Widget::id` 不经 JSON 往返
/// （`serialize_props` 不含 id、`from_json` 也不读），所以按 id 匹配在热重载场景下根本对不上。
/// 树路径（`"0"` / `"1/2"`，与 `Inspector::find_node` 同格式）只要结构没变就稳定，且无需给
/// `Widget` 增加任何新虚接口。代价：结构重排后同名路径会错位 —— 这是刻意的取舍，
/// 见 `try_sync` 注释。
///
/// 只回填 **JSON 里没写** 的属性：源文件显式声明的值永远优先，热重载不该把用户刚改的 JSON 覆盖掉。
///
/// 限制：仅保留**标量**属性（布尔 / 数字 / 字符串）；不保留回调、订阅者与复杂对象。
/// C++ 源码热重载需外部工具链（动态库 / JIT）；本工具聚焦运行时 JSON 迭代。
///
/// @note Thread: main-thread only
/// @note Side-effects: 重建时写入控件属性
/// @note Rebuildable: yes, via from_json
class HotReload {
  public:
    using JsonLoadFn = std::function<Json()>;  // 外部读取 JSON 的工具

    explicit HotReload(std::string path) : path_(std::move(path)) {}

    HotReload(std::string path, JsonLoadFn loader) : path_(std::move(path)), loader_(std::move(loader)) {}

    /// @brief 注入 JSON 读取器（用于测试时不解耦 IO）。
    void set_loader(JsonLoadFn loader) { loader_ = std::move(loader); }

    /// @brief 保留项 —— 现已按树路径匹配，故本设置不再参与匹配。
    /// @deprecated 仅为兼容保留；`id` 不经 JSON 往返，按 id 保留状态在热重载下无法成立。
    void set_state_key([[maybe_unused]] std::string key) {}

    /// @brief 检查文件时间戳；如有更新则从 JSON 重建树。
    /// @return 新根节点（共享指针所有权），无变化或重建失败返回 nullptr。
    ///
    /// @note 结构变化（增删子节点）后按路径保留会对错位：此时宁可少恢复几项也不猜，
    ///       故只对**新旧都存在**的路径回填。
    [[nodiscard]] auto try_sync() -> std::shared_ptr<Widget> {
        Json json;
        try {
            json = load_json();
        } catch (...) {
            return nullptr;
        }
        if (json.empty()) {
            return nullptr;
        }
        if (json == last_json_) {
            return nullptr;
        }

        // 保存旧树的状态快照
        preserve_state();

        // 重建
        auto root = aurora::serialization::from_json(json);
        if (!root.ok()) {
            return nullptr;
        }

        // 恢复状态（需在 last_json_ 接管 json 之前 —— 恢复要用它判断哪些属性是显式声明的）
        restore_state(Node{root.value()}, json);

        last_json_ = std::move(json);
        last_root_ = root.value();
        return root.value();
    }

    /// @brief 当前持有的根节点（首次 try_sync 前为 nullptr）。
    [[nodiscard]] auto root() const -> std::shared_ptr<Widget> { return last_root_; }

  private:
    [[nodiscard]] auto load_json() const -> Json {
        if (loader_) {
            return loader_();
        }
        std::ifstream f(path_);
        if (!f.is_open()) {
            return {};
        }
        return Json::parse(f, nullptr, false);
    }

    // ── 状态快照：路径 → 该节点序列化出的属性对象 ──
    void preserve_state() {
        saved_state_.clear();
        if (!last_root_) {
            return;
        }
        collect_state(Node{last_root_}, std::string{});
    }

    void collect_state(Node node, const std::string &path) {
        Widget &w = node.widget();
        Json props = Json::object();
        w.serialize_props(props);
        if (!props.empty()) {
            saved_state_[path] = std::move(props);
        }
        const std::vector<Node> &children = w.child_nodes();
        for (std::size_t i = 0; i < children.size(); ++i) {
            // 子节点路径：根为 ""，故首层直接是 "0"、"1"，与 find_node_by_path 同格式。
            collect_state(children[i],
                          path.empty() ? std::to_string(i) : path + "/" + std::to_string(i));
        }
    }

    // ── 状态回填：只补 JSON 未显式声明的标量属性 ──
    void restore_state(Node node, const Json &json_node) {
        const std::string path = current_path_;
        const auto it = saved_state_.find(path);
        const Json declared = json_node.is_object() ? json_node.value("props", Json::object())
                                                    : Json::object();

        if (it != saved_state_.end() && declared.is_object()) {
            Widget &w = node.widget();
            // nlohmann 的对象迭代器解引用得到的是**值**而非 pair，故用 `it.value()` 取回该路径的快照。
            const Json &snapshot = it.value();
            for (auto kv = snapshot.begin(); kv != snapshot.end(); ++kv) {
                if (declared.contains(kv.key())) {
                    continue;  // 源文件显式声明的值优先
                }
                // 只回填标量：回调 / 订阅者 / 复杂对象无法经 JSON 表达，硬喂只会报错。
                const Json &v = kv.value();
                if (!(v.is_string() || v.is_number() || v.is_boolean())) {
                    continue;
                }
                static_cast<void>(Inspector::set_prop(w, kv.key(), v));
            }
        }

        const std::vector<Node> &children = node.widget().child_nodes();
        const Json &json_children = json_node.is_object() ? json_node.value("children", Json::array())
                                                          : Json::array();
        const std::size_t count = children.size() < json_children.size() ? children.size()
                                                                         : json_children.size();
        for (std::size_t i = 0; i < count; ++i) {
            const std::string saved = current_path_;
            current_path_ = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
            restore_state(children[i], json_children[i]);
            current_path_ = saved;
        }
    }

    std::string path_;
    Json last_json_;
    std::shared_ptr<Widget> last_root_;
    JsonLoadFn loader_;
    Json saved_state_;         ///< 路径 → 属性对象
    std::string current_path_;  ///< restore_state 递归过程中的当前路径
};

}  // namespace aurora
