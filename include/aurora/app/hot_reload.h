#pragma once

#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief JSON UI 热重载（specification/08-tooling.md §2.7）：监视 JSON 变化 → `from_json` 重建整棵树，
/// 并**按树路径**保留上一棵树里 JSON 未显式声明的属性值。
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
    /// @brief 外部 JSON 读取器签名：无参返回解析好的 Json（测试可注入，替代按 `path_` 的文件读取）。
    using JsonLoadFn = std::function<Json()>;  // 外部读取 JSON 的工具

    /// @brief 按文件路径构造：每次 try_sync 用 ifstream 读取该路径的 JSON。
    /// @param path JSON UI 文件路径（可稍后经 set_loader 注入读取器覆盖）。
    explicit HotReload(std::string path) : path_(std::move(path)) {}

    /// @brief 路径 + 自定义读取器构造：loader 非空时优先于文件读取。
    /// @param path JSON UI 文件路径（保留为状态来源标识）。
    /// @param loader JSON 读取器（测试或自定义 IO 注入）。
    HotReload(std::string path, JsonLoadFn loader) : path_(std::move(path)), loader_(std::move(loader)) {}

    /// @brief 注入 JSON 读取器（用于测试时不解耦 IO）。
    /// @param loader 替代按 path_ 文件读取的 JSON 读取器；非空时 try_sync 每次经它取 JSON。
    void set_loader(JsonLoadFn loader) { loader_ = std::move(loader); }

    /// @brief 保留项 —— 现已按树路径匹配，故本设置不再参与匹配。
    /// @deprecated 改用 try_sync 的按树路径状态保留（无需任何键设置）；自 1.0.0 起仅为兼容保留。
    /// @note `id` 不经 JSON 往返，按 id 保留状态在热重载下无法成立。
    /// @param key 原「按 id 保留状态」的键名；本接口为空实现，不消费该参数。
    /// @note 形参取 const 引用：空实现（仅兼容占位），按值传 std::string 徒增一次拷贝。
    void set_state_key([[maybe_unused]] const std::string &key) {}

    /// @brief 检查文件时间戳；如有更新则从 JSON 重建树。
    /// @return 新根节点（共享指针所有权），无变化或重建失败返回 nullptr。
    /// @note 结构变化（增删子节点）后按路径保留会对错位：此时宁可少恢复几项也不猜，
    ///       故只对**新旧都存在**的路径回填。
    [[nodiscard]] auto try_sync() -> std::shared_ptr<Widget> {
        Json json;  // 本轮读入并与 last_json_ 比对的 JSON 暂存
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

        preserve_state();  // 保存旧树的状态快照，供重建后按路径回填

        auto root = aurora::serialization::from_json(json);  // 由新 JSON 重建的根节点结果
        if (!root.ok()) {
            return nullptr;
        }

        restore_state(Node{root.value()},
                      json);  // 恢复状态（须在 last_json_ 接管 json 之前——恢复要用它判断哪些属性是显式声明的）

        last_json_ = std::move(json);
        last_root_ = root.value();
        return root.value();
    }

    /// @brief 当前持有的根节点（首次 try_sync 前为 nullptr）。
    /// @return 最近一次成功重建的根节点共享指针；尚未重建过时为 `nullptr`。
    [[nodiscard]] auto root() const -> std::shared_ptr<Widget> { return last_root_; }

  private:
    [[nodiscard]] auto load_json() const -> Json {
        if (loader_) {
            return loader_();
        }
        std::ifstream f(path_);  // 未注入 loader 时按 path_ 打开的只读文件流
        if (!f.is_open()) {
            return {};
        }
        std::string content{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        const auto parsed = json::parse(content);
        return parsed.ok() ? parsed.value() : Json{};
    }

    /// @brief 状态快照：清空并重扫上一棵树，把各路径节点序列化出的属性对象存入 saved_state_。
    /// 无历史根（首次同步）时直接返回，不产生任何快照。
    void preserve_state() {
        saved_state_.clear();
        if (!last_root_) {
            return;
        }
        collect_state(Node{last_root_}, std::string{});
    }

    /// @brief 深度遍历收集状态：把 node 自身属性按 path 记入 saved_state_，再递归子节点。
    /// 子节点路径为 `path + "/" + 序号`（根为 ""，首层即 "0"、"1"，与 find_node_by_path 同格式）。
    /// @param node 当前遍历到的控件节点。
    /// @param path 该节点的树路径（根为空串）。
    void collect_state(Node node, const std::string &path) {
        Widget &w = node.widget();
        Json props = Json::object();
        w.serialize_props(props);
        if (!props.empty()) {
            saved_state_.set(path, std::move(props));
        }
        const std::vector<Node> &children = w.child_nodes();
        for (std::size_t i = 0; i < children.size(); ++i) {
            // 子节点路径：根为 ""，故首层直接是 "0"、"1"，与 find_node_by_path 同格式。
            collect_state(children[i], path.empty() ? std::to_string(i) : path + "/" + std::to_string(i));
        }
    }

    /// @brief 状态回填：只补 JSON 未显式声明的标量属性，按树路径逐层对齐新旧树。
    /// 仅当快照存在且该路径在新 JSON 中也有对应节点时回填；非 JSON 对象节点按空 props/children 处理。
    /// @param node 新树中的当前控件节点。
    /// @param json_node 该节点在新 JSON 中的对象（含可选的 props / children 键）。
    void restore_state(Node node, const Json &json_node) {
        const std::string path = current_path_;
        const Json *snapshot_ptr = saved_state_.find(path);
        const auto *props_ptr = json_node.is_object() ? json_node.at("props") : nullptr;
        const Json declared = props_ptr != nullptr ? *props_ptr : Json::object();

        if (snapshot_ptr != nullptr && declared.is_object()) {
            Widget &w = node.widget();
            // Value::find 按键寻址，未命中返回 nullptr；此处已判空，直接解引用取该路径的快照。
            const Json &snapshot = *snapshot_ptr;
            for (const auto &kv : snapshot.entries()) {
                if (declared.contains(kv.key)) {
                    continue;  // 源文件显式声明的值优先
                }
                // 只回填标量：回调 / 订阅者 / 复杂对象无法经 JSON 表达，硬喂只会报错。
                const Json &v = kv.value;
                if (!(v.is_string() || v.is_number() || v.is_bool())) {
                    continue;
                }
                static_cast<void>(Inspector::set_prop(w, kv.key, v));
            }
        }

        const std::vector<Node> &children = node.widget().child_nodes();
        const auto *children_ptr = json_node.is_object() ? json_node.at("children") : nullptr;
        const Json json_children = children_ptr != nullptr ? *children_ptr : Json::array();
        const std::size_t count = children.size() < json_children.size() ? children.size() : json_children.size();
        for (std::size_t i = 0; i < count; ++i) {
            const std::string saved = current_path_;
            current_path_ = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
            restore_state(children[i], *json_children.at(i));
            current_path_ = saved;
        }
    }

    std::string path_;  ///< JSON UI 文件路径（未注入 loader 时的读取来源）
    Json last_json_;  ///< 上次成功解析的 JSON（与新读内容比对判是否变化；恢复状态时据此识别显式声明）
    std::shared_ptr<Widget> last_root_;  ///< 上次重建的根节点（preserve_state 的快照来源；初始为空）
    JsonLoadFn loader_;  ///< 注入的 JSON 读取器（可空；非空时优先于按 path_ 的文件读取）
    Json saved_state_ = Json::object();  ///< 路径 → 属性对象
    std::string current_path_;  ///< restore_state 递归过程中的当前路径
};

}  // namespace aurora
