// solid_page.h — Aurora 公共测试 fixture（header-only，ODR 安全）。
//
// 归一化 5 处重复的 SolidPage 纯色页定义（test_navigator ×2、test_window ×2、test_widget ×1）。
// 行为以「constrain(c.max) 布局 + fill_rect(bounds, bg) 绘制」为准。

#pragma once

#include <cstdint>
#include <vector>

#include "aurora/aurora.h"

namespace aurora::test_common {

struct SolidPage : Widget {
    Color bg;

    explicit SolidPage(Color c) : bg(c) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }

    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, bg);
    }
};

}  // namespace aurora::test_common
