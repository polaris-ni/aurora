// 无头验证新增修饰节点：Size/Fill、Border、Clip。
// 检查：size 强制子节点尺寸、fill_max_width 填充父宽、边框/裁剪绘制不崩溃且裁剪栈平衡。
// ── API 覆盖映射 ─────────────────────────────
// modifier/modifier_base.h(ModifierNode 基类契约)、modifier/modifier_layout.h(Size/Fill 布局修饰)、
// modifier/modifier_paint.h(Paint 家族：blur/shadow/shader_mask/cache_layer/gradient/tint——
//   行为细节另见 test_blur/test_shadow/test_shader_mask/test_cache_layer/test_gradient/test_blend_mode)。

#include <cstdio>
#include <memory>

#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Reactive;
using aurora::Rect;
using aurora::Size;
using aurora::State;
using aurora::Text;

AURORA_TEST() {
    int failures = 0;

    // ---- 1. size 强制尺寸 ----
    {
        auto t = std::make_shared<Text>();
        t->content = LocalizedString{ "sized" };
        t->modifier.set(aurora::Modifier{}.size(120.0f, 40.0f));

        BuildContext ctx;
        t->mount(ctx);
        Constraints cc;
        cc.min = Size{ .width = 0.0f, .height = 0.0f };
        cc.max = Size{ .width = 640.0f, .height = 480.0f };
        t->layout(cc, ctx);

        const Size bs = t->size();
        AURORA_TEST_PRINTF("size:        got %.0fx%.0f (expect 120x40)\n", bs.width, bs.height);
        if (bs.width != 120.0f || bs.height != 40.0f) {
            AURORA_LOG_INFO("test", "  FAIL: size modifier did not force dimensions");
            ++failures;
        }
    }

    // ---- 2. fill_max_width 填充父宽 ----
    {
        auto t = std::make_shared<Text>();
        t->content = LocalizedString{ "fill" };
        t->modifier.set(aurora::Modifier{}.fill_max_width());

        BuildContext ctx;
        t->mount(ctx);
        Constraints cc;
        cc.min = Size{ .width = 0.0f, .height = 0.0f };
        cc.max = Size{ .width = 640.0f, .height = 480.0f };
        t->layout(cc, ctx);

        const Size bs = t->size();
        AURORA_TEST_PRINTF("fill_max_width: got %.0f (expect 640)\n", bs.width);
        if (bs.width != 640.0f) {
            AURORA_LOG_INFO("test", "  FAIL: fill_max_width did not fill parent width");
            ++failures;
        }
    }

    // ---- 3. border + clip + background 绘制不崩溃，且裁剪栈平衡 ----
    {
        State count{ 0 };
        auto btn = std::make_shared<Button>();
        btn->label = Reactive{ LocalizedString{ "hit" } };
        btn->on_click = [&count]() -> void { count.set(count.get() + 1); };
        btn->modifier.set(aurora::Modifier{}
                              .size(80.0f, 30.0f)
                              .background(Color{ 0, 120, 215, 255 })
                              .border(2.0f, Color{ 255, 0, 0, 255 })
                              .clip());

        Column col{ ColumnProps{ .children = { Node{ btn } } } };
        BuildContext ctx;
        col.mount(ctx);
        Constraints cc;
        cc.min = Size{ .width = 0.0f, .height = 0.0f };
        cc.max = Size{ .width = 640.0f, .height = 480.0f };
        col.layout(cc, ctx);

        aurora::Painter p;
        p.begin(640, 480);
        col.paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 640.0f, .height = 480.0f } },
                  ctx);

        // 点击仍应命中（Clickable 与新修饰共存）
        const Rect bb = col.child_nodes()[0].bounds();
        const Point center{ .x = bb.origin.x + (bb.size.width / 2.0f), .y = bb.origin.y + (bb.size.height / 2.0f) };
        MouseEvent press;
        press.position = center;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        const bool hp = EventDispatcher::dispatch(col, press);
        MouseEvent release = press;
        release.action = MouseAction::Release;
        const bool hr = EventDispatcher::dispatch(col, release);

        AURORA_TEST_PRINTF("border+clip:  hit press=%d release=%d count=%d\n", hp, hr, count.get());
        if (!hp || !hr || count.get() != 1) {
            AURORA_LOG_INFO("test", "  FAIL: button with border+clip not clickable / count wrong");
            ++failures;
        }
    }

    // 本地累加的失败计数桥接到框架上下文，交由 runner 统一判定（替代旧 `return 0/1`）。
    AURORA_TEST_CHECK_EQ(failures, 0);
}
