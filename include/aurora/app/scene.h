#pragma once

#include <optional>
#include <string>

#include "aurora/render/offscreen.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 场景：持有一棵 widget 树，提供无头渲染与结构快照。
///
/// 对应 specification/06-app-platform.md §2.3 / ARCHITECTURE.md §4.6：`Scene` 只持 widget 树与根环境；`Window` 归
/// `Application`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class Scene {
  public:
    /// @brief 以给定根节点构造场景：接管该 widget 树作为渲染/快照对象。
    /// @param root 根节点（必须持有有效 widget）。
    explicit Scene(Node root) : root_(std::move(root)) {}

    /// @brief 根 widget 引用（读写控件属性用）。
    /// @return root_ 内 widget 的引用。
    [[nodiscard]] auto root() -> Widget & { return root_.widget(); }

    /// @brief 返回根节点（供事件派发读取根命中矩形；几何权威在 Node）。
    /// @return 根节点的引用（即构造时传入的 root_，非拷贝）。
    [[nodiscard]] auto root_node() -> Node & { return root_; }

    /// @brief 无头渲染为 PNG（specification/03-layout-render.md §8.4）。
    /// @param path 输出 PNG 文件路径。
    /// @param width 画布宽（像素）。
    /// @param height 画布高（像素）。
    /// @param background 可选底色：与真实窗口比对时传 `Surface::clear_color()` 同款（见 offscreen.h）。
    /// @return 渲染并写盘成功为 `Ok(true)`；失败为结构化 Error。
    [[nodiscard]] auto render_to_png(const char *path, int width, int height,
                                     std::optional<Color> background = std::nullopt) -> Result<bool> {
        return aurora::render_to_png(root_, width, height, path, background);
    }

    /// @brief 结构快照（JSON）：用于 golden 比对（specification/06-app-platform.md §12.2，跨平台稳定）。
    /// @return 嵌套 JSON 字符串：每节点含 type 与 size，含子节点时附 children 数组。
    [[nodiscard]] auto serialize() const -> std::string;

  private:
    static auto serialize_widget(const Widget &w, std::string &out) -> void;

    Node root_;
};

}  // namespace aurora
