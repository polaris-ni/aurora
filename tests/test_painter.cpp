// 目标源单元：render/painter.h + src/aurora/render/painter.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_painter_aa.cpp
//   - test_painter_fill_fast.cpp
//   - test_painter_primitives.cpp
//   - test_painter_shift_pixels.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/color.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"

#include "test_harness.h"

using aurora::Color;
using aurora::DisplayList;
using aurora::Image;
using aurora::Matrix2D;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;

namespace aurora::tests::sec_painter_aa {

// 读取像素亮度（黑/白场景下 r==g==b）。
static auto lum(const Painter &p, int x, int y) -> int {
    const uint8_t *d = p.data();
    const int i = ((y * p.width()) + x) * 4;
    return d[i];
}

static void run() {
    constexpr int w = 100;
    constexpr int h = 100;
    Painter p;
    p.begin(w, h);

    // 背景白
    p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                Color::white());

    // 圆角裁剪（默认抗锯齿）+ 黑填充
    p.push_clip_rounded(
        Rect{ .origin = Point{ .x = 10.0f, .y = 10.0f }, .size = Size{ .width = 80.0f, .height = 80.0f } }, 20.0f);
    p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                Color::black());
    p.pop_clip();

    // 内部：纯黑
    AURORA_TEST_CHECK(lum(p, 50, 50) < 40);
    // 外部：纯白
    AURORA_TEST_CHECK(lum(p, 2, 2) > 210);

    // 左上圆角环：应存在抗锯齿灰阶像素（0<亮度<255）
    bool found_aa = false;
    for (int y = 11; y <= 29; ++y) {
        for (int x = 11; x <= 29; ++x) {
            const int g = lum(p, x, y);
            if (g > 20 && g < 235) {
                found_aa = true;
            }
        }
    }
    AURORA_TEST_CHECK(found_aa);

    // 硬遮罩模式：圆角边界不应有灰阶像素（仅纯黑/纯白）
    Painter p2;
    p2.begin(w, h);
    p2.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                       .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                 Color::white());
    p2.push_clip_rounded(
        Rect{ .origin = Point{ .x = 10.0f, .y = 10.0f }, .size = Size{ .width = 80.0f, .height = 80.0f } }, 20.0f,
        false);
    p2.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                       .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                 Color::black());
    p2.pop_clip();
    bool found_aa_hard = false;
    for (int y = 11; y <= 29; ++y) {
        for (int x = 11; x <= 29; ++x) {
            const int g = lum(p2, x, y);
            if (g > 20 && g < 235) {
                found_aa_hard = true;
            }
        }
    }
    AURORA_TEST_CHECK(!found_aa_hard);

    // get_pixel 只读语义：界内返回写入值，越界返回 Color{}。
    {
        Painter p3;
        p3.begin(8, 8);
        p3.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 8.0f, .height = 8.0f } },
                     Color{ 10, 20, 30, 255 });
        const Color oob = p3.get_pixel(-1, -1);
        AURORA_TEST_CHECK(oob.m_r == 0 && oob.m_g == 0 && oob.m_b == 0 && oob.m_a == 0);
        const Color c = p3.get_pixel(3, 3);
        AURORA_TEST_CHECK(c.m_r == 10 && c.m_g == 20 && c.m_b == 30 && c.m_a == 255);
    }

    // 回归：圆角裁剪（慢路径，m_has_rounded_clip=true 且无整窗边界保护）下，调用方传入
    // 越界坐标（负 y / 超界）时不得访问越界内存（0xC0000005）。LazyList 的 cache 区子项在
    // 列表贴近屏幕边缘时会绘制到视口外负坐标，若外层有圆角裁剪容器（卡片/SegmentedControl
    // 等）包裹，fill_rect/blend 走慢路径；此处以负坐标 fill_rect + 半透明 blend 覆盖验证。
    {
        Painter p4;
        p4.begin(w, h);
        p4.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                           .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                     Color::white());
        p4.push_clip_rounded(
            Rect{ .origin = Point{ .x = 10.0f, .y = 10.0f }, .size = Size{ .width = 80.0f, .height = 80.0f } }, 20.0f);
        // 正常填充（会被圆角裁剪到 clip 内黑、clip 外白）——建立期望基准
        p4.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                           .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
                     Color::black());
        // 负坐标与超界坐标（慢路径此前无边界检查，会 (负y*width+x)*4u 下溢越界）
        p4.fill_rect(
            Rect{ .origin = Point{ .x = -40.0f, .y = -40.0f }, .size = Size{ .width = 60.0f, .height = 60.0f } },
            Color::black());
        p4.fill_rect(
            Rect{ .origin = Point{ .x = 1000.0f, .y = 1000.0f }, .size = Size{ .width = 60.0f, .height = 60.0f } },
            Color::black());
        // 半透明 blend 同样走慢路径
        p4.fill_rect(
            Rect{ .origin = Point{ .x = -10.0f, .y = 50.0f }, .size = Size{ .width = 20.0f, .height = 20.0f } },
            Color{ 0, 0, 0, 128 });
        p4.pop_clip();
        // 越界填充不应影响裁剪内已有正确像素（中心仍应为白底被圆角裁剪后的结果）
        AURORA_TEST_CHECK(lum(p4, 50, 50) < 40); // 裁剪内被黑填充
        AURORA_TEST_CHECK(lum(p4, 2, 2) > 210);  // 裁剪外保持白
        // 到达此处即说明慢路径未越界崩溃
    }

    AURORA_LOG_INFO("test", "painter_aa_test: ALL PASS");
}
} // namespace aurora::tests::sec_painter_aa

namespace aurora::tests::sec_painter_fill_fast {

// 与 Painter::set_pixel 相同的 source-over 浮点公式（位级参考）。
static auto blend_ref(Color dst, Color src) -> Color {
    const float a = static_cast<float>(src.m_a) / 255.0f;
    const float inv = 1.0f - a;
    const auto blend = [a, inv](std::uint8_t d, std::uint8_t s) -> std::uint8_t {
        return static_cast<std::uint8_t>((static_cast<float>(d) * inv) + (static_cast<float>(s) * a));
    };
    return Color{ blend(dst.m_r, src.m_r), blend(dst.m_g, src.m_g), blend(dst.m_b, src.m_b), 255 };
}

static void run() {
    // 1) 不透明 + 半透明填充：与参考公式逐点一致（含非整数矩形边界）。
    {
        Painter p;
        p.begin(64, 48);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 48 } },
                    Color{ 10, 200, 30, 255 });
        const Color bg = p.get_pixel(5, 5);
        AURORA_TEST_CHECK(bg.m_r == 10 && bg.m_g == 200 && bg.m_b == 30);

        p.fill_rect(Rect{ .origin = Point{ .x = 8.4f, .y = 6.7f }, .size = Size{ .width = 20.3f, .height = 10.2f } },
                    Color{ 80, 120, 220, 110 });
        const Color exp = blend_ref(Color{ 10, 200, 30, 255 }, Color{ 80, 120, 220, 110 });
        // 覆盖区内部（远离边界取整）逐点等于参考混合值
        bool inner_ok = true;
        for (int y = 8; y <= 15; ++y) {
            for (int x = 10; x <= 27; ++x) {
                const Color c = p.get_pixel(x, y);
                if (c.m_r != exp.m_r || c.m_g != exp.m_g || c.m_b != exp.m_b) {
                    inner_ok = false;
                }
            }
        }
        AURORA_TEST_CHECK(inner_ok);
        // 矩形外未被污染
        const Color out = p.get_pixel(40, 30);
        AURORA_TEST_CHECK(out.m_r == 10 && out.m_g == 200 && out.m_b == 30);
        AURORA_LOG_INFO("test", "[1] opaque/alpha fill matches reference blend OK");
    }

    // 2) 矩形裁剪边界语义：与 coverage/contains（含右/下边界）像素级等价。
    //    clip = [10.0, 30.0]×[10.0, 20.0]：x=10..30、y=10..20（含端点）保留。
    {
        Painter p;
        p.begin(64, 48);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 48 } },
                    Color{ 255, 255, 255, 255 });
        p.push_clip(Rect{ .origin = Point{ .x = 10, .y = 10 }, .size = Size{ .width = 20, .height = 10 } });
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 48 } },
                    Color{ 0, 0, 0, 255 });
        p.pop_clip();
        auto black = [&](int x, int y) -> bool { return p.get_pixel(x, y).m_r == 0; };
        AURORA_TEST_CHECK(black(10, 10) && black(30, 20));  // 含头含尾（contains 含右/下边界）
        AURORA_TEST_CHECK(!black(9, 10) && !black(31, 20)); // 裁剪外
        AURORA_TEST_CHECK(!black(10, 9) && !black(10, 21));
        AURORA_LOG_INFO("test", "[2] rect-clip boundary matches contains() semantics OK");
    }

    // 3) 圆角裁剪走慢路径：角外像素被裁掉、中心保留（行为与历史一致）。
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } },
                    Color{ 255, 255, 255, 255 });
        p.push_clip_rounded(Rect{ .origin = Point{ .x = 8, .y = 8 }, .size = Size{ .width = 48, .height = 48 } },
                            16.0f);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } },
                    Color{ 0, 0, 0, 255 });
        p.pop_clip();
        AURORA_TEST_CHECK(p.get_pixel(32, 32).m_r == 0); // 中心被填充
        AURORA_TEST_CHECK(p.get_pixel(9, 9).m_r > 200);  // 圆角角外仍是白（被 SDF 裁掉）
        AURORA_LOG_INFO("test", "[3] rounded-clip slow path unchanged OK");
    }

    // 4) 全局透明度 <1 走慢路径：结果乘全局 alpha（与历史一致）。
    {
        Painter p;
        p.begin(16, 16);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 16, .height = 16 } },
                    Color{ 255, 255, 255, 255 });
        p.set_alpha(0.5);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 16, .height = 16 } },
                    Color{ 0, 0, 0, 255 });
        p.set_alpha(1.0);
        const Color c = p.get_pixel(8, 8);
        AURORA_TEST_CHECK(c.m_r > 100 && c.m_r < 155); // ≈127：半透明合成而非全黑覆写
        AURORA_LOG_INFO("test", "[4] global-alpha slow path unchanged OK");
    }
}
} // namespace aurora::tests::sec_painter_fill_fast

namespace aurora::tests::sec_painter_primitives {
namespace au = aurora;

namespace {
auto near_color(Color a, Color b, const int tol = 8) -> bool {
    return std::abs(a.m_r - b.m_r) <= tol && std::abs(a.m_g - b.m_g) <= tol && std::abs(a.m_b - b.m_b) <= tol;
}
} // namespace

static void run() {
    constexpr Color bg{ 255, 255, 255, 255 };
    constexpr Color red{ 255, 0, 0, 255 };

    // ---- 1. draw_line：水平线中心实体、远处不受影响、端点外圆帽内 ----
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        p.draw_line(Point{ .x = 10.0f, .y = 32.0f }, Point{ .x = 54.0f, .y = 32.0f }, 4.0f, red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 32), red)); // 线心
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 10), bg));  // 远离线
        AURORA_TEST_CHECK(near_color(p.get_pixel(5, 32), bg));   // 起点圆帽以外
        // 抗锯齿：分数坐标线段的羽化带像素应为红白过渡色（整对齐线段覆盖度恰为 0/1，
        // 故另画一条 y=48.25 的线验证 AA：像素 y=50 中心 50.5，dist=2.25 → cov=0.25）
        p.draw_line(Point{ .x = 10.0f, .y = 48.25f }, Point{ .x = 54.0f, .y = 48.25f }, 4.0f, red);
        const Color edge = p.get_pixel(32, 50);
        AURORA_TEST_CHECK(edge.m_r > 200 && edge.m_g > 30 && edge.m_g < 240);
    }

    // ---- 2. draw_line：斜线（勾号几何）两端可达 ----
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        p.draw_line(Point{ .x = 16.0f, .y = 16.0f }, Point{ .x = 48.0f, .y = 48.0f }, 3.0f, red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 32), red)); // 对角线中点
        AURORA_TEST_CHECK(near_color(p.get_pixel(48, 16), bg));  // 反对角不受影响
    }

    // ---- 3. fill_rounded_rect：中心填充、圆角角落保留背景 ----
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        p.fill_rounded_rect(Rect{ .origin = Point{ .x = 8, .y = 8 }, .size = Size{ .width = 48, .height = 48 } }, 12.0f,
                            red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 32), red)); // 中心
        AURORA_TEST_CHECK(near_color(p.get_pixel(9, 9), bg));    // 圆角外角落
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 10), red)); // 上边中部（非角落）
                                                                 // radius <= 0 退化为 fill_rect（角落也填充）
        Painter q;
        q.begin(32, 32);
        q.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 32, .height = 32 } }, bg);
        q.fill_rounded_rect(Rect{ .origin = Point{ .x = 4, .y = 4 }, .size = Size{ .width = 24, .height = 24 } }, 0.0f,
                            red);
        AURORA_TEST_CHECK(near_color(q.get_pixel(5, 5), red));
    }

    // ---- 3b. 回归：圆角裁剪 push/pop 严格配对，不泄漏裁剪栈 ----
    // 旧版 push_clip_rounded 每次压两个 ClipRegion 而 pop_clip 只弹一个，导致后续所有
    // 绘制被永久裁剪到首个圆角控件区域（demo 整窗白屏只剩一个 Checkbox）。
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rounded_rect(Rect{ .origin = Point{ .x = 4, .y = 4 }, .size = Size{ .width = 16, .height = 16 } }, 4.0f,
                            red);
        AURORA_TEST_CHECK_FALSE(p.has_clip()); // 配对后裁剪栈必须归零
                                               // 圆角控件之后的普通绘制不得被残留裁剪吞掉
        p.fill_rect(Rect{ .origin = Point{ .x = 40, .y = 40 }, .size = Size{ .width = 16, .height = 16 } }, red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(48, 48), red));
    }

    // ---- 4. draw_rounded_border：边框带实体、内部与外部保留背景 ----
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        p.draw_rounded_border(Rect{ .origin = Point{ .x = 8, .y = 8 }, .size = Size{ .width = 48, .height = 48 } },
                              8.0f, 3.0f, red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 9), red)); // 上边框带内（向内描边：y ∈ [8, 11]）
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 32), bg)); // 内部
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 4), bg));  // 外部
        AURORA_TEST_CHECK(near_color(p.get_pixel(9, 32), red)); // 左边框带内
    }

    // ---- 5. draw_rounded_border 圆环（radius = 半径）：RadioButton 外圈几何 ----
    {
        Painter p;
        p.begin(64, 64);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        p.draw_rounded_border(Rect{ .origin = Point{ .x = 16, .y = 16 }, .size = Size{ .width = 32, .height = 32 } },
                              16.0f, 3.0f, red);
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 17), red)); // 顶部环带
        AURORA_TEST_CHECK(near_color(p.get_pixel(32, 32), bg));  // 圆心
        AURORA_TEST_CHECK(near_color(p.get_pixel(17, 17), bg));  // 外接矩形角落（圆环之外）
    }

    // ---- 6. DisplayList 录制回放：新原语命令录制后 replay 与直绘一致 ----
    {
        Painter direct;
        direct.begin(64, 64);
        direct.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        direct.draw_line(Point{ .x = 10, .y = 32 }, Point{ .x = 54, .y = 32 }, 4.0f, red);
        direct.draw_rounded_border(Rect{ .origin = Point{ .x = 8, .y = 8 }, .size = Size{ .width = 48, .height = 48 } },
                                   8.0f, 2.0f, red);

        Painter replayed;
        replayed.begin(64, 64);
        replayed.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 64, .height = 64 } }, bg);
        DisplayList dl;
        replayed.record(dl);
        replayed.draw_line(Point{ .x = 10, .y = 32 }, Point{ .x = 54, .y = 32 }, 4.0f, red);
        replayed.draw_rounded_border(
            Rect{ .origin = Point{ .x = 8, .y = 8 }, .size = Size{ .width = 48, .height = 48 } }, 8.0f, 2.0f, red);
        replayed.stop();
        dl.replay(replayed);

        bool identical = true;
        for (int y = 0; y < 64 && identical; ++y) {
            for (int x = 0; x < 64; ++x) {
                const Color a = direct.get_pixel(x, y);
                const Color b = replayed.get_pixel(x, y);
                if (a.m_r != b.m_r || a.m_g != b.m_g || a.m_b != b.m_b || a.m_a != b.m_a) {
                    identical = false;
                    break;
                }
            }
        }
        AURORA_TEST_CHECK_MSG(identical,
                              "DisplayList replay bit-identical to direct draw (draw_line + rounded_border)");
    }
}
} // namespace aurora::tests::sec_painter_primitives

namespace aurora::tests::sec_painter_shift_pixels {

namespace {

/// @brief 逐行填不同灰阶（行号可辨识），任何行错位/漏搬都能被检出。
auto fill_row_ramp(Painter &p, int w, int h) -> void {
    for (int y = 0; y < h; ++y) {
        const auto v = static_cast<std::uint8_t>(10 + y);
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = static_cast<float>(y) },
                          .size = Size{ .width = static_cast<float>(w), .height = 1.0f } },
                    Color{ v, v, v, 255 });
    }
}

/// @brief 取物理像素行的红通道（fill_row_ramp 下等于该行标记值；未绘制行为 0）。
auto row_mark(const Painter &p, int y) -> int { return p.get_pixel(0, y).m_r; }

/// @brief 造一张含「全透明 / 半透明 / 不透明」三类像素的源图（Painter 缓冲 alpha 恒为 0 或 255，
///        只有 Image 源能覆盖 composite 的半透明混合分支）。
auto make_rgba_probe(int w, int h) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i =
                ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4u;
            img.pixels[i + 0] = static_cast<std::uint8_t>((x * 7) + 3);
            img.pixels[i + 1] = static_cast<std::uint8_t>((y * 11) + 5);
            img.pixels[i + 2] = static_cast<std::uint8_t>((x + y) * 5);
            // 三类 alpha 轮转：0（跳过）/ 128（混合）/ 255（覆写）。
            img.pixels[i + 3] = static_cast<std::uint8_t>((x + y) % 3 == 0 ? 0 : ((x + y) % 3 == 1 ? 128 : 255));
        }
    }
    return img;
}

/// @brief 两块画布逐字节比较（尺寸须一致）。
auto pixels_identical(const Painter &a, const Painter &b) -> bool {
    if (a.width() != b.width() || a.height() != b.height()) {
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(a.width()) * static_cast<std::size_t>(a.height()) * 4u;
    const std::uint8_t *pa = a.data();
    const std::uint8_t *pb = b.data();
    for (std::size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_painter_shift_pixels ===\n");

    // ---- 1) 内容下移（dy > 0）：顶部让出零基底，其余行整体后移 ----
    {
        Painter p;
        p.begin(4, 8);
        fill_row_ramp(p, 4, 8);
        p.shift_pixels(3.0f);
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 0 && row_mark(p, 1) == 0 && row_mark(p, 2) == 0,
                              "shift(+3): top 3 rows yield zero baseline");
        AURORA_TEST_CHECK_MSG(row_mark(p, 3) == 10 && row_mark(p, 4) == 11 && row_mark(p, 7) == 14,
                              "shift(+3): old rows 0..4 moved to 3..7");
    }

    // ---- 2) 内容上移（dy < 0）：底部让出零基底 ----
    {
        Painter p;
        p.begin(4, 8);
        fill_row_ramp(p, 4, 8);
        p.shift_pixels(-3.0f);
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 13 && row_mark(p, 4) == 17, "shift(-3): old rows 3..7 moved to 0..4");
        AURORA_TEST_CHECK_MSG(row_mark(p, 5) == 0 && row_mark(p, 6) == 0 && row_mark(p, 7) == 0,
                              "shift(-3): bottom 3 rows yield zero baseline");
    }

    // ---- 3) 位移 ≥ 缓冲高：无像素可复用，整块归零 ----
    {
        Painter p;
        p.begin(4, 8);
        fill_row_ramp(p, 4, 8);
        p.shift_pixels(100.0f);
        bool all_zero = true;
        for (int y = 0; y < 8; ++y) {
            all_zero = all_zero && row_mark(p, y) == 0;
        }
        AURORA_TEST_CHECK_MSG(all_zero, "shift(+100) exceeds buffer height: whole block returns to zero baseline");

        Painter q;
        q.begin(4, 8);
        fill_row_ramp(q, 4, 8);
        q.shift_pixels(-8.0f); // 恰好等于高度，同样无可复用像素
        bool q_zero = true;
        for (int y = 0; y < 8; ++y) {
            q_zero = q_zero && row_mark(q, y) == 0;
        }
        AURORA_TEST_CHECK_MSG(q_zero, "shift(-8) equals buffer height: whole block returns to zero baseline");
    }

    // ---- 4) dy 取整到 0 时为 no-op（亚像素滚动不得抹掉内容）----
    {
        Painter p;
        p.begin(4, 8);
        fill_row_ramp(p, 4, 8);
        p.shift_pixels(0.0f);
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 10 && row_mark(p, 7) == 17, "shift(0): pixels unchanged");
        p.shift_pixels(0.2f); // lround(0.2) == 0
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 10 && row_mark(p, 7) == 17,
                              "shift(0.2): rounded to 0 rows, pixels unchanged");
    }

    // ---- 5) scale ≠ 1：dy 为逻辑 dp，按 scale 换算物理行 ----
    {
        Painter p;
        p.set_scale(2.0f);
        p.begin(4, 8); // 物理 8×16
        AURORA_TEST_CHECK_MSG(p.height() == 16, "scale=2: physical height is 16");
        for (int y = 0; y < 16; ++y) {
            // 直接按物理行写标记：fill_rect 走逻辑 dp，这里要精确控制物理行。
            p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = static_cast<float>(y) / 2.0f },
                              .size = Size{ .width = 4.0f, .height = 0.5f } },
                        Color{ static_cast<std::uint8_t>(10 + y), 0, 0, 255 });
        }
        p.shift_pixels(1.5f); // 物理位移 lround(1.5*2) = 3 行
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 0 && row_mark(p, 2) == 0,
                              "scale=2 shift(1.5dp): top 3 physical rows yield");
        AURORA_TEST_CHECK_MSG(row_mark(p, 3) == 10, "scale=2 shift(1.5dp): physical row 0 moved to 3");
    }

    // ---- 6) 录制模式为 no-op（Display List 无对应命令，不得录出半成品）----
    {
        Painter p;
        p.begin(4, 8);
        fill_row_ramp(p, 4, 8);
        DisplayList dl;
        p.record(dl);
        p.shift_pixels(4.0f);
        p.stop();
        AURORA_TEST_CHECK_MSG(row_mark(p, 0) == 10 && row_mark(p, 7) == 17,
                              "shift_pixels in recording mode does not modify framebuffer");
    }

    // ---- 7) composite 快路径（纯平移 + 同 scale）与慢路径逐位一致 ----
    // 慢路径用「radius=0 的圆角裁剪」强制触发：m_has_rounded_clip 置位即回退逐像素分支，
    // 而 radius=0 的 SDF 在远离边界处覆盖度恒为 1，故理论结果应与快路径完全相同。
    {
        const Image probe = make_rgba_probe(9, 7);

        Painter fast;
        fast.begin(16, 16);
        fast.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 16.0f, .height = 16.0f } },
                       Color{ 30, 60, 90, 255 });
        fast.composite(probe, Matrix2D::from_translate(3.0f, 4.0f), 1.0f);

        Painter slow;
        slow.begin(16, 16);
        slow.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 16.0f, .height = 16.0f } },
                       Color{ 30, 60, 90, 255 });
        slow.push_clip_rounded(
            Rect{ .origin = Point{ .x = -8.0f, .y = -8.0f }, .size = Size{ .width = 32.0f, .height = 32.0f } }, 0.0f,
            true);
        slow.composite(probe, Matrix2D::from_translate(3.0f, 4.0f), 1.0f);
        slow.pop_clip();

        AURORA_TEST_CHECK_MSG(pixels_identical(fast, slow),
                              "composite pure-translate fast path bit-identical to slow path (integer translate)");
    }

    // ---- 8) 小数平移同样一致（映射表复刻慢路径表达式，不依赖平移量为整数）----
    {
        const Image probe = make_rgba_probe(9, 7);

        Painter fast;
        fast.begin(16, 16);
        fast.composite(probe, Matrix2D::from_translate(2.4f, -1.7f), 1.0f);

        Painter slow;
        slow.begin(16, 16);
        slow.push_clip_rounded(
            Rect{ .origin = Point{ .x = -8.0f, .y = -8.0f }, .size = Size{ .width = 32.0f, .height = 32.0f } }, 0.0f,
            true);
        slow.composite(probe, Matrix2D::from_translate(2.4f, -1.7f), 1.0f);
        slow.pop_clip();

        AURORA_TEST_CHECK_MSG(pixels_identical(fast, slow),
                              "composite fast path bit-identical to slow path (fractional translate, partial OOB)");
    }

    // ---- 9) scale=2 下仍逐位一致（高 DPI 是像素最多、最依赖快路径的场景）----
    {
        const Image probe = make_rgba_probe(12, 10);

        Painter fast;
        fast.set_scale(2.0f);
        fast.begin(16, 16);
        fast.composite(probe, Matrix2D::from_translate(1.5f, 2.5f), 2.0f);

        Painter slow;
        slow.set_scale(2.0f);
        slow.begin(16, 16);
        slow.push_clip_rounded(
            Rect{ .origin = Point{ .x = -8.0f, .y = -8.0f }, .size = Size{ .width = 40.0f, .height = 40.0f } }, 0.0f,
            true);
        slow.composite(probe, Matrix2D::from_translate(1.5f, 2.5f), 2.0f);
        slow.pop_clip();

        AURORA_TEST_CHECK_MSG(pixels_identical(fast, slow), "composite fast path bit-identical to slow path (scale=2)");
    }

    // ---- 10) Painter 源重载走同一核心（滚动 blit 的真实调用形态）----
    {
        Painter src;
        src.begin(10, 6);
        fill_row_ramp(src, 10, 6);

        Painter fast;
        fast.begin(16, 16);
        fast.composite(src, Matrix2D::from_translate(2.0f, 3.0f));

        Painter slow;
        slow.begin(16, 16);
        slow.push_clip_rounded(
            Rect{ .origin = Point{ .x = -8.0f, .y = -8.0f }, .size = Size{ .width = 32.0f, .height = 32.0f } }, 0.0f,
            true);
        slow.composite(src, Matrix2D::from_translate(2.0f, 3.0f));
        slow.pop_clip();

        AURORA_TEST_CHECK_MSG(pixels_identical(fast, slow), "composite(Painter) fast path bit-identical to slow path");
    }

    // ---- 11) 非纯平移（含旋转）仍走慢路径，结果不受快路径影响 ----
    {
        const Image probe = make_rgba_probe(8, 8);
        Painter a;
        a.begin(16, 16);
        a.composite(probe, Matrix2D::from_rotate(90.0f).compose(Matrix2D::from_translate(4.0f, 4.0f)), 1.0f);
        Painter b;
        b.begin(16, 16);
        b.composite(probe, Matrix2D::from_rotate(90.0f).compose(Matrix2D::from_translate(4.0f, 4.0f)), 1.0f);
        AURORA_TEST_CHECK_MSG(pixels_identical(a, b),
                              "rotation matrix takes slow path and result is stable/reproducible");
    }
}
} // namespace aurora::tests::sec_painter_shift_pixels

AURORA_TEST() {
    aurora::tests::sec_painter_aa::run();
    aurora::tests::sec_painter_fill_fast::run();
    aurora::tests::sec_painter_primitives::run();
    aurora::tests::sec_painter_shift_pixels::run();
}
