#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/render/painter.h"
#include "aurora/widget/data_widgets.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/inspect.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief Inspector 面板（树形浏览器 + 属性编辑器）。
 *
 * 左右分栏布局：左侧 TreeView 展示 Widget 层级树，右侧属性面板展示选中 Widget
 * 的类型名、属性描述与当前值。支持运行时属性回写（经 set_widget_prop）。
 *
 * 构造时接受 `std::function<Node()>` 以获取目标 Widget 树根节点（支持动态树）。
 * 调用 `refresh()` 重建树映射；选中 TreeView 行时自动更新右侧属性面板。
 *
 * 对标 Flutter Inspector / Chrome DevTools Elements 面板。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class InspectorPanel : public Container {
  public:
    InspectorPanel() = default;

    /// @brief 构造：接受目标树获取函数 + 可选初始比例。
    explicit InspectorPanel(std::function<Node()> root_getter, float tree_ratio = 0.35F);

    [[nodiscard]] auto type_name() const -> const char * override { return "InspectorPanel"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 设置/更新目标树获取函数并刷新。
    auto set_root(std::function<Node()> getter) -> void;

    /// @brief 刷新树映射与属性面板。
    auto refresh() -> void;

    /// @brief 选中 Widget 的回调（可选，外部联动用）。
    std::function<void(Widget *)> on_select_widget;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 当前选中的 Widget 指针（nullptr 表示未选中）。
    [[nodiscard]] auto selected_widget() const -> Widget * { return selected_widget_; }

    /// @brief Export the current widget tree as C++ source code.
    auto export_code() const -> std::string;

    /// @brief Callback invoked when the "Export Code" button is clicked.
    std::function<void(const std::string &code)> on_export_code;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 当前属性面板内容（属性名值对列表，供自定义渲染/测试读取）。
    [[nodiscard]] auto current_props() const -> const std::vector<std::pair<std::string, std::string>> & {
        return prop_rows_;
    }

    auto serialize_props(Json &props) const -> void override;

    auto on_pointer_event(MouseEvent &e) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override;
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override;
    auto on_mount(const BuildContext &ctx) -> void override;

  private:
    static constexpr float AURORA_ROW_HEIGHT = 24.0F;
    static constexpr float AURORA_HEADER_HEIGHT = 28.0F;
    static constexpr float AURORA_HANDLE_SIZE = 5.0F;
    static constexpr float AURORA_PROP_INDENT = 8.0F;

    /// @brief 重建 TreeView items 与 widget 映射表。
    auto rebuild_tree() -> void;

    /// @brief 更新右侧属性面板内容。
    auto update_props_panel() -> void;

    /// @brief 处理 TreeView 区域点击（选中行 → 更新属性面板）。
    auto handle_tree_click(int local_y) -> void;

    /// @brief 处理属性面板区域点击（属性行 → 编辑/回写）。
    static auto handle_props_click(int local_y) -> void;

    /// @brief 计算可见行数（TreeView 展开序）。
    [[nodiscard]] auto visible_count() const -> std::size_t;

    /// @brief 获取可见行对应的 TreeItem 指针。
    [[nodiscard]] auto visible_item(int row) const -> const TreeItem *;
    [[nodiscard]] auto visible_item_mut(int row) -> TreeItem *;

    /// @brief 递归统计可见行数。
    static auto count_visible(const TreeItem &item, std::size_t &n) -> void;

    /// @brief 递归查找可见行对应 item。
    static auto find_visible(const TreeItem &item, int &idx) -> const TreeItem *;

    /// @brief 递归查找可见行对应 item（可变）。
    static auto find_visible_mut(TreeItem &item, int &idx) -> TreeItem *;

    /// @brief 获取可见行深度。
    [[nodiscard]] auto visible_depth(int row) const -> int;
    static auto depth_of(const TreeItem &item, int &idx, int depth) -> int;

    /// @brief 根据可见行号查找对应 Widget 指针。
    [[nodiscard]] auto widget_for_row(int row) const -> Widget *;

    std::function<Node()> root_getter_;  ///< 目标树获取函数
    Node target_root_;  ///< 缓存的目标树根节点
    std::vector<TreeItem> tree_items_;  ///< TreeView 数据
    std::vector<Widget *> widget_map_;  ///< 可见行号 → Widget 指针映射
    float ratio_ = 0.35F;  ///< 左侧树占比
    bool dragging_ = false;  ///< 分隔条拖拽中
    Widget *selected_widget_ = nullptr;  ///< 当前选中 Widget
    std::vector<std::pair<std::string, std::string>> prop_rows_;  ///< 属性名值对
    Size total_size_{.width = 0.0F, .height = 0.0F};  ///< 上次布局总尺寸
    bool needs_rebuild_ = true;  ///< 是否需要重建树
    Rect export_btn_rect_;  ///< Export Code 按钮命中区域
};

}  // namespace aurora
