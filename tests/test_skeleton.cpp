// Skeleton 骨架屏验证：底色渲染 + shimmer 相位随 tick 推进。

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Json;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Skeleton;
using aurora::Stack;
using aurora::Widget;

namespace {

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0, h = 0;
    [[nodiscard]] auto at(int x, const int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels[off + ch];
    }
};

auto render_in_root(std::shared_ptr<Widget> w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{ Node{ std::move(w) } });
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) };
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) } },
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w = ww;
    r.h = hh;
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}
} // namespace

AURORA_TEST() {
    // 底色渲染：Skeleton 占满宽度，底色灰
    const auto sk = std::make_shared<Skeleton>(Size{ .width = 100.0f, .height = 20.0f });
    sk->set_color(Color{ 200, 200, 200 }).set_highlight(Color{ 255, 255, 255 }).set_duration(1.0);
    const auto r = render_in_root(sk, 200, 200);
    AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 200, "base color renders (gray 200)");

    // 相位初始 0
    AURORA_TEST_CHECK_MSG(sk->phase() == 0.0, "initial phase 0");

    // tick 推进相位：用两个不同时间点
    const auto t0 = std::chrono::steady_clock::now();
    sk->tick(t0);
    // 等 ~60ms 后再 tick（duration=1s → phase≈0.06）
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sk->tick(std::chrono::steady_clock::now());
    AURORA_TEST_CHECK_MSG(sk->phase() > 0.0, "phase advances after tick (>0)");
    AURORA_TEST_CHECK_MSG(sk->phase() < 1.0, "phase within [0,1)");

    // 序列化往返
    Json props = Json::object();
    sk->serialize_props(props);
    const auto sk2 = std::make_shared<Skeleton>();
    sk2->deserialize_props(props);
    AURORA_TEST_CHECK_MSG(sk2->size_hint().height == 20.0f, "deserialize preserves height");
    AURORA_TEST_CHECK_MSG(sk2->phase() == 0.0, "deserialize yields fresh phase 0");
}
