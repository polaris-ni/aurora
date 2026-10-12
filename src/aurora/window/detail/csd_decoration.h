#pragma once

// csd_decoration.h —— CSD 装饰的「策略 / 交互 / 状态装配」层（跨后端共享，仅库实现可见，不属公共 API）。
//
// 为何单列：CHG-001 收敛了几何（`csd_geometry.h`）与绘制（`title_bar_painter.h`）两处真源，但
// 「意图怎么变成这三个布尔」「指针进来先判谁」「全屏揭示条何时展开」这些**策略与交互**仍埋在
// `WaylandSurface` 的两千行里。结果是：任何第二个想吃 CSD 的后端（X11/Win32）只能把同一套命中
// 顺序、连击窗口、悬停序号约定再抄一遍——抄的那份必然漂移，而漂移的代价是「热区错位」这类
// 已经在 `title_bar_geometry()` 上修过一次的缺陷复发。故把它们抽成本层的纯值/纯函数，后端只做
// 「平台事件 → 本层入参 → 平台动作」的映射。
//
// 命名为何取 decoration 而非 chrome：UI 行话里「chrome」的确指内容之外的窗户口沿（浏览器 chrome /
// 窗口 chrome），但本库公共 API **已有** `WindowChrome`（`window/window_chrome.h`，窗口动作门面，
// 与几何 / 状态无关），两者同姓会把「服务门面」与「本层的纯值策略」混为一谈，
// 故本层改用与 `DecorationPolicy` 同源的 decoration。
//
// 本层边界（刻意）：
// - 不持有任何 wl_* / HWND / Window 句柄，不 include 任何平台头，故可在无合成器的进程里单测；
// - 不推导任何矩形：命中判定一律消费 `title_bar_geometry()` 的结果，与绘制层同源；
// - 不改变任何既有语义：下面的每个函数都与抽层前 `WaylandSurface` 的对应分支逐值等价，
//   等价性由 `utest_csd_decoration` 的策略快照与命中/连击用例锁定。

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "aurora/core/image.h"  // Image（标题栏图标像素）
#include "aurora/core/types.h"  // Size / Rect / EdgeInsets
#include "aurora/event/event.h"  // 连击判定的库级常量（单一真源）
#include "aurora/window/detail/csd_geometry.h"  // 阴影边距 / 热区分类 / content_inset
#include "aurora/window/detail/title_bar_painter.h"  // TitleBarPaintState（本层的产出目标）
#include "aurora/window/surface.h"  // DecorationPolicy
#include "aurora/window/title_bar_geometry.h"  // TitleBarGeometry / title_bar_geometry
#include "aurora/window/title_bar_style.h"  // TitleBarStyle
#include "aurora/window/window_state.h"  // WindowMode

namespace aurora::csd {

/// @brief 标题栏按钮序号（悬停态与命中序号的公共契约；`TitleBarPaintState::hovered_button` 同序）。
inline constexpr int AURORA_CSD_BUTTON_MINIMIZE = 0;
/// @brief 最大化按钮序号（悬停态与命中序号的公共契约）。
inline constexpr int AURORA_CSD_BUTTON_MAXIMIZE = 1;
/// @brief 关闭按钮序号（悬停态与命中序号的公共契约）。
inline constexpr int AURORA_CSD_BUTTON_CLOSE = 2;
/// @brief 无悬停 / 无命中的哨兵序号。
inline constexpr int AURORA_CSD_BUTTON_NONE = -1;

// ── 1 策略解析 ────────────────────────────────────────────────────────────────

/// @brief 归一化的装饰策略：把 `WindowStyleOptions` 的两个输入折叠成一个决策输入。
/// @param frameless `WindowStyleOptions::frameless` 旧字段（`true` 等价于 Frameless）。
/// @param decoration `WindowStyleOptions::decoration` 策略字段。
/// @return 生效的装饰策略。
[[nodiscard]] inline auto normalize_policy(bool frameless, DecorationPolicy decoration) -> DecorationPolicy {
    return frameless ? DecorationPolicy::Frameless : decoration;
}

/// @brief 装饰形态三元组：`DecorationPolicy` 与「本后端能否提供服务端装饰」的唯一解析结果。
struct CsdMode {
    bool title = false;  ///< 自绘标题栏（提供拖拽移动 + 关闭/最大/最小化按钮）。
    bool border = false;  ///< 自绘可缩放边框（承担边缘拖拽缩放）。
    bool mod_move = false;  ///< 无标题栏时的兜底：按住 Super / Alt 拖拽任意处移动窗口。
    /// @brief 是否需要自绘任何装饰（标题栏或边框）——决定阴影边距的存在与否。
    /// @return 标题栏或边框任一为真时 true。
    [[nodiscard]] auto client_decorated() const -> bool { return title || border; }
    /// @brief 是否需要接管指针（标题栏命中 / 边缘缩放 / 修饰键移动三者任一）。
    /// @return 三者任一为真时 true。
    [[nodiscard]] auto takes_pointer() const -> bool { return title || border || mod_move; }
};

/// @brief 解析装饰形态（原 `WaylandSurface` 构造期的 switch，抽此为求真源的唯一处）。
///
/// 语义（逐条保留）：
/// - `Auto` / `ServerSide`：有 SSD 则用服务端装饰（三项全 false）；无 SSD 时退化为 CSD 兜底，
///   以免窗口无标题栏且不可移动不可关闭——「 ServerSide 不可用时也必须能操作」是这条的动机；
/// - `ClientSide`：即便合成器支持 SSD 也自绘标题栏；边框分量由阴影边距承担（见 `CsdMode::border`）；
/// - `Borderless`：无标题栏但保留缩放热区，移动改由修饰键拖拽提供；
/// - `Frameless`：完全无装饰，移动同样靠修饰键拖拽。
/// @param policy 归一化后的装饰策略（先过 `normalize_policy`）。
/// @param ssd_available 本后端这次能否协商到服务端装饰（Wayland 即合成器是否广播 xdg-decoration）。
/// @return 装饰形态三元组。
[[nodiscard]] inline auto resolve_mode(DecorationPolicy policy, bool ssd_available) -> CsdMode {
    CsdMode m;
    switch (policy) {
        case DecorationPolicy::ServerSide:
        case DecorationPolicy::Auto:
            m.title = !ssd_available;
            m.border = m.title;
            break;
        case DecorationPolicy::ClientSide:
            m.title = true;
            m.border = false;
            break;
        case DecorationPolicy::Borderless:
            m.title = false;
            m.border = true;
            m.mod_move = true;
            break;
        case DecorationPolicy::Frameless:
            m.title = false;
            m.border = false;
            m.mod_move = true;
            break;
    }
    return m;
}

// ── 2 交互状态机 ──────────────────────────────────────────────────────────────

/// @brief 装饰层自持的可变交互状态（每个窗口一份，跨帧存活）。
struct DecorationInteraction {
    int hovered_button = AURORA_CSD_BUTTON_NONE;  ///< 当前悬停按钮序号（`-1` = 无）。
    int pressed_button = AURORA_CSD_BUTTON_NONE;  ///< 当前按压按钮序号（`-1` = 未按）。
    bool fullscreen_revealed = false;  ///< 全屏顶边揭示条是否展开（覆盖层，不回流布局）。
    bool grab = false;  ///< 是否处于拖拽交互中（此间吞噬指针事件，不转发应用）。
    std::uint32_t last_click_time = 0;  ///< 上次标题栏按下的时间戳（ms，取指针事件的时间戳口径）。
    double last_click_x = 0.0;  ///< 上次标题栏按下的 x（内容坐标）。
    double last_click_y = 0.0;  ///< 上次标题栏按下的 y（内容坐标）。
};

/// @brief 判定内容坐标点是否命中标题栏按钮，并回其序号。
///
/// 热区即 `title_bar_geometry()` 的矩形本身（绘制与命中共用同一份几何，不允许各自推矩形）。
/// 宽度为零的矩形表示「该按钮隐藏」（`!resizable` 下的最大化钮、`show_*` 关闭的钮），恒不命中。
/// @param g 本帧的标题栏几何（`title_bar_geometry` 的输出）。
/// @param x 指针 x（内容坐标）。
/// @param y 指针 y（内容坐标）。
/// @return 命中序号（`AURORA_CSD_BUTTON_*`）；未命中返回 `AURORA_CSD_BUTTON_NONE`。
[[nodiscard]] inline auto hit_button(const TitleBarGeometry &g, double x, double y) -> int {
    const auto in_box = [x, y](const Rect &r) {
        return r.size.width > 0.0F && x >= r.origin.x && x < r.origin.x + r.size.width && y >= r.origin.y &&
               y < r.origin.y + r.size.height;
    };
    if (in_box(g.close)) {
        return AURORA_CSD_BUTTON_CLOSE;
    }
    if (in_box(g.maximize)) {
        return AURORA_CSD_BUTTON_MAXIMIZE;
    }
    if (in_box(g.minimize)) {
        return AURORA_CSD_BUTTON_MINIMIZE;
    }
    return AURORA_CSD_BUTTON_NONE;
}

/// @brief 按指针位置重算悬停按钮（原 `ptr_motion` 的悬停跟踪段）。
///
/// 非自绘标题栏时恒返回 `-1`——此时本就无按钮可悬停，且清空是安全的：策略在窗口构造期即定档，
/// 不存在「上一帧有标题栏、这一帧没有」的中间态。
/// @param style 当前标题栏样式（提供栏高与按钮几何输入）。
/// @param width 内容区宽度（逻辑 dp；`title_bar_geometry` 的水平基准）。
/// @param maximized 是否最大化（仅决定最大化钮字形，不影响盒位）。
/// @param resizable 是否可缩放（false 时最大化盒为空，恒不命中）。
/// @param csd_title 本窗口是否自绘标题栏。
/// @param x 指针 x（内容坐标）。
/// @param y 指针 y（内容坐标）。
/// @return 新的悬停序号（`-1` = 无）；调用方据此判定是否需要重绘装饰层。
[[nodiscard]] inline auto resolve_hover(const TitleBarStyle &style, float width, bool maximized, bool resizable,
                                        bool csd_title, double x, double y) -> int {
    if (!csd_title) {
        return AURORA_CSD_BUTTON_NONE;
    }
    // 标题栏垂直带之外：不再逐个试探按钮（等价于原实现的「先判带、带内才查按钮」）。
    if (y < 0.0 || y >= static_cast<double>(style.height)) {
        return AURORA_CSD_BUTTON_NONE;
    }
    return hit_button(title_bar_geometry(width, style, maximized, resizable), x, y);
}

/// @brief 全屏顶边揭示条状态机（近顶 6dp 展开，指针下离「栏高 + 4dp」后收回；非全屏一律复位）。
/// @param revealed 当前揭示态。
/// @param mode 当前窗口几何态（仅 `FullScreen` 参与该状态机）。
/// @param y 指针 y（内容坐标）。
/// @param bar_height 标题栏高度（逻辑 dp）。
/// @return 新的揭示态；调用方据此判定是否需要重绘装饰层。
[[nodiscard]] inline auto update_fullscreen_reveal(bool revealed, WindowMode mode, double y, float bar_height) -> bool {
    if (mode != WindowMode::FullScreen) {
        return false;  // 退出全屏必须复位，否则揭示条会残留在普通窗口上
    }
    if (!revealed && y < 6.0) {
        return true;
    }
    if (revealed && y > static_cast<double>(bar_height) + 4.0) {
        return false;
    }
    return revealed;
}

/// @brief 连击判定并就地记账（标题栏双击最大化；Wayland 无原生双击事件，须客户端自测）。
///
/// 阈值直接引自 `event` 层的库级常量，窗口装饰层与 widget 树的 `MouseEvent::click_count` 因此共用同一份真源。
/// **无论判定结果如何都会更新上次点击的时点/位置**——这是「第二次点击才成立」的前提。
/// @param st 交互状态（就地更新 `last_click_*`）。
/// @param timestamp 本次按下事件的时戳（ms）。
/// @param x 本次按下 x（内容坐标）。
/// @param y 本次按下 y（内容坐标）。
/// @return 构成连击（二次点击且落在时间窗与半径内）时 true。
[[nodiscard]] inline auto consume_double_click(DecorationInteraction &st, std::uint32_t timestamp, double x, double y)
    -> bool {
    const bool is_dblclick = (timestamp - st.last_click_time < AURORA_DEFAULT_CLICK_WINDOW_MS) &&
                             std::hypot(x - st.last_click_x, y - st.last_click_y) < AURORA_DEFAULT_CLICK_RADIUS_DP;
    st.last_click_time = timestamp;
    st.last_click_x = x;
    st.last_click_y = y;
    return is_dblclick;
}

// ── 3 按压结算 ────────────────────────────────────────────────────────────────

/// @brief 记录按钮按下：**只置按压态，不触发任何窗口动作**。
///
/// 动作推迟到释放（见 `consume_press`）是必要的，不是风格选择：CSD 按钮若按下即触发，
/// 「按压态」就没有可渲染的时间窗（按下与关闭/最大化在同一帧发生），样式里的
/// `pressed_tint` / `close_pressed` 会永远画不出来。释放触发同时带来业界一致的
/// 「按错可以拖开取消」语义（GTK / Win32 均如此）。
/// @param st 交互状态（就地置 `pressed_button` 与 `grab`）。
/// @param btn 本次按下的按钮序号（`AURORA_CSD_BUTTON_*`；`NONE` 表示按在标题栏空白处）。
inline auto begin_press(DecorationInteraction &st, int btn) -> void {
    st.pressed_button = btn;
    st.grab = true;  // 与拖拽移动同路：此间吞噬指针事件，避免把按钮上的拖拽转给应用
}

/// @brief 释放时结算按压，回应当触发的按钮序号。
///
/// 仅当「按下与释落在同一枚按钮上」才成立——按下关闭钮后拖开再松手**不关闭窗口**，
/// 这是释放触发语义自带的纠错能力。
/// @param st 交互状态（就地清空 `pressed_button` 与 `grab`）。
/// @param hovered_now 释放那一刻指针所在按钮序号（由 `resolve_hover` 求得）。
/// @return 应触发的按钮序号；无需触发返回 `AURORA_CSD_BUTTON_NONE`。
[[nodiscard]] inline auto consume_press(DecorationInteraction &st, int hovered_now) -> int {
    const int pressed = st.pressed_button;
    st.pressed_button = AURORA_CSD_BUTTON_NONE;
    st.grab = false;
    return (pressed != AURORA_CSD_BUTTON_NONE && pressed == hovered_now) ? pressed : AURORA_CSD_BUTTON_NONE;
}

// ── 4 绘制状态装配 ────────────────────────────────────────────────────────────

/// @brief 一帧装饰所需的全部宿主状态（后端 → 本层的输入；其余 SOURCES 一律由本层自持）。
///
/// 成员均为值语义：`std::string` / `TitleBarStyle` / `shared_ptr` 的搬运成本与抽层前
/// `decoration_state()` 内部的逐字段赋值同量级（`std::move` 入参即可免二次拷贝）。
struct DecorationInput {
    Size size{};  ///< 内容区尺寸（window geometry 口径，逻辑 dp）
    WindowMode mode = WindowMode::Normal;  ///< 窗口几何态（决定标题栏显隐与最大化字形）
    bool active = true;  ///< 焦点态（决定取激活配色还是失焦配色）
    bool resizable = true;  ///< 可缩放性（false 时最大化钮为空盒）
    int margin_dp = 0;  ///< 本帧阴影边距厚度（逻辑 dp；塌缩态 0，由 `shadow_margin_dp` 求得）
    bool csd_title = false;  ///< 是否自绘标题栏（`CsdMode::title`）
    bool fullscreen_revealed = false;  ///< 全屏揭示条是否展开
    int hovered_button = AURORA_CSD_BUTTON_NONE;  ///< 当前悬停按钮序号
    int pressed_button = AURORA_CSD_BUTTON_NONE;  ///< 当前按压按钮序号（`-1` = 未按）
    std::string title;  ///< 标题文字（空串或 `show_title=false` 时不绘制）
    std::shared_ptr<Image> icon;  ///< 标题栏图标（nullptr = 留白，几何位保留）
    TitleBarStyle style{};  ///< 标题栏样式（栏高 / 配色 / 按钮语言 / 显隐开关）
    WindowFrameStyle frame{};  ///< 窗框样式（投影基色 / 模糊半径；厚度走 `margin_dp`）
};

/// @brief 装配本帧的 `TitleBarPaintState`（软件光栅与 GPU 录制两条上屏路径的唯一装配点）。
///
/// 内容区原点即阴影边距；GPU 路径的 overlay / underlay 回放据此平移，软件路径的光栅同此坐标域。
/// @param in 本帧宿主状态快照。
/// @return 可直接喂给 `paint_title_bar` / `paint_window_shadow` 的纯值绘制状态。
[[nodiscard]] inline auto make_paint_state(DecorationInput in) -> TitleBarPaintState {
    TitleBarPaintState s;
    s.width = in.size.width;
    s.height = in.size.height;
    const auto m = static_cast<float>(in.margin_dp);
    s.origin_x = m;  // 内容区原点相对整幅表面的偏移 = margin（塌缩态 0）
    s.origin_y = m;
    s.shadow_margin = m;
    s.shadow_blur = frame_blur_dp(in.frame, in.margin_dp);
    s.shadow_color = in.frame.shadow_color;
    s.mode = in.mode;
    s.fullscreen_bar_revealed = in.fullscreen_revealed;
    s.title_bar = in.csd_title;
    s.active = in.active;
    s.resizable = in.resizable;
    s.hovered_button = in.hovered_button;
    s.pressed_button = in.pressed_button;
    s.title = std::move(in.title);
    s.icon = std::move(in.icon);
    s.style = in.style;
    return s;
}

/// @brief 本帧的 CSD 安全区（表面原点 → 应用内容原点的总偏移）。
/// @param csd_title 是否自绘标题栏。
/// @param bar_height 标题栏高度（逻辑 dp）。
/// @param margin_dp 本帧阴影边距厚度（逻辑 dp）。
/// @param mode 当前窗口几何态。
/// @return 四向内边距（left/top/right/bottom 顺序）。
[[nodiscard]] inline auto content_inset(bool csd_title, float bar_height, int margin_dp, WindowMode mode)
    -> EdgeInsets {
    return content_inset_for(csd_title, bar_height, margin_dp, mode);
}

}  // namespace aurora::csd
