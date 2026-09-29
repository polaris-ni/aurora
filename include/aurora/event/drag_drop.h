#pragma once

#include <functional>
#include <string>

#include "aurora/core/types.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/// @brief 拖放数据载荷（specification/05-event-navigation.md §5.2）。
///
/// 携带 MIME 类型标识与 JSON 载荷，支持文本/控件/自定义数据拖放。
struct DragData {
    std::string mime_type;  ///< "text/plain", "aurora/widget", 自定义
    Json payload;  ///< 拖放数据（JSON 格式）

    /// @brief 构造文本拖放。
    /// @param s 文本内容，包装为 JSON 字符串存入载荷。
    /// @return MIME 类型为 `text/plain` 的拖放载荷。
    [[nodiscard]] static auto text(const std::string &s) -> DragData {
        return DragData{.mime_type = "text/plain", .payload = Json(s)};
    }

    /// @brief 构造控件树拖放（序列化 JSON）。
    /// @param tree_json 已序列化的控件树 JSON，移动存入载荷。
    /// @return MIME 类型为 `aurora/widget` 的拖放载荷。
    [[nodiscard]] static auto widget_tree(Json tree_json) -> DragData {
        return DragData{.mime_type = "aurora/widget", .payload = std::move(tree_json)};
    }

    /// @brief 是否为空（无数据）。
    /// @return mime_type 为空串（未装载任何数据）时为 true。
    [[nodiscard]] auto empty() const -> bool { return mime_type.empty(); }
};

/// @brief 拖放会话状态（由事件派发器维护）。
///
/// 追踪当前拖放操作的数据、源位置与目标。
class DragSession {
  public:
    /// @brief 开始拖放会话。
    /// @param data 被拖动的数据载荷（移入会话，直到 end 清空）。
    /// @param origin 拖放起点坐标。
    auto begin(DragData data, Point origin) -> void {
        data_ = std::move(data);
        origin_ = origin;
        active_ = true;
    }

    /// @brief 结束拖放会话。
    auto end() -> void {
        data_ = DragData{};
        active_ = false;
    }

    /// @brief 是否正在拖放中。
    /// @return begin 置位、end 复位；仅会话期间为 true。
    [[nodiscard]] auto is_active() const -> bool { return active_; }

    /// @brief 当前拖放数据。
    /// @return 会话载荷的 const 引用；end 后为置空的 DragData。
    [[nodiscard]] auto data() const -> const DragData & { return data_; }

    /// @brief 拖放起点。
    /// @return 最近一次 begin 传入的坐标（end 不清除，未 begin 过为原点 (0,0)）。
    [[nodiscard]] auto origin() const -> Point { return origin_; }

  private:
    DragData data_;
    Point origin_{.x = 0.0F, .y = 0.0F};
    bool active_ = false;
};

/// @brief 放置目标回调接口。
///
/// 控件实现此接口以接受拖放：
/// - `on_drag_enter`：拖拽进入时调用，返回是否接受。
/// - `on_drag_leave`：拖拽离开时调用。
/// - `on_drop`：释放时调用，执行实际放置逻辑。
struct DropTargetCallbacks {
    std::function<bool(const DragData &)> on_drag_enter;  ///< 是否接受
    std::function<void()> on_drag_leave;  ///< 离开
    std::function<void(const DragData &, Point)> on_drop;  ///< 放置（local_pos 为相对坐标）
};

}  // namespace aurora
