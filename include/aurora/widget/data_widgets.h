#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 表格列描述。
struct DataColumn {
    std::string label;  ///< 表头文本
    float width = 100.0F;  ///< 列宽(dp)
    bool sortable = false;  ///< 是否可点击表头排序
};

/// @brief 排序方向。
enum class SortOrder : std::uint8_t { None, Ascending, Descending };

/**
 * @brief 数据表格：字符串单元格的表格展示。
 *
 * 行数据为字符串二维数组（序列化友好）；列排序回调 `on_sort(col, order)`
 * 由调用方对数据源重排后 `set_rows` 回填（单向数据流）。行选择（单选）。
 *
 * 对标 Qt `QTableView`、WPF `DataGrid`、Flutter `DataTable`、SwiftUI `Table`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class DataTable : public Widget {
  public:
    DataTable() = default;
    DataTable(std::vector<DataColumn> columns, std::vector<std::vector<std::string>> rows)
        : columns_(std::move(columns)), rows_(std::move(rows)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "DataTable"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_row_); }

    [[nodiscard]] auto column_count() const -> std::size_t { return columns_.size(); }
    [[nodiscard]] auto row_count() const -> std::size_t { return rows_.size(); }
    [[nodiscard]] auto selected_row() -> State<int> & { return selected_row_; }
    [[nodiscard]] auto selected_row_index() const -> int { return selected_row_.get(); }
    [[nodiscard]] auto sort_column() const -> int { return sort_column_; }
    [[nodiscard]] auto sort_order() const -> SortOrder { return sort_order_; }

    /// @brief 单元格文本（越界返回空串）。
    [[nodiscard]] auto cell(std::size_t row, std::size_t col) const -> std::string {
        if (row >= rows_.size() || col >= rows_[row].size()) {
            return {};
        }
        return rows_[row][col];
    }

    /// @brief 替换行数据（排序回填/数据刷新）。
    auto set_rows(std::vector<std::vector<std::string>> rows) -> void {
        rows_ = std::move(rows);
        if (std::cmp_greater_equal(selected_row_.get(), rows_.size())) {
            selected_row_.set(-1);
        }
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 选中行（-1 取消选中；越界忽略）。
    auto select_row(int row) -> void {
        if (row >= -1 && std::cmp_less(row, rows_.size()) && row != selected_row_.get()) {
            selected_row_.set(row);
            mark_needs_paint();
            if (on_select_) {
                on_select_(row);
            }
        }
    }

    /// @brief 点击表头排序：同列循环 Asc→Desc→Asc；换列重置 Asc。
    auto sort_by(int col) -> void {
        if (col < 0 || std::cmp_greater_equal(col, columns_.size())) {
            return;
        }
        if (!columns_[static_cast<std::size_t>(col)].sortable) {
            return;
        }
        if (sort_column_ == col) {
            sort_order_ = sort_order_ == SortOrder::Ascending ? SortOrder::Descending : SortOrder::Ascending;
        } else {
            sort_column_ = col;
            sort_order_ = SortOrder::Ascending;
        }
        mark_needs_paint();
        if (on_sort_) {
            on_sort_(col, sort_order_);
        }
    }

    auto set_on_sort(std::function<void(int, SortOrder)> cb) -> DataTable & {
        on_sort_ = std::move(cb);
        return *this;
    }
    auto set_on_select(std::function<void(int)> cb) -> DataTable & {
        on_select_ = std::move(cb);
        return *this;
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

  private:
    static constexpr float AURORA_HEADER_HEIGHT = 32.0F;
    static constexpr float AURORA_ROW_HEIGHT = 28.0F;

    std::vector<DataColumn> columns_;
    std::vector<std::vector<std::string>> rows_;
    State<int> selected_row_{-1};
    int sort_column_ = -1;
    SortOrder sort_order_ = SortOrder::None;
    std::function<void(int, SortOrder)> on_sort_;
    std::function<void(int)> on_select_;
};

/// @brief 树节点（递归结构）。
struct TreeItem {
    std::string label;
    std::vector<TreeItem> children;
    bool expanded = false;

    [[nodiscard]] auto is_leaf() const -> bool { return children.empty(); }
};

/**
 * @brief 树形视图：层级树展示 + 展开/折叠 + 行选中。
 *
 * 可见行 = 深度优先展开序（折叠节点子树不展开）。选中以「可见行号」表达，
 * `selected_path()` 返回选中项的标签路径。
 *
 * 对标 Qt `QTreeView`、WPF `TreeView`、SwiftUI `OutlineGroup`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class TreeView : public Widget {
  public:
    TreeView() = default;
    explicit TreeView(std::vector<TreeItem> roots) : roots_(std::move(roots)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "TreeView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_); }

    [[nodiscard]] auto roots() -> std::vector<TreeItem> & { return roots_; }
    [[nodiscard]] auto selected() -> State<int> & { return selected_; }
    [[nodiscard]] auto selected_row() const -> int { return selected_.get(); }

    /// @brief 可见行数（展开序）。
    [[nodiscard]] auto visible_count() const -> std::size_t {
        std::size_t n = 0;
        for (const auto &r : roots_) {
            count_visible(r, n);
        }
        return n;
    }

    /// @brief 可见行的标签（越界空串）与缩进深度。
    [[nodiscard]] auto visible_label(int row) const -> std::string {
        const TreeItem *item = visible_item(row);
        return item != nullptr ? item->label : std::string{};
    }

    /// @brief 选中可见行（触发 on_select）。
    auto select(int row) -> void {
        if (row >= -1 && std::cmp_less(row, visible_count()) && row != selected_.get()) {
            selected_.set(row);
            mark_needs_paint();
            if (on_select_) {
                on_select_(row);
            }
        }
    }

    /// @brief 展开/折叠可见行（叶节点无操作；触发 on_toggle）。
    auto toggle(int row) -> void {
        TreeItem *item = visible_item_mut(row);
        if (item == nullptr || item->is_leaf()) {
            return;
        }
        item->expanded = !item->expanded;
        mark_needs_layout();
        mark_needs_paint();
        if (on_toggle_) {
            on_toggle_(row, item->expanded);
        }
    }

    auto set_on_select(std::function<void(int)> cb) -> TreeView & {
        on_select_ = std::move(cb);
        return *this;
    }
    auto set_on_toggle(std::function<void(int, bool)> cb) -> TreeView & {
        on_toggle_ = std::move(cb);
        return *this;
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 可见行的缩进深度。
    [[nodiscard]] auto visible_depth(int row) const -> int {
        int idx = row;
        for (const auto &r : roots_) {
            const int d = depth_of(r, idx, 0);
            if (d >= 0) {
                return d;
            }
        }
        return 0;
    }

    auto serialize_props(Json &props) const -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

  private:
    static constexpr float AURORA_ROW_HEIGHT = 26.0F;
    static constexpr float AURORA_INDENT = 18.0F;
    static constexpr float AURORA_ARROW_ZONE = 20.0F;

    static auto count_visible(const TreeItem &item, std::size_t &n) -> void {
        ++n;
        if (item.expanded) {
            for (const auto &ch : item.children) {
                count_visible(ch, n);
            }
        }
    }

    /// @brief 按可见序取第 idx 个节点（递减 idx；命中返回指针）。
    static auto find_visible(const TreeItem &item, int &idx) -> const TreeItem * {
        if (idx == 0) {
            return &item;
        }
        --idx;
        if (item.expanded) {
            for (const auto &ch : item.children) {
                const TreeItem *r = find_visible(ch, idx);
                if (r != nullptr) {
                    return r;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto visible_item(int row) const -> const TreeItem * {
        if (row < 0) {
            return nullptr;
        }
        int idx = row;
        for (const auto &r : roots_) {
            const TreeItem *found = find_visible(r, idx);
            if (found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto visible_item_mut(int row) const -> TreeItem * {
        return const_cast<TreeItem *>(visible_item(row));  // NOLINT：内部可变访问
    }

    /// @brief 可见序中第 idx 个节点的深度（找到返回深度并把 idx 置 -1，未找到返回 -1）。
    static auto depth_of(const TreeItem &item, int &idx, int depth) -> int {
        if (idx == 0) {
            idx = -1;
            return depth;
        }
        --idx;
        if (item.expanded) {
            for (const auto &ch : item.children) {
                const int d = depth_of(ch, idx, depth + 1);
                if (d >= 0) {
                    return d;
                }
            }
        }
        return -1;
    }

    std::vector<TreeItem> roots_;
    State<int> selected_{-1};
    std::function<void(int)> on_select_;
    std::function<void(int, bool)> on_toggle_;
};

/**
 * @brief 列表视图：数据模型 + 委托渲染 + 选择语义。
 *
 * 与 `Repeater` 区分：`Repeater` 是纯 UI 重复器；`ListView` 绑定字符串数据模型、
 * 内置行选择（单选/多选）与删除操作。委托为可选行构建器（缺省渲染文本行）。
 *
 * 对标 Qt `QListView`+delegate、WPF `ListBox`、SwiftUI `List`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ListView : public Widget {
  public:
    ListView() = default;
    explicit ListView(std::vector<std::string> items, bool multi_select = false)
        : items_(std::move(items)), multi_(multi_select) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ListView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    [[nodiscard]] auto item_count() const -> std::size_t { return items_.size(); }
    [[nodiscard]] auto item(std::size_t i) const -> std::string {
        return i < items_.size() ? items_[i] : std::string{};
    }

    /// @brief 选中行集合（升序）。
    [[nodiscard]] auto selection() const -> const std::vector<int> & { return selection_; }
    [[nodiscard]] auto is_selected(int row) const -> bool {
        return std::ranges::find(selection_, row) != selection_.end();
    }

    /// @brief 选中行：单选替换、多选切换。
    auto select(int row) -> void {
        if (row < 0 || std::cmp_greater_equal(row, items_.size())) {
            return;
        }
        if (multi_) {
            const auto it = std::ranges::find(selection_, row);
            if (it != selection_.end()) {
                selection_.erase(it);
            } else {
                selection_.push_back(row);
                std::ranges::sort(selection_);
            }
        } else {
            selection_.assign(1, row);
        }
        mark_needs_paint();
        if (on_select_) {
            on_select_(row);
        }
    }

    auto clear_selection() -> void {
        selection_.clear();
        mark_needs_paint();
    }

    /// @brief 删除行（选中集合随之修正；触发 on_remove）。
    auto remove(int row) -> void {
        if (row < 0 || std::cmp_greater_equal(row, items_.size())) {
            return;
        }
        items_.erase(items_.begin() + row);
        // 修正选中：删除项移除，之后的行号前移
        std::vector<int> fixed;
        for (int s : selection_) {
            if (s == row) {
                continue;
            }
            fixed.push_back(s > row ? s - 1 : s);
        }
        selection_ = std::move(fixed);
        mark_needs_layout();
        mark_needs_paint();
        if (on_remove_) {
            on_remove_(row);
        }
    }

    /// @brief 追加行。
    auto append(std::string item) -> void {
        items_.push_back(std::move(item));
        mark_needs_layout();
    }

    auto set_on_select(std::function<void(int)> cb) -> ListView & {
        on_select_ = std::move(cb);
        return *this;
    }
    auto set_on_remove(std::function<void(int)> cb) -> ListView & {
        on_remove_ = std::move(cb);
        return *this;
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override;

    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

  private:
    static constexpr float AURORA_ROW_HEIGHT = 26.0F;

    std::vector<std::string> items_;
    bool multi_ = false;
    std::vector<int> selection_;
    std::function<void(int)> on_select_;
    std::function<void(int)> on_remove_;
};

}  // namespace aurora
