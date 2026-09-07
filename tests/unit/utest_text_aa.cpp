// 目标源单元：widget/text.h + src/aurora/widget/text.cpp
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// render/text_aa_mode.h(TextAAMode，经 AA 各段行使)、render/bitmap_font.h(BitmapFont 内置字体回退)、
// widget/text_span.h(TextSpan，经 sec_rich_text? 见 test_rich_text.cpp——TextSpan 归属 rich_text 单元)。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

// （自 utest_text.cpp 拆分：ClearType 彩色镶边 / AA 模式覆写段）

namespace aurora::test_cases::utest_text_aa {

namespace render = aurora::render;
using au::Alignment;
using au::BuildContext;
using au::Button;
using au::Clipboard;
using au::Color;
using au::Column;
using au::ColumnProps;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::Font;
using au::FontStyle;
using au::FontWeight;
using au::Json;
using au::KeyAction;
using au::KeyCode;
using au::KeyEvent;
using au::LocalizedString;
using au::Modifier;
using au::ModifierKey;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Row;
using au::RowProps;
using au::set_current_focus_manager;
using au::Size;
using au::Text;
using au::TextAlign;
using au::TextDecoration;
using au::TextOverflow;
using au::TextProps;
using au::Widget;
namespace sec_text_aa_cleartype_fringe {
namespace ar = aurora::render;

void run() {
    using au::Alignment;
    using au::BuildContext;
    using au::Color;
    using au::Column;
    using au::Constraints;
    using au::Modifier;
    using au::Node;
    using au::Painter;
    using au::Point;
    using au::Rect;
    using au::Size;
    using au::Text;

    constexpr int w = 360;
    constexpr int h = 80;
    constexpr auto winbg = Color{245, 245, 247};

    auto render = [&](bool supersample) -> Painter {
        const auto t = std::make_shared<Text>(LocalizedString{"curve@0.5 = 0.500000"});
        if (supersample) {
            t->text_aa_mode = ar::TextAAMode::Supersample;
        } else {
            // 显式设为 ClearType 以触发子像素 RGB 着色路径
            // （默认已是 Supersample，不显式切换则两边都是灰度 AA，无法测到彩色镶边）
            t->text_aa_mode = ar::TextAAMode::ClearType;
        }
        auto const col = std::make_shared<Column>(std::initializer_list{Node{t}});
        const BuildContext ctx;
        col->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0, .height = 0};
        c.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
        col->layout(c, ctx);
        Painter p;
        p.begin(w, h);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                         .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                    winbg);
        col->paint(p,
                   Rect{.origin = Point{.x = 0, .y = 0},
                        .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                   ctx);
        return p;
    };

    Painter p_ct = render(false);  // 默认 ClearType
    Painter p_ss = render(true);  // Supersample

    const std::uint8_t *buf_ct = p_ct.data();
    const std::uint8_t *buf_ss = p_ss.data();
    auto px = [&](const std::uint8_t *b, int x, int y) -> std::array<int, 3> {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return {b[i], b[i + 1], b[i + 2]};  // NOLINT
    };
    // 浅灰底上的彩色镶边判定：非背景、非纯黑、且三通道强失衡（R/G/B 差异大）。
    auto is_colored_fringe = [&](const std::array<int, 3> &c) -> bool {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool is_bg = std::abs(c[0] - 245) <= 12 && std::abs(c[1] - 245) <= 12 && std::abs(c[2] - 247) <= 12;
        if (is_bg) {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (c[0] < 60 && c[1] < 60 && c[2] < 60) {
            return false;  // 字形核心（黑）
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const int mx = std::max({c[0], c[1], c[2]});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const int mn = std::min({c[0], c[1], c[2]});
        return (mx - mn) > 40;  // 强通道失衡 = ClearType 红/蓝子像素镶边
    };

    int ct_fringe = 0;
    int ss_fringe = 0;
    int total = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto a = px(buf_ct, x, y);
            const auto b = px(buf_ss, x, y);
            ++total;
            if (is_colored_fringe(a)) {
                ++ct_fringe;
            }
            if (is_colored_fringe(b)) {
                ++ss_fringe;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType colored-fringe pixels = ", ct_fringe, "/", total, " (",
                    100.0 * ct_fringe / total, "%)");
    AURORA_LOG_INFO("test", "Supersample colored-fringe pixels = ", ss_fringe, "/", total, " (",
                    100.0 * ss_fringe / total, "%)");

    // 逐帧闪烁验证：ClearType 渲染两帧（每帧都先清成 245,245,247），应完全一致。
    Painter p_c_t2 = render(false);
    const std::uint8_t *buf_c_t2 = p_c_t2.data();
    int diff = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto a = px(buf_ct, x, y);
            const auto b = px(buf_c_t2, x, y);
            if (a != b) {
                ++diff;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType two-frame (cleared per frame) diff pixels = ", diff, "/", total,
                    " (=0 means no inter-frame flicker)");

    AURORA_TEST_CHECK(ct_fringe > ss_fringe && diff == 0);
}
}  // namespace sec_text_aa_cleartype_fringe

namespace sec_text_aa_override {
namespace ar = aurora::render;

void run() {
    using au::Alignment;
    using au::BuildContext;
    using au::Color;
    using au::Constraints;
    using au::Font;
    using au::Modifier;
    using au::Node;
    using au::Painter;
    using au::Point;
    using au::Rect;
    using au::Size;
    using au::Stack;
    using au::Text;

    constexpr int w = 240;
    constexpr int h = 240;
    constexpr float k_stage = 120.0F;
    constexpr float k_base_box = 80.0F;
    constexpr auto breathe = Color{236, 72, 153};  // 粉相（较亮），最易暴露白边
    constexpr auto winbg = Color{245, 245, 247};

    const auto box = std::make_shared<Text>(LocalizedString{"color pulse"});
    box->text_color = Color{255, 255, 255};  // 呼吸盒上白字
    box->text_aa_mode = ar::TextAAMode::Supersample;  // 修复：彩色背景走 Supersample，避免 ClearType 白边
    box->modifier.set(Modifier{}.size(k_stage, k_stage).background(breathe).align(Alignment::Center));

    // 两档缩放，分别看模糊
    auto mk_scale = [&](float s) -> std::shared_ptr<Text> {
        auto t = std::make_shared<Text>(LocalizedString{"scale"});
        t->modifier.set(Modifier{}.size(k_base_box, k_base_box).align(Alignment::Center).scale(s));
        return t;
    };
    const auto scale_inner = mk_scale(1.4F);

    AURORA_LOG_INFO("test", "box->text_aa_mode has_value=", box->text_aa_mode.has_value());

    auto const stage = std::make_shared<Stack>(std::vector{Node{box}, Node{scale_inner}}, Alignment::Center);
    stage->modifier.set(Modifier{}.size(k_stage, k_stage).clip());

    const BuildContext ctx;
    stage->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    stage->layout(c, ctx);

    Painter p;
    p.begin(w, h);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                winbg);
    stage->paint(p,
                 Rect{.origin = Point{.x = (w - k_stage) / 2.0F, .y = (h - k_stage) / 2.0F},
                      .size = Size{.width = k_stage, .height = k_stage}},
                 ctx);

    const std::uint8_t *buf = p.data();
    auto px = [&](int x, int y) -> std::array<int, 3> {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return {buf[i], buf[i + 1], buf[i + 2]};  // NOLINT
    };
    auto classify = [&](int x, int y) -> char {
        const auto pc = px(x, y);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (pc[0] > 200 && pc[1] > 200 && pc[2] > 200) {
            return 'W';  // 纯白字
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (std::abs(pc[0] - breathe.r) <= 10 && std::abs(pc[1] - breathe.g) <= 10 &&
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            std::abs(pc[2] - breathe.b) <= 10) {
            return '.';  // 呼吸色
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (pc[0] < 60 && pc[1] < 60 && pc[2] < 60) {
            return '#';  // 黑字
        }
        // ClearType 真·彩色尖刺：某一通道≈255 而另两通道仍贴近底色低值（红/蓝镶边）。
        // 注意呼吸底色本身 mx-mn 就很大（236-72=164），故不能用「整体方差」判定。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool brighter = (pc[0] > breathe.r + 12 || pc[1] > breathe.g + 12 || pc[2] > breathe.b + 12);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_g = std::abs(pc[1] - breathe.g) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_b = std::abs(pc[2] - breathe.b) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_r = std::abs(pc[0] - breathe.r) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool red_spike = (pc[0] > 240 && near_g && near_b);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool blue_spike = (pc[2] > 240 && near_r && near_g);
        if (brighter && (red_spike || blue_spike)) {
            return 'S';  // ClearType 子像素红/蓝镶边
        }
        if (brighter) {
            return 'L';  // 亮于背景但柔和（正常 AA 边）
        }
        return '?';  // 其它（深色 AA 边）
    };

    AURORA_LOG_INFO("test", "=== stage region (120x120) downsampled to 60x60 ===");
    constexpr int x0 = static_cast<int>((w - k_stage) / 2);
    constexpr int y0 = static_cast<int>((h - k_stage) / 2);
    for (int y = y0; y < y0 + static_cast<int>(k_stage); y += 2) {
        for (int x = x0; x < x0 + static_cast<int>(k_stage); x += 2) {
            AURORA_LOG_INFO("test", classify(x, y));
        }
        AURORA_LOG_INFO("test");
    }

    // 统计 'S'（ClearType 彩色尖刺白边）与 'L'（柔和亮边）
    int spike = 0;
    int light = 0;
    int other = 0;
    int total = 0;
    for (int y = y0; y < y0 + static_cast<int>(k_stage); ++y) {
        for (int x = x0; x < x0 + static_cast<int>(k_stage); ++x) {
            ++total;
            const char k = classify(x, y);
            if (k == 'S') {
                ++spike;
            }
            if (k == 'L') {
                ++light;
            }
            if (k == '?') {
                ++other;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType colored-spike white edge (S) = ", spike, "/", total, " (", 100.0 * spike / total,
                    "%)");
    AURORA_LOG_INFO("test", "soft bright edge (L) = ", light, "  dark AA edge (?) = ", other);
    AURORA_LOG_INFO(
        "test", "conclusion: S near 0 means white text on colored bg has no ClearType fringe (Supersample effective)");
    // 断言：白字在彩色/动画背景上显式 Supersample 后，不得出现 ClearType 子像素红/蓝镶边。
    // （缩放文字走离屏双线性合成，路径也需稳定无崩溃。）
    AURORA_TEST_CHECK(spike == 0);
}
}  // namespace sec_text_aa_override

AURORA_TEST() {
    sec_text_aa_cleartype_fringe::run();
    sec_text_aa_override::run();
}

}  // namespace aurora::test_cases::utest_text_aa
