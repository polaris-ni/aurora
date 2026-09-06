// render_util.h — Aurora 公共测试 fixture（header-only，ODR 安全）。
//
// 归一化 8 处重复的 RenderResult / render_in_root 实现（test_blend_mode / test_skeleton /
// test_modifier_transform / test_scroll / test_cache_layer / test_grid_view / test_shader_mask /
// test_navigator 等）。行为以「拷贝主缓冲像素」为准。
//
// 使用：测试体位于 namespace aurora::test_cases::<stem>，类型在外层 aurora 命名空间下，
// 故可直接写 test_common::RenderResult / test_common::render_in_root 裸名。

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "aurora/aurora.h"

namespace aurora::test_common {

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0;
    int h = 0;

    // 归一：const 成员函数 + 非 const 形参；按 (y*w + x)*4 + ch 索引 RGBA。
    [[nodiscard]] auto at(int x, int y, int ch) const -> std::uint8_t {
        const std::size_t off = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                 static_cast<std::size_t>(x)) * 4U;
        return pixels.at(off + static_cast<std::size_t>(ch));
    }
};

// 无头 layout + paint 到 ww×hh 离屏缓冲，拷贝主像素缓冲返回。
[[nodiscard]] inline auto render_in_root(std::shared_ptr<Widget> w, int ww, int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{Node{std::move(w)}});
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                      .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w = ww;
    r.h = hh;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * static_cast<std::size_t>(hh) * 4U));
    return r;
}

}  // namespace aurora::test_common
