#pragma once

#include <functional>
#include <string>

#include "aurora/core/types.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/**
 * @brief 拖放数据载荷（specification/05-event-navigation.md §5.2）。
 *
 * 携带 MIME 类型标识与 JSON 载荷，支持文本/控件/自定义数据拖放。
 */
struct DragData {
    std::string mime_type; ///< "text/plain", "aurora/widget", 自定义
    Json payload;          ///< 拖放数据（JSON 格式）

    /// @brief 构造文本拖放。
    [[nodiscard]] static auto text(const std::string &s) -> DragData {
        return DragData{ .mime_type = "text/plain", .payload = Json(s) };
    }

    /// @brief 构造控件树拖放（序列化 JSON）。
    [[nodiscard]] static auto widget_tree(Json tree_json) -> DragData {
        return DragData{ .mime_type = "aurora/widget", .payload = std::move(tree_json) };
    }

    /// @brief 是否为空（无数据）。
    [[nodiscard]] auto empty() const -> bool { return mime_type.empty(); }
};

/**
 * @brief 拖放会话状态（由事件派发器维护）。
 *
 * 追踪当前拖放操作的数据、源位置与目标。
 */
class DragSession {
  public:
    /// @brief 开始拖放会话。
    auto begin(DragData data, Point origin) -> void {
        m_data = std::move(data);
        m_origin = origin;
        m_active = true;
    }

    /// @brief 结束拖放会话。
    auto end() -> void {
        m_data = DragData{};
        m_active = false;
    }

    /// @brief 是否正在拖放中。
    [[nodiscard]] auto is_active() const -> bool { return m_active; }

    /// @brief 当前拖放数据。
    [[nodiscard]] auto data() const -> const DragData & { return m_data; }

    /// @brief 拖放起点。
    [[nodiscard]] auto origin() const -> Point { return m_origin; }

  private:
    DragData m_data;
    Point m_origin{ .x = 0.0f, .y = 0.0f };
    bool m_active = false;
};

/**
 * @brief 放置目标回调接口。
 *
 * 控件实现此接口以接受拖放：
 * - `on_drag_enter`：拖拽进入时调用，返回是否接受。
 * - `on_drag_leave`：拖拽离开时调用。
 * - `on_drop`：释放时调用，执行实际放置逻辑。
 */
struct DropTargetCallbacks {
    std::function<bool(const DragData &)> on_drag_enter;  ///< 是否接受
    std::function<void()> on_drag_leave;                  ///< 离开
    std::function<void(const DragData &, Point)> on_drop; ///< 放置（local_pos 为相对坐标）
};

} // namespace aurora
