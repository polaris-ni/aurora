#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/types.h"

namespace aurora {

// 分段说明：以下事件类型均为纯数据载荷，只在主线程构造与派发
// （契约 Thread: main-thread only、Side-effects: pure 逐类成立，此处统一声明）。

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
/// @param a 左侧掩码。
/// @param b 右侧掩码。
/// @return 两者位并集对应的 `ModifierKey`（组合值，非单个枚举量）。
[[nodiscard]] inline auto operator|(ModifierKey a, ModifierKey b) noexcept -> ModifierKey {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
    return static_cast<ModifierKey>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @brief 修饰键位按位与（判定某个修饰键是否按下）。
/// @param a 待测组合（如 `KeyEvent::modifiers`）。
/// @param b 掩码（单个修饰键位）。
/// @return 交集的原始位掩码；非 0 表示 `b` 落在 `a` 内。
[[nodiscard]] inline auto operator&(ModifierKey a, ModifierKey b) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b);
}

/// @brief 焦点到达方式：决定基类统一焦点环是否画出（specification/05-event-navigation.md §4.4）。
///
/// 由 `FocusManager::set_focus` 随焦点一并写入控件（`Widget::focus_arrival()`），控件与基类绘制
/// 路径据此判定可见性；本枚举刻意定义在本头而非 `event/focus.h`，因为 `Widget` 需在包含关系上
/// 先于焦点管理器看到它（`focus.h` 包含 `widget.h`，反向不可）。
///
/// - `Pointer`：指针 / 触摸按下把焦点交给控件。控件此刻已有 pressed / hover 反馈，再补一圈环会
///   被知觉归组成「控件自带的边框」，故不画环——与浏览器 `:focus-visible`、WinUI 的 `FocusVisual`、
///   Qt 的 `TabFocusReason` vs `MouseFocusReason` 同口径。
/// - `Keyboard`：经 Tab / Shift+Tab / 方向键移动而来。键盘可达性的可观测停点，必须画环。
/// - `Programmatic`：显式聚焦（`set_focus` / `request_focus` / 焦点作用域进出恢复等），无从判断
///   用户所处模态，保守按可见处理，因而既有调用点行为逐位不变。
enum class FocusArrival : std::uint8_t { Pointer, Keyboard, Programmatic };

/// @brief 输入事件基类。
///
/// 持有 `is_handled` 标志：响应链中某级消费事件后置 true，派发器据此停止冒泡（specification/05-event-navigation.md
/// §2.1）。
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
    MouseButton button = MouseButton::Left;  ///< 触发的鼠标键（后端派发时写入；由触点合成的事件一律记左键）
    MouseAction action = MouseAction::Press;  ///< 指针动作（按下/抬起/移动）
    /// @brief 触点 ID（多点触控场景由 TouchEvent 合成时写入，标识该次手势归属的指针；
    ///        鼠标/真实 MouseEvent 为 nullopt，表示「任意指针」）。用于 Draggable/LongPress
    ///        在并发触控下绑定到具体指针，避免同控件被第二根手指误触发。
    std::optional<int> pointer_id;
};

/// @brief 键盘事件：键码为平台无关的逻辑键码（见 event/keycode.h 的 KeyCode）。
struct KeyEvent : Event {
    int key = 0;  ///< 逻辑键码的整数值（按 `KeyCode` 解释，见 event/keycode.h）；0 == KeyCode::Unknown（未映射）
    KeyAction action = KeyAction::Down;  ///< 键盘动作（按下/抬起）
    ModifierKey modifiers = ModifierKey::None;  ///< 修饰键位组合（Shift/Ctrl/Alt/Meta）
};

/// @brief 滚轮事件（specification/05-event-navigation.md §2.2）。delta 为设备无关增量，y 正方向为向上滚动。
struct ScrollEvent : Event {
    Point position;  ///< 事件发生的逻辑坐标（鼠标所在处）
    float delta_x = 0;  ///< 水平滚动增量（右为正）
    float delta_y = 0;  ///< 垂直滚动增量（上为正）
    /// @brief 消费后未用尽的垂直余量（与 delta_y 同单位、同号；嵌套滚动协调的回传通道）。
    /// 消费方在 `on_scroll` 内写入本字段声明「我只吃了这么多」，派发器据此沿命中链
    /// 向更浅可滚动祖先继续派发（见 `EventDispatcher::dispatch(Widget&, ScrollEvent&)`）。
    /// **默认 0 = 全量消费**：不写本字段的既有自定义 handler 行为与「最深可滚动者
    /// 一次性消费、不冒泡」的旧约定逐位一致。
    float remaining_y = 0;
};

/// @brief 文本输入事件（specification/05-event-navigation.md §2.2）：由键盘/输入法产生的 Unicode 文本片段。
struct TextInputEvent : Event {
    std::string text;  ///< UTF-8 文本片段
};

/// @brief 输入法（IME）组合事件（CJK 攻坚）。
///
/// 与 `TextInputEvent` 并存、职责分离：
/// - **已提交文本**（上屏）走 `TextInputEvent`；
/// - **组合态**（preedit / 候选 / 组合内选区）走本事件。
///
/// 一次中文输入的典型序列（`TextInput` / `RichTextEdit` 均按此消费）：
/// 1. `preedit="ni hao"` → 显示带下划线的预编辑串；
/// 2. `preedit="你好"`、`cursor_index=2` → 候选替换预编辑串，光标停在候选插入点；
/// 3. `preedit=""`、`committed="你好"` → 预编辑串落字为正式文本，组合结束。
///
/// `sel_start` / `sel_end` 描述 **preedit 内部** 的选中区间（含头含尾，码点下标），
/// 用于输入法高亮「待转换的拼音片段」；无区间时 `sel_end == AURORA_NO_SELECTION`。
struct TextCompositionEvent : Event {
    /// @brief preedit 内「无选区」哨兵（与 `TextInput::AURORA_NO_SEL` 同语义，独立定义以免跨头依赖）。
    static constexpr std::size_t AURORA_NO_SELECTION = static_cast<std::size_t>(-1);

    std::string preedit;  ///< 预编辑串（UTF-8）；空串 = 组合结束 / 取消
    std::size_t cursor_index = 0;  ///< 光标在 preedit 内的码点下标（候选插入点）
    std::size_t sel_start = 0;  ///< preedit 内选中区间起点（码点下标）
    std::size_t sel_end = AURORA_NO_SELECTION;  ///< preedit 内选中区间终点（含尾）；AURORA_NO_SELECTION = 无
    std::string committed;  ///< 本次随组合一并上屏的文本（UTF-8），可为空

    /// @brief preedit 内是否存在选中区间。
    /// @return `sel_end != AURORA_NO_SELECTION` 时为 true（区间含头含尾）。
    [[nodiscard]] auto has_preedit_selection() const -> bool { return sel_end != AURORA_NO_SELECTION; }
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
    /// @return `points` 中 `is_active == true` 的个数（无活跃触点时为 0）。
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
    /// @return `points[0]` 与 `points[1]` 当前位置（逻辑坐标）的欧氏距离；不足两个活跃触点时 0.0F。
    [[nodiscard]] auto pinch_distance() const -> float {
        // 同时判 points.size()<2：既保证下标访问不越界，又让 GCC 的 -Warray-bounds 能据此
        // 证明 points[1] 合法（active_count>=2 已蕴含 size>=2，二者语义等价，故不改变返回值）。
        if (points.size() < 2 || active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        return std::sqrt((dx * dx) + (dy * dy));
    }

    /// @brief 双指角度（弧度，仅 active_count>=2 时有意义）。
    /// @return `atan2(dy, dx)`：从 `points[0]` 指向 `points[1]` 的连线与 +x 轴夹角；不足两个活跃触点时 0.0F。
    [[nodiscard]] auto pinch_angle() const -> float {
        // 同 pinch_distance：先判 points.size()<2 以向 -Warray-bounds 证明下标合法。
        if (points.size() < 2 || active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        return std::atan2(y2 - y1, x2 - x1);
    }

    /// @brief 按 id 查找触点（不存在返回 nullopt），用于手势在并发场景下锁定特定指针对。
    /// @param id 触点唯一 ID（平台分配，见 `TouchPoint::id`）。
    /// @return 首个 `id` 相同的触点副本；无匹配时为 `std::nullopt`。
    [[nodiscard]] auto point_by_id(int id) const -> std::optional<TouchPoint> {
        for (const auto &p : points) {
            if (p.id == id) {
                return p;
            }
        }
        return std::nullopt;
    }
};

}  // namespace aurora
