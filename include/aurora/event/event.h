#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/types.h"

namespace aurora {

/// @note Thread: main-thread only
/// @note Side-effects: pure
/// @{

/// @brief 鼠标/触摸按键。
enum class MouseButton : std::uint8_t { Left, Right, Middle };

/// @brief 指针动作。
enum class MouseAction : std::uint8_t {
    Press,  ///< 按下（激活/点击）
    Release,  ///< 抬起
    Move,  ///< 移动（悬停/拖拽）
};

/// @brief 键盘动作。
enum class KeyAction : std::uint8_t { Down, Up };

/// @brief 键盘修饰键位（位掩码，可组合）。
enum class ModifierKey : std::uint8_t {
    None = 0,
    Shift = 1U << 0U,
    Control = 1U << 1U,
    Alt = 1U << 2U,
    Meta = 1U << 3U,
};

/// @brief 修饰键位按位或（便于组合 `modifiers`）。
[[nodiscard]] inline auto operator|(ModifierKey a, ModifierKey b) noexcept -> ModifierKey {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
    return static_cast<ModifierKey>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] inline auto operator&(ModifierKey a, ModifierKey b) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b);
}

/**
 * @brief 输入事件基类。
 *
 * 持有 `is_handled_` 标志：响应链中某级消费事件后置 true，派发器据此停止冒泡（specification/05-event-navigation.md
 * §2.1）。
 */
struct Event {
    bool is_handled = false;  ///< 是否已被消费（停止冒泡）

    Event() = default;
    virtual ~Event() = default;

    // Rule of Five：虚析构一旦由用户声明，拷贝/移动便不再隐式生成，故显式全部声明。
    // 派生事件在派发与手势处理中会被按值复制（如 `TouchEvent copy = e;`、
    // `MouseEvent release = press;`），这里保留默认语义（仅基类子对象参与，不发生多态切片）。
    Event(const Event &) = default;
    Event(Event &&) = default;
    auto operator=(const Event &) -> Event & = default;
    auto operator=(Event &&) -> Event & = default;
};

/// @brief 指针（鼠标/触摸）事件：坐标为相对根坐标系。
struct MouseEvent : Event {
    Point position{.x = 0.0F, .y = 0.0F};  ///< 全局（窗口逻辑）坐标，由 Surface 后端写入
    Point local_position{.x = 0.0F, .y = 0.0F};  ///< 相对当前控件的本地坐标，由 EventDispatcher 在命中链冒泡时写入
    MouseButton button = MouseButton::Left;
    MouseAction action = MouseAction::Press;
    /// @brief 触点 ID（多点触控场景由 TouchEvent 合成时写入，标识该次手势归属的指针；
    ///        鼠标/真实 MouseEvent 为 nullopt，表示「任意指针」）。用于 Draggable/LongPress
    ///        在并发触控下绑定到具体指针，避免同控件被第二根手指误触发。
    std::optional<int> pointer_id;
};

/// @brief 键盘事件：键码为平台无关的逻辑键码（见 event/keycode.h 的 KeyCode）。
struct KeyEvent : Event {
    int key = 0;
    KeyAction action = KeyAction::Down;
    ModifierKey modifiers = ModifierKey::None;  ///< 修饰键位组合（Shift/Ctrl/Alt/Meta）
};

/// @brief 滚轮事件（specification/05-event-navigation.md §2.2）。delta 为设备无关增量，y 正方向为向上滚动。
struct ScrollEvent : Event {
    Point position;  ///< 事件发生的逻辑坐标（鼠标所在处）
    float delta_x = 0;  ///< 水平滚动增量（右为正）
    float delta_y = 0;  ///< 垂直滚动增量（上为正）
};

/// @brief 文本输入事件（specification/05-event-navigation.md §2.2）：由键盘/输入法产生的 Unicode 文本片段。
struct TextInputEvent : Event {
    std::string text;  ///< UTF-8 文本片段
};

/// @brief 操作系统文件拖放事件（窗口级；位置为窗口逻辑坐标，specification/05-event-navigation.md §2.2）。
struct FileDropEvent : Event {
    Point position;  ///< 落点（窗口逻辑坐标）
    std::vector<std::string> paths;  ///< 被拖入的文件/目录绝对路径
};

/// @brief 单个触点（多点触控）。
struct TouchPoint {
    int id = 0;  ///< 触点唯一 ID（平台分配）
    Point position;  ///< 当前位置（逻辑坐标）
    Point prev_position;  ///< 上一帧位置
    bool is_active = true;  ///< 是否按下（false = 已抬起）
};

/// @brief 多点触控事件：携带所有活跃触点。
struct TouchEvent : Event {
    std::vector<TouchPoint> points;  ///< 当前所有触点（含已抬起的，active=false）

    /// @brief 活跃触点数。
    [[nodiscard]] auto active_count() const -> int {
        int n = 0;
        for (const auto &p : points) {
            if (p.is_active) {
                ++n;
            }
        }
        return n;
    }

    /// @brief 双指距离（仅 active_count>=2 时有意义）。
    [[nodiscard]] auto pinch_distance() const -> float {
        if (active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        return std::sqrt((dx * dx) + (dy * dy));
    }

    /// @brief 双指角度（弧度，仅 active_count>=2 时有意义）。
    [[nodiscard]] auto pinch_angle() const -> float {
        if (active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        return std::atan2(y2 - y1, x2 - x1);
    }

    /// @brief 按 id 查找触点（不存在返回 nullopt），用于手势在并发场景下锁定特定指针对。
    [[nodiscard]] auto point_by_id(int id) const -> std::optional<TouchPoint> {
        for (const auto &p : points) {
            if (p.id == id) {
                return p;
            }
        }
        return std::nullopt;
    }
};

/// @} // main-thread only, pure

}  // namespace aurora
