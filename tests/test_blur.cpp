// 验证模糊修饰：Painter::blur_region 像素级模糊 + Modifier::blur/backdrop_filter 集成。
#include <cstdio>
#include <memory>

#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BlurNode;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::LocalizedString;
using aurora::Modifier;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Text;

AURORA_TEST() {
    // ---- 1. BlurNode 构造与降级 ----
    {
        BlurNode b1{ 4.0f };
        AURORA_TEST_CHECK(b1.radius() == 4.0f);
        AURORA_TEST_CHECK(!b1.is_backdrop());

        BlurNode b2{ 8.0f, true };
        AURORA_TEST_CHECK(b2.is_backdrop());

        BlurNode b3{ -5.0f };
        AURORA_TEST_CHECK(b3.radius() == 0.0f); // 负值降级
    }

    // ---- 2. Modifier 工厂 ----
    {
        auto mod = Modifier{}.blur(3.0f).backdrop_filter(6.0f);
        AURORA_TEST_CHECK(mod.nodes().size() == 2);
        const auto *n0 = dynamic_cast<const BlurNode *>(mod.nodes()[0].get());
        const auto *n1 = dynamic_cast<const BlurNode *>(mod.nodes()[1].get());
        AURORA_TEST_CHECK(n0 != nullptr && !n0->is_backdrop());
        AURORA_TEST_CHECK(n1 != nullptr && n1->is_backdrop());
    }

    // ---- 3. blur_region 像素级验证：锐利边界被平滑 ----
    {
        Painter p;
        p.begin(100, 100);
        // 左半黑右半白（x=50 处硬边界）
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 100.0f } },
                    Color(0, 0, 0, 255));
        p.fill_rect(Rect{ .origin = Point{ .x = 50.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 100.0f } },
                    Color(255, 255, 255, 255));

        // 模糊前：边界两侧对比强烈
        const auto before_l = p.get_pixel(48, 50);
        const auto before_r = p.get_pixel(52, 50);
        AURORA_TEST_CHECK(before_l.m_r == 0);
        AURORA_TEST_CHECK(before_r.m_r == 255);

        p.blur_region(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 100.0f, .height = 100.0f } }, 5.0f);

        // 模糊后：边界处出现中间灰度
        const auto after_edge = p.get_pixel(50, 50);
        AURORA_TEST_CHECK(after_edge.m_r > 30 && after_edge.m_r < 225);
        // 远离边界处基本不变
        const auto after_far_l = p.get_pixel(5, 50);
        const auto after_far_r = p.get_pixel(95, 50);
        AURORA_TEST_CHECK(after_far_l.m_r < 30);
        AURORA_TEST_CHECK(after_far_r.m_r > 225);
    }

    // ---- 4. blur_region 仅影响指定区域 ----
    {
        Painter p;
        p.begin(100, 100);
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 100.0f } },
                    Color(0, 0, 0, 255));
        p.fill_rect(Rect{ .origin = Point{ .x = 50.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 100.0f } },
                    Color(255, 255, 255, 255));

        // 只模糊上半 30 行
        p.blur_region(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 100.0f, .height = 30.0f } },
                      5.0f);

        // 上半边界模糊
        AURORA_TEST_CHECK(p.get_pixel(50, 15).m_r > 30 && p.get_pixel(50, 15).m_r < 225);
        // 下半边界仍锐利
        AURORA_TEST_CHECK(p.get_pixel(48, 80).m_r == 0);
        AURORA_TEST_CHECK(p.get_pixel(52, 80).m_r == 255);
    }

    // ---- 5. 非法参数无操作 ----
    {
        Painter p;
        p.begin(50, 50);
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 50.0f } },
                    Color(100, 100, 100, 255));

        p.blur_region(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 50.0f } },
                      0.0f); // 零半径
        p.blur_region(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 50.0f } },
                      -3.0f); // 负半径
        p.blur_region(
            Rect{ .origin = Point{ .x = 200.0f, .y = 200.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } },
            5.0f);                                         // 区域出界
        AURORA_TEST_CHECK(p.get_pixel(25, 25).m_r == 100); // 全部无操作
    }

    // ---- 6. Widget 集成：带 blur 修饰的控件绘制不崩溃 ----
    {
        auto t = std::make_shared<Text>();
        t->content = LocalizedString{ "blurred text" };
        t->modifier.set(Modifier{}.background(Color(255, 0, 0, 255)).blur(2.0f));

        BuildContext ctx;
        t->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 200.0f, .height = 100.0f };
        t->layout(c, ctx);

        Painter p;
        p.begin(200, 100);
        t->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 100.0f } },
                 ctx);
        AURORA_TEST_CHECK(p.width() == 200);
    }

    // ---- 7. Widget 集成：backdrop_filter 毛玻璃不崩溃 ----
    {
        auto t = std::make_shared<Text>();
        t->content = LocalizedString{ "frosted" };
        t->modifier.set(Modifier{}.backdrop_filter(4.0f).background(Color(255, 255, 255, 120)));

        BuildContext ctx;
        t->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 200.0f, .height = 100.0f };
        t->layout(c, ctx);

        Painter p;
        p.begin(200, 100);
        // 背景先画点内容供模糊
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 100.0f, .height = 100.0f } },
                    Color(0, 0, 255, 255));
        t->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 200.0f, .height = 100.0f } },
                 ctx);
        AURORA_TEST_CHECK(p.width() == 200);
    }
}
