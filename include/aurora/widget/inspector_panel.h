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

/// @brief Inspector 面板（树形浏览器 + 属性编辑器）。
///
/// 左右分栏布局：左侧 TreeView 展示 Widget 层级树，右侧属性面板展示选中 Widget
/// 的类型名、属性描述与当前值。支持运行时属性回写（经 set_widget_prop）。
///
/// 构造时接受 `std::function<Node()>` 以获取目标 Widget 树根节点（支持动态树）。
/// 调用 `refresh()` 重建树映射；选中 TreeView 行时自动更新右侧属性面板。
///
/// 对标 Flutter Inspector / Chrome DevTools Elements 面板。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class InspectorPanel : public Container {
  public:
    InspectorPanel() = default;

    /// @brief 构造：接受目标树获取函数 + 可选初始比例。
    /// @param root_getter 目标 Widget 树根节点的获取函数（可为空；每次刷新时调用以支持动态树）。
    /// @param tree_ratio 初始左侧树宽度占比，构造时夹入 [0.1, 0.9]。
    explicit InspectorPanel(std::function<Node()> root_getter, float tree_ratio = 0.35F);

    /// @brief 返回控件类型名。
    /// @return 静态字符串 "InspectorPanel"。
    [[nodiscard]] auto type_name() const -> const char * override { return "InspectorPanel"; }

    /// @brief 自描述：类型名、ratio 属性（含默认值与合法域）、选中事件与不变量。
    /// @return 本控件的静态描述符（不依赖实例状态）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例自描述：内容等价 describe_static()。
    /// @return 本控件的 descriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 设置/更新目标树获取函数并刷新。
    /// @param getter 新的根节点获取函数（立即调用一次以缓存目标树，并标记重建）。
    auto set_root(std::function<Node()> getter) -> void;

    /// @brief 刷新树映射与属性面板。
    auto refresh() -> void;

    /// @brief 选中 Widget 的回调（可选，外部联动用）。
    std::function<void(Widget *)> on_select_widget;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 当前选中的 Widget 指针（nullptr 表示未选中）。
    /// @return 最近一次在树区选中的控件（非拥有）；未选中或从未点击时为 nullptr。
    [[nodiscard]] auto selected_widget() const -> Widget * { return selected_widget_; }

    /// @brief 把当前目标 Widget 树导出为 C++ 源码文本。
    /// @return 由整棵目标树的树 JSON 生成的 C++ 源码；目标树为空时返回空串。
    auto export_code() const -> std::string;

    /// @brief Callback invoked when the "Export Code" button is clicked.
    std::function<void(const std::string &code)> on_export_code;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 当前属性面板内容（属性名值对列表，供自定义渲染/测试读取）。
    /// @return 面板内部的属性行列表（本控件持有；随选中行变化于重绘前更新）。
    [[nodiscard]] auto current_props() const -> const std::vector<std::pair<std::string, std::string>> & {
        return prop_rows_;
    }

    /// @brief 序列化本控件属性：基类属性之外追加 ratio。
    /// @param props 输出目标 JSON 对象（就地写入 ratio 键，值为当前左侧树占比）。
    auto serialize_props(Json &props) const -> void override;

    /// @brief 处理指针事件：分隔条拖拽调比例、树区点击选中行、属性区点击（导出按钮/属性编辑）。
    /// @param e 鼠标事件（Press/Move/Release；命中的分支会置 e.is_handled 并更新布局）。
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
