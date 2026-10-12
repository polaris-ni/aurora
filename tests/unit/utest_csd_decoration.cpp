/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/csd_decoration.h
/// 测试说明: 锁定 CSD 装饰「策略 / 交互 / 状态装配」共享层的语义真值：
///   ① `DecorationPolicy` × 有无 SSD 十格策略表 → `{title, border, mod_move}` 的解析结果；
///   ② 每格再 × {Normal, Maximized, FullScreen, 平铺} 的 `content_inset` 四分量（含阴影边距塌缩）；
///   ③ 按钮命中：绘制矩形即命中矩形、隐藏空盒恒不命中、重叠时以关闭钮优先；
///   ④ 悬停解析的栏带判据与「非自绘标题栏恒无悬停」；
///   ⑤ 全屏揭示条状态机（近顶展开 / 下离收回 / 退出全屏必复位）；
///   ⑥ 连击判定的时间窗与半径边界（含欧氏距离而非切比雪夫距离的证据）；
///   ⑦ `make_paint_state` 的逐字段装配。
/// 全程进程内纯值运算，不连任何合成器 / 窗口系统，无头 CI 亦可跑。

#include <cstdint>
#include <memory>
#include <string>

#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/window/detail/csd_decoration.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/detail/title_bar_painter.h"
#include "aurora/window/surface.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_csd_decoration {

namespace {
namespace csd = aurora::csd;

/// @brief 快照基准栏高：下表所有 `content_inset` 期望值均按它手写推算，故改动此处必须先改期望。
auto sample_style() -> TitleBarStyle {
    TitleBarStyle s;
    s.height = 36.0F;
    return s;
}

struct Triple {
    bool title;
    bool border;
    bool mod_move;
};

struct Pad {
    float left;
    float top;
    float right;
    float bottom;
};

struct SnapshotRow {
    DecorationPolicy policy;
    bool ssd;
    Triple triple;
    Pad normal;
    Pad maximized;
    Pad fullscreen;
    Pad tiled;
};

// 十格策略表 + 四分量期望（手写推算，见 sample_style 的栏高契约）。
// **动 `resolve_mode` 前先看这张表**：它记录的是「抽层前 WaylandSurface 构造期 switch」的逐格结果，
// 任何一格变化都意味着「同样的意图会得到不同的装饰形态」，须同步 CHG-004 验收判据与 codespec/ 回写。
constexpr SnapshotRow AURORA_POLICY_SNAPSHOT[] = {
    // Auto / ServerSide + 有 SSD：真·服务端装饰，三项全 false，任何态都不留边距。
    {.policy = DecorationPolicy::Auto,
     .ssd = true,
     .triple = {.title = false, .border = false, .mod_move = false},
     .normal = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
    {.policy = DecorationPolicy::ServerSide,
     .ssd = true,
     .triple = {.title = false, .border = false, .mod_move = false},
     .normal = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
    // Auto / ServerSide + 无 SSD：CSD 兜底（否则窗口不可移动、不可关闭），Normal 态带 10dp 边距。
    {.policy = DecorationPolicy::Auto,
     .ssd = false,
     .triple = {.title = true, .border = true, .mod_move = false},
     .normal = {.left = 10.0F, .top = 46.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F}},
    {.policy = DecorationPolicy::ServerSide,
     .ssd = false,
     .triple = {.title = true, .border = true, .mod_move = false},
     .normal = {.left = 10.0F, .top = 46.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F}},
    // ClientSide：即便有 SSD 也自绘标题栏；边框分量交由阴影边距承担，故 border=false 而边距照旧。
    {.policy = DecorationPolicy::ClientSide,
     .ssd = true,
     .triple = {.title = true, .border = false, .mod_move = false},
     .normal = {.left = 10.0F, .top = 46.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F}},
    {.policy = DecorationPolicy::ClientSide,
     .ssd = false,
     .triple = {.title = true, .border = false, .mod_move = false},
     .normal = {.left = 10.0F, .top = 46.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F}},
    // Borderless：无标题栏故顶部分量只有边距；边距本身承担八向缩放热区。
    {.policy = DecorationPolicy::Borderless,
     .ssd = true,
     .triple = {.title = false, .border = true, .mod_move = true},
     .normal = {.left = 10.0F, .top = 10.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
    {.policy = DecorationPolicy::Borderless,
     .ssd = false,
     .triple = {.title = false, .border = true, .mod_move = true},
     .normal = {.left = 10.0F, .top = 10.0F, .right = 10.0F, .bottom = 10.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
    // Frameless：完全无装饰，连边距也没有；移动交给修饰键拖拽（mod_move）。
    {.policy = DecorationPolicy::Frameless,
     .ssd = true,
     .triple = {.title = false, .border = false, .mod_move = true},
     .normal = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
    {.policy = DecorationPolicy::Frameless,
     .ssd = false,
     .triple = {.title = false, .border = false, .mod_move = true},
     .normal = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .maximized = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .fullscreen = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F},
     .tiled = {.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}},
};

auto row_tag(const SnapshotRow &row) -> std::string {
    std::string tag = "policy=";
    tag += std::to_string(static_cast<int>(row.policy));
    tag += " ssd=";
    tag += row.ssd ? "true" : "false";
    return tag;
}

auto check_pad(const std::string &tag, const EdgeInsets &got, const Pad &want) -> void {
    AURORA_TEST_CHECK_MSG(got.left == want.left,
                          tag + ": left want=" + std::to_string(want.left) + " got=" + std::to_string(got.left));
    AURORA_TEST_CHECK_MSG(got.top == want.top,
                          tag + ": top want=" + std::to_string(want.top) + " got=" + std::to_string(got.top));
    AURORA_TEST_CHECK_MSG(got.right == want.right,
                          tag + ": right want=" + std::to_string(want.right) + " got=" + std::to_string(got.right));
    AURORA_TEST_CHECK_MSG(got.bottom == want.bottom,
                          tag + ": bottom want=" + std::to_string(want.bottom) + " got=" + std::to_string(got.bottom));
}

auto box_center(const Rect &r) -> Point {
    return Point{.x = r.origin.x + (r.size.width / 2.0F), .y = r.origin.y + (r.size.height / 2.0F)};
}

}  // namespace

AURORA_TEST_CASE(policy_resolution_snapshot) {
    for (const SnapshotRow &row : AURORA_POLICY_SNAPSHOT) {
        const csd::CsdMode m = csd::resolve_mode(row.policy, row.ssd);
        const std::string tag = row_tag(row);
        AURORA_TEST_CHECK_MSG(m.title == row.triple.title, tag + ": title");
        AURORA_TEST_CHECK_MSG(m.border == row.triple.border, tag + ": border");
        AURORA_TEST_CHECK_MSG(m.mod_move == row.triple.mod_move, tag + ": mod_move");
    }
}

AURORA_TEST_CASE(content_inset_snapshot_across_geometry_states) {
    const TitleBarStyle style = sample_style();
    AURORA_TEST_CHECK_EQ(style.height, 36.0F);  // 期望值的推导基准，变了必须同步下表

    for (const SnapshotRow &row : AURORA_POLICY_SNAPSHOT) {
        const csd::CsdMode m = csd::resolve_mode(row.policy, row.ssd);
        const std::string tag = row_tag(row);

        // Normal（未平铺）：唯一的非塌缩态，边距 = AURORA_SHADOW_MARGIN_DP。
        const int m_normal = csd::shadow_margin_dp(m.client_decorated(), WindowMode::Normal, false);
        check_pad(tag + " normal", csd::content_inset(m.title, style.height, m_normal, WindowMode::Normal), row.normal);

        const int m_maximized = csd::shadow_margin_dp(m.client_decorated(), WindowMode::Maximized, false);
        check_pad(tag + " maximized", csd::content_inset(m.title, style.height, m_maximized, WindowMode::Maximized),
                  row.maximized);

        const int m_full = csd::shadow_margin_dp(m.client_decorated(), WindowMode::FullScreen, false);
        check_pad(tag + " fullscreen", csd::content_inset(m.title, style.height, m_full, WindowMode::FullScreen),
                  row.fullscreen);

        // 平铺：几何态仍是 Normal，仅边距塌缩（title 分量不塌）。
        const int m_tiled = csd::shadow_margin_dp(m.client_decorated(), WindowMode::Normal, true);
        check_pad(tag + " tiled", csd::content_inset(m.title, style.height, m_tiled, WindowMode::Normal), row.tiled);
    }
}

AURORA_TEST_CASE(client_decorated_and_takes_pointer_matrix) {
    for (const SnapshotRow &row : AURORA_POLICY_SNAPSHOT) {
        const csd::CsdMode m = csd::resolve_mode(row.policy, row.ssd);
        const std::string tag = row_tag(row);
        AURORA_TEST_CHECK_MSG(m.client_decorated() == (row.triple.title || row.triple.border), tag + ": decorated");
        AURORA_TEST_CHECK_MSG(m.takes_pointer() == (row.triple.title || row.triple.border || row.triple.mod_move),
                              tag + ": takes_pointer");
    }
    // Frameless 是无装饰但仍接管指针的唯一组合（靠修饰键拖拽移动），别把它判成「不需要接管」。
    const csd::CsdMode frameless = csd::resolve_mode(DecorationPolicy::Frameless, true);
    AURORA_TEST_CHECK_FALSE(frameless.client_decorated());
    AURORA_TEST_CHECK_TRUE(frameless.takes_pointer());
}

AURORA_TEST_CASE(normalize_policy_folds_frameless_flag) {
    // frameless 旧字段优先级最高：任何策略都被折叠成 Frameless。
    AURORA_TEST_CHECK(csd::normalize_policy(true, DecorationPolicy::Auto) == DecorationPolicy::Frameless);
    AURORA_TEST_CHECK(csd::normalize_policy(true, DecorationPolicy::ServerSide) == DecorationPolicy::Frameless);
    AURORA_TEST_CHECK(csd::normalize_policy(true, DecorationPolicy::ClientSide) == DecorationPolicy::Frameless);
    AURORA_TEST_CHECK(csd::normalize_policy(true, DecorationPolicy::Borderless) == DecorationPolicy::Frameless);
    AURORA_TEST_CHECK(csd::normalize_policy(true, DecorationPolicy::Frameless) == DecorationPolicy::Frameless);
    // frameless=false 时原样透传。
    AURORA_TEST_CHECK(csd::normalize_policy(false, DecorationPolicy::Auto) == DecorationPolicy::Auto);
    AURORA_TEST_CHECK(csd::normalize_policy(false, DecorationPolicy::ServerSide) == DecorationPolicy::ServerSide);
    AURORA_TEST_CHECK(csd::normalize_policy(false, DecorationPolicy::ClientSide) == DecorationPolicy::ClientSide);
    AURORA_TEST_CHECK(csd::normalize_policy(false, DecorationPolicy::Borderless) == DecorationPolicy::Borderless);
    AURORA_TEST_CHECK(csd::normalize_policy(false, DecorationPolicy::Frameless) == DecorationPolicy::Frameless);
}

AURORA_TEST_CASE(hit_button_tracks_geometry_boxes) {
    const TitleBarStyle style = sample_style();
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);

    // 三钮齐备时，各盒中心分别命中自己的序号。
    AURORA_TEST_CHECK_GT(g.close.size.width, 0.0F);
    AURORA_TEST_CHECK_GT(g.maximize.size.width, 0.0F);
    AURORA_TEST_CHECK_GT(g.minimize.size.width, 0.0F);

    const Point c_close = box_center(g.close);
    AURORA_TEST_CHECK_EQ(csd::hit_button(g, c_close.x, c_close.y), csd::AURORA_CSD_BUTTON_CLOSE);
    const Point c_max = box_center(g.maximize);
    AURORA_TEST_CHECK_EQ(csd::hit_button(g, c_max.x, c_max.y), csd::AURORA_CSD_BUTTON_MAXIMIZE);
    const Point c_min = box_center(g.minimize);
    AURORA_TEST_CHECK_EQ(csd::hit_button(g, c_min.x, c_min.y), csd::AURORA_CSD_BUTTON_MINIMIZE);

    // 标题文字区不应被误判成按钮（否则整条标题栏都能触发关闭）。
    const Point c_title = box_center(g.title);
    AURORA_TEST_CHECK_EQ(csd::hit_button(g, c_title.x, c_title.y), csd::AURORA_CSD_BUTTON_NONE);
}

AURORA_TEST_CASE(hidden_button_box_never_hits) {
    // 纯值性质：宽度为 0 的盒恒不命中——即便它与人造可见盒同点位，也不拦截别人的命中。
    const TitleBarStyle style = sample_style();
    const TitleBarGeometry shown = title_bar_geometry(800.0F, style, false, true);
    const Point c_close = box_center(shown.close);

    TitleBarGeometry blinded = shown;
    blinded.close = Rect{.origin = shown.close.origin, .size = Size{}};  // 同点位、零宽度
    AURORA_TEST_CHECK_EQ(blinded.close.size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(csd::hit_button(blinded, c_close.x, c_close.y), csd::AURORA_CSD_BUTTON_NONE);

    // 把三个盒全部清零：整条标题栏再无热区。
    TitleBarGeometry none_visible;
    AURORA_TEST_CHECK_EQ(csd::hit_button(none_visible, c_close.x, c_close.y), csd::AURORA_CSD_BUTTON_NONE);
}

AURORA_TEST_CASE(hidden_button_reflows_and_vacated_slot_reassigned) {
    // 既有契约（几何层「尺寸规则」注释块）：隐藏按钮不占位，其余按钮向贴边侧收缩补位。
    // 于是原按钮位不会闲置，而是被补位上来的邻居接管——绘制与命中共用同一份几何，故两者同步换行。
    const TitleBarStyle style = sample_style();
    const TitleBarGeometry shown = title_bar_geometry(800.0F, style, false, true);
    const Point c_close = box_center(shown.close);
    const Point c_max = box_center(shown.maximize);

    // resizable=false：close→min 两枚（max 让位后 min 平移到原 max 位）。
    const TitleBarGeometry fixed = title_bar_geometry(800.0F, style, false, false);
    AURORA_TEST_CHECK_EQ(fixed.maximize.size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(fixed.minimize.origin.x, shown.maximize.origin.x);
    AURORA_TEST_CHECK_EQ(csd::hit_button(fixed, c_max.x, c_max.y), csd::AURORA_CSD_BUTTON_MINIMIZE);
    AURORA_TEST_CHECK_EQ(csd::hit_button(fixed, c_close.x, c_close.y), csd::AURORA_CSD_BUTTON_CLOSE);

    // show_close=false：max→min 两枚整体右移，原 close 位被 max 接管。
    TitleBarStyle no_close = style;
    no_close.show_close = false;
    const TitleBarGeometry g = title_bar_geometry(800.0F, no_close, false, true);
    AURORA_TEST_CHECK_EQ(g.close.size.width, 0.0F);
    AURORA_TEST_CHECK_EQ(g.maximize.origin.x, shown.close.origin.x);
    AURORA_TEST_CHECK_EQ(csd::hit_button(g, c_close.x, c_close.y), csd::AURORA_CSD_BUTTON_MAXIMIZE);
}

AURORA_TEST_CASE(hit_button_priority_order) {
    // 人为造三盒重叠，锁定关闭 > 最大化 > 最小化的判定顺序。
    constexpr Rect overlap{.origin = Point{.x = 100.0F, .y = 4.0F}, .size = Size{.width = 20.0F, .height = 20.0F}};
    constexpr Point probe{.x = 110.0F, .y = 14.0F};

    TitleBarGeometry all;
    all.close = overlap;
    all.maximize = overlap;
    all.minimize = overlap;
    AURORA_TEST_CHECK_EQ(csd::hit_button(all, probe.x, probe.y), csd::AURORA_CSD_BUTTON_CLOSE);

    TitleBarGeometry without_close = all;
    without_close.close = Rect{};
    AURORA_TEST_CHECK_EQ(csd::hit_button(without_close, probe.x, probe.y), csd::AURORA_CSD_BUTTON_MAXIMIZE);

    TitleBarGeometry only_minimize = without_close;
    only_minimize.maximize = Rect{};
    AURORA_TEST_CHECK_EQ(csd::hit_button(only_minimize, probe.x, probe.y), csd::AURORA_CSD_BUTTON_MINIMIZE);

    only_minimize.minimize = Rect{};
    AURORA_TEST_CHECK_EQ(csd::hit_button(only_minimize, probe.x, probe.y), csd::AURORA_CSD_BUTTON_NONE);
}

AURORA_TEST_CASE(resolve_hover_band_and_policy_gating) {
    const TitleBarStyle style = sample_style();
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    const Point c_close = box_center(g.close);

    // 自绘标题栏：盒内 → 悬停；带外（含刚出带的 y=height）→ 无悬停。
    AURORA_TEST_CHECK_EQ(csd::resolve_hover(style, 800.0F, false, true, true, c_close.x, c_close.y),
                         csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(csd::resolve_hover(style, 800.0F, false, true, true, c_close.x, -0.5),
                         csd::AURORA_CSD_BUTTON_NONE);
    AURORA_TEST_CHECK_EQ(csd::resolve_hover(style, 800.0F, false, true, true, c_close.x, 36.0),
                         csd::AURORA_CSD_BUTTON_NONE);
    // 栏带内但不在任何按钮上（x 落在标题文字区）。
    AURORA_TEST_CHECK_EQ(csd::resolve_hover(style, 800.0F, false, true, true, 8.0, 18.0), csd::AURORA_CSD_BUTTON_NONE);
    // 非自绘标题栏：即便坐标正好压在按钮盒上也恒无悬停（本就无按钮可悬停）。
    AURORA_TEST_CHECK_EQ(csd::resolve_hover(style, 800.0F, false, true, false, c_close.x, c_close.y),
                         csd::AURORA_CSD_BUTTON_NONE);
}

AURORA_TEST_CASE(fullscreen_reveal_state_machine) {
    constexpr float bar = 36.0F;
    // 近顶 6dp 展开（收→展的跃迁点在 y<6）。
    AURORA_TEST_CHECK_TRUE(csd::update_fullscreen_reveal(false, WindowMode::FullScreen, 0.0, bar));
    AURORA_TEST_CHECK_TRUE(csd::update_fullscreen_reveal(false, WindowMode::FullScreen, 5.5, bar));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(false, WindowMode::FullScreen, 6.0, bar));
    // 已展开：栏高 + 4dp 缓冲区内保持展开，超出才收回。
    AURORA_TEST_CHECK_TRUE(csd::update_fullscreen_reveal(true, WindowMode::FullScreen, 0.0, bar));
    AURORA_TEST_CHECK_TRUE(csd::update_fullscreen_reveal(true, WindowMode::FullScreen, 40.0, bar));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(true, WindowMode::FullScreen, 40.5, bar));
    // 缓冲区写字面上跟着栏高走：栏高 28 时阈值是 32。
    AURORA_TEST_CHECK_TRUE(csd::update_fullscreen_reveal(true, WindowMode::FullScreen, 32.0, 28.0F));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(true, WindowMode::FullScreen, 32.5, 28.0F));
}

AURORA_TEST_CASE(reveal_resets_when_leaving_fullscreen) {
    // 退出全屏必须复位，否则揭示条会永久残留在普通窗口上。
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(true, WindowMode::Normal, 0.0, 36.0F));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(true, WindowMode::Maximized, 0.0, 36.0F));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(true, WindowMode::Minimized, 0.0, 36.0F));
    AURORA_TEST_CHECK_FALSE(csd::update_fullscreen_reveal(false, WindowMode::Normal, 0.0, 36.0F));
}

AURORA_TEST_CASE(double_click_window_boundary) {
    // 阈值符号直取 event 层常量：装饰层与 widget 树的 click_count 共用同一份真源。
    constexpr std::uint32_t window = AURORA_DEFAULT_CLICK_WINDOW_MS;
    csd::DecorationInteraction st;

    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(st, 1000U, 40.0, 10.0));  // 首击不成连击
    AURORA_TEST_CHECK_TRUE(csd::consume_double_click(st, 1000U + (window - 1U), 40.0, 10.0));
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(st, 1000U + (window - 1U) + window, 40.0, 10.0));
    AURORA_TEST_CHECK_TRUE(csd::consume_double_click(st, 1000U + (window - 1U) + window + (window - 1U), 40.0, 10.0));
}

AURORA_TEST_CASE(double_click_radius_boundary) {
    constexpr auto radius = static_cast<double>(AURORA_DEFAULT_CLICK_RADIUS_DP);
    csd::DecorationInteraction st;

    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(st, 0U, 100.0, 20.0));
    // 半径内：dx = radius - 0.5 → 成立。
    AURORA_TEST_CHECK_TRUE(csd::consume_double_click(st, 1U, 100.0 + (radius - 0.5), 20.0));
    // 恰等于半径：严格小于，故不成立（边界对称：与 dt == window 同款）。
    csd::DecorationInteraction edge;
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(edge, 0U, 100.0, 20.0));
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(edge, 1U, 100.0 + radius, 20.0));

    // 距离是欧氏距离而非切比雪夫：dx=3.0 / dy=2.7 → hypot≈4.036 > 4（若按 max(3.0,2.7) 会误判成立）。
    csd::DecorationInteraction diag;
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(diag, 0U, 100.0, 20.0));
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(diag, 1U, 103.0, 22.7));
    // 反证：同向 3-4-5 缩放一半（1.5/2.0 → 2.5）在窗内成立。
    csd::DecorationInteraction inside;
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(inside, 0U, 100.0, 20.0));
    AURORA_TEST_CHECK_TRUE(csd::consume_double_click(inside, 1U, 101.5, 22.0));
}

AURORA_TEST_CASE(double_click_always_records_state) {
    csd::DecorationInteraction st;
    // 无论判定结果如何，上次点击的时点 / 位置都要更新——这是「第二次才成立」的前提。
    AURORA_TEST_CHECK_FALSE(csd::consume_double_click(st, 500U, 12.0, 8.0));
    AURORA_TEST_CHECK_EQ(st.last_click_time, 500U);
    AURORA_TEST_CHECK_EQ(st.last_click_x, 12.0);
    AURORA_TEST_CHECK_EQ(st.last_click_y, 8.0);

    AURORA_TEST_CHECK_TRUE(csd::consume_double_click(st, 600U, 13.0, 8.0));
    AURORA_TEST_CHECK_EQ(st.last_click_time, 600U);
    AURORA_TEST_CHECK_EQ(st.last_click_x, 13.0);
    AURORA_TEST_CHECK_EQ(st.last_click_y, 8.0);
}

AURORA_TEST_CASE(make_paint_state_assembles_fields) {
    const TitleBarStyle style = sample_style();
    auto icon = std::make_shared<Image>();

    csd::DecorationInput in;
    in.size = Size{.width = 640.0F, .height = 480.0F};
    in.mode = WindowMode::Maximized;
    in.active = false;
    in.resizable = false;
    in.margin_dp = 10;
    in.csd_title = true;
    in.fullscreen_revealed = false;
    in.hovered_button = csd::AURORA_CSD_BUTTON_MAXIMIZE;
    in.title = "probe";
    in.icon = icon;
    in.style = style;

    const csd::TitleBarPaintState s = csd::make_paint_state(std::move(in));
    AURORA_TEST_CHECK_EQ(s.width, 640.0F);
    AURORA_TEST_CHECK_EQ(s.height, 480.0F);
    AURORA_TEST_CHECK_EQ(s.origin_x, 10.0F);  // 内容区原点 = 阴影边距
    AURORA_TEST_CHECK_EQ(s.origin_y, 10.0F);
    AURORA_TEST_CHECK_EQ(s.shadow_margin, 10.0F);
    AURORA_TEST_CHECK(s.mode == WindowMode::Maximized);
    AURORA_TEST_CHECK_FALSE(s.fullscreen_bar_revealed);
    AURORA_TEST_CHECK_TRUE(s.title_bar);
    AURORA_TEST_CHECK_FALSE(s.active);
    AURORA_TEST_CHECK_FALSE(s.resizable);
    AURORA_TEST_CHECK_EQ(s.hovered_button, csd::AURORA_CSD_BUTTON_MAXIMIZE);
    AURORA_TEST_CHECK_STREQ(s.title, "probe");
    AURORA_TEST_CHECK(s.icon.get() == icon.get());

    // 塌缩态：边距为 0 时原点回到整幅左上角。
    csd::DecorationInput collapsed;
    collapsed.size = Size{.width = 640.0F, .height = 480.0F};
    collapsed.margin_dp = 0;
    const csd::TitleBarPaintState flat = csd::make_paint_state(std::move(collapsed));
    AURORA_TEST_CHECK_EQ(flat.origin_x, 0.0F);
    AURORA_TEST_CHECK_EQ(flat.origin_y, 0.0F);
    AURORA_TEST_CHECK_EQ(flat.shadow_margin, 0.0F);
}

AURORA_TEST_CASE(shadow_resize_band_follows_resolved_decoration) {
    // 缩放热区是否存在由「解析后的装饰形态 × 几何态」共同决定：只有带阴影边距的组合才有八向热区。
    for (const SnapshotRow &row : AURORA_POLICY_SNAPSHOT) {
        constexpr double content_h = 600.0;
        constexpr double content_w = 800.0;
        const csd::CsdMode m = csd::resolve_mode(row.policy, row.ssd);
        const std::string tag = row_tag(row);
        const int normal_margin = csd::shadow_margin_dp(m.client_decorated(), WindowMode::Normal, false);

        const auto left = static_cast<int>(csd::classify_resize_zone(-5.0, 300.0, content_w, content_h, normal_margin));
        const auto top_left =
            static_cast<int>(csd::classify_resize_zone(-5.0, -5.0, content_w, content_h, normal_margin));
        if (normal_margin > 0) {
            AURORA_TEST_CHECK_MSG(left == static_cast<int>(csd::CsdResizeZone::Left), tag + ": left edge");
            AURORA_TEST_CHECK_MSG(top_left == static_cast<int>(csd::CsdResizeZone::TopLeft), tag + ": top-left corner");
        } else {
            AURORA_TEST_CHECK_MSG(left == static_cast<int>(csd::CsdResizeZone::None), tag + ": no band when collapsed");
            AURORA_TEST_CHECK_MSG(top_left == static_cast<int>(csd::CsdResizeZone::None), tag + ": no corner either");
        }

        // 最大化无论如何都不留热区（否则会挡住最大化窗口的边缘像素）。
        const int maximized_margin = csd::shadow_margin_dp(m.client_decorated(), WindowMode::Maximized, false);
        const auto maximized_zone =
            static_cast<int>(csd::classify_resize_zone(-5.0, 300.0, content_w, content_h, maximized_margin));
        AURORA_TEST_CHECK_MSG(maximized_zone == static_cast<int>(csd::CsdResizeZone::None), tag + ": maximized");
    }
}

AURORA_TEST_CASE(press_then_release_on_same_button_activates) {
    // 释放触发语义（对齐 GTK/Win32）：按下只置按压态，动作在「按下与释放同钮」时才成立。
    csd::DecorationInteraction st;
    csd::begin_press(st, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(st.pressed_button, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_TRUE(st.grab);  // 按下期间吞噬指针，不把按钮上的拖拽转给应用

    const int act = csd::consume_press(st, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(act, csd::AURORA_CSD_BUTTON_CLOSE);
}

AURORA_TEST_CASE(press_then_drag_away_cancels) {
    // 按错可撤销：按下关闭钮后拖到别处松手**不得**关窗（这是释放触发自带的纠错能力）。
    csd::DecorationInteraction st;
    csd::begin_press(st, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(csd::consume_press(st, csd::AURORA_CSD_BUTTON_NONE), csd::AURORA_CSD_BUTTON_NONE);

    // 拖到另一枚按钮上松手同样不成立（否则会把「拖开」误判成「改点最大化」）。
    csd::begin_press(st, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(csd::consume_press(st, csd::AURORA_CSD_BUTTON_MAXIMIZE), csd::AURORA_CSD_BUTTON_NONE);
}

AURORA_TEST_CASE(consume_press_always_clears_state) {
    // 无论结算结果如何，按压态与抓取标志都必须复位——否则按钮会留下「卡住的高亮」，
    // 且 grab 残留会让后续所有指针事件被静默吞掉。
    csd::DecorationInteraction st;
    for (const int ordinal : {csd::AURORA_CSD_BUTTON_MINIMIZE, csd::AURORA_CSD_BUTTON_MAXIMIZE,
                              csd::AURORA_CSD_BUTTON_CLOSE, csd::AURORA_CSD_BUTTON_NONE}) {
        csd::begin_press(st, ordinal);
        (void)csd::consume_press(st, ordinal);
        AURORA_TEST_CHECK_EQ(st.pressed_button, csd::AURORA_CSD_BUTTON_NONE);
        AURORA_TEST_CHECK_FALSE(st.grab);
    }
}

AURORA_TEST_CASE(pressed_state_reaches_paint_state) {
    // 按压序号须一路传到绘制层：漏掉这一环则 `pressed_tint` / `close_pressed` 永远画不出来。
    csd::DecorationInput in;
    in.size = Size{.width = 640.0F, .height = 480.0F};
    in.csd_title = true;
    in.hovered_button = csd::AURORA_CSD_BUTTON_CLOSE;
    in.pressed_button = csd::AURORA_CSD_BUTTON_CLOSE;
    const csd::TitleBarPaintState s = csd::make_paint_state(std::move(in));
    AURORA_TEST_CHECK_EQ(s.hovered_button, csd::AURORA_CSD_BUTTON_CLOSE);
    AURORA_TEST_CHECK_EQ(s.pressed_button, csd::AURORA_CSD_BUTTON_CLOSE);
}

AURORA_TEST_CASE(default_metrics_reproduce_legacy_geometry) {
    // 度量覆写引入后 `TitleBarStyle{}`（= 默认 Adwaita）的几何基线必须显式钉住，否则改动
    // 推导式会一次性改坏三套既有预设的观感。期望值全部手写推算（Adwaita，width=800，栏高 36）：
    //   按钮边长 clamp(36-10, 22, 30)=26，间距 6，右缘外边距 6；图标 min(16, 36-20)=16 @ x=12。
    // ⚠️ 间距 / 外边距原为 4 / 8（close 766 / min 706 / 标题 662），CHG-009 按 libadwaita
    //   `.toolbar` 的「6px margins and spacing」改为 6 / 6，本用例基线随之更新。
    const TitleBarStyle style = sample_style();
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);

    AURORA_TEST_CHECK_EQ(g.close.origin.x, 768.0F);
    AURORA_TEST_CHECK_EQ(g.close.origin.y, 5.0F);
    AURORA_TEST_CHECK_EQ(g.close.size.width, 26.0F);
    AURORA_TEST_CHECK_EQ(g.maximize.origin.x, 736.0F);
    AURORA_TEST_CHECK_EQ(g.minimize.origin.x, 704.0F);

    AURORA_TEST_CHECK_EQ(g.icon.origin.x, 12.0F);
    AURORA_TEST_CHECK_EQ(g.icon.size.width, 16.0F);

    // 标题区：Adwaita 档 `center_title` 默认 true ⇒ 整宽带（居中由绘制层按实测量宽完成）。
    AURORA_TEST_CHECK_EQ(g.title.origin.x, 0.0F);
    AURORA_TEST_CHECK_EQ(g.title.size.width, 800.0F);
}

AURORA_TEST_CASE(explicit_metrics_override_derived_rules) {
    // 非 0 才是覆写：按钮 40 / 间距 10 / 外边距 20 / 图标 24 / 内容内边距 30。
    // `center_title` 现默认为 true（居中档给整宽带），本用例测左对齐口径的标题区，故显式置回。
    TitleBarStyle style = sample_style();
    style.center_title = false;
    style.metrics.button_size = 40.0F;
    style.metrics.button_gap = 10.0F;
    style.metrics.button_margin = 20.0F;
    style.metrics.icon_size = 24.0F;
    style.metrics.content_padding = 30.0F;
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);

    AURORA_TEST_CHECK_EQ(g.close.origin.x, 740.0F);  // 800 − 20 − 40
    AURORA_TEST_CHECK_EQ(g.close.size.width, 40.0F);
    AURORA_TEST_CHECK_EQ(g.maximize.origin.x, 690.0F);  // 740 − 10 − 40
    AURORA_TEST_CHECK_EQ(g.minimize.origin.x, 640.0F);  // 690 − 10 − 40

    // 覆写优先于「min(16, 栏高−20)」：显式 24 即 24，不再被栏高夹小。
    AURORA_TEST_CHECK_EQ(g.icon.origin.x, 30.0F);
    AURORA_TEST_CHECK_EQ(g.icon.size.width, 24.0F);

    AURORA_TEST_CHECK_EQ(g.title.origin.x, 62.0F);  // 30 + 24 + 8
    AURORA_TEST_CHECK_EQ(g.title.size.width, 570.0F);  // (640 − 8) − 62
}

}  // namespace aurora::test_cases::utest_csd_decoration
