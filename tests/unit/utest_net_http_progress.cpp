/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_progress.h + src/aurora/net/detail/http_progress.cpp
/// 测试说明: 发起侧进度合流闸的发射次序回归。覆盖写入与发射解耦（`observe`
///           一发都不发）、一次可发射机会内每方向至多一发且发的是最新值、干净槽不被重复发射、帧号源在场
///           时以帧号为闸（同帧号内多次机会只发首发，帧号推进才再发）而未装帧号源时以宿主回合为闸（每次
///           `flush` 就是一个新回合）、`finish` 无视闸号把仍脏的槽发出（每方向末值恒发一次）、上传与下载
///           各占一枚槽互不覆盖（同一回合内两方向各发一次）、空回调时闸完全不作为（`armed` 假、三入口皆空
///           操作、一次调用都不发生）。
/// 全用例零 socket、零时钟、零线程——闸本身是个纯状态机，把发射频次写成可与回合数逐名对齐的判据，正是
/// 「按 chunk 发射会放大成重绘风暴」这条约束唯一能被证伪的形态；三平台行为一致，wasm 腿同样可跑。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "aurora/net/detail/http_progress.h"
#include "aurora/net/http.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_progress {
namespace {

using aurora::net::detail::ProgressGate;
using aurora::net::http::Progress;
using aurora::net::http::ProgressPhase;

/// @brief 记下一笔进度：`observe` 与发射两条路都经它，用例据此区分「记了几笔」与「发出去几笔」。
/// @param phase 方向。
/// @param transferred 已累计字节数。
/// @param total 总量。
/// @param settled 是否已收摊。
/// @return 进度值。
[[nodiscard]] auto sample(ProgressPhase phase, std::uint64_t transferred, std::uint64_t total, bool settled = false)
    -> Progress {
    Progress progress;
    progress.phase = phase;
    progress.transferred_bytes = transferred;
    progress.total_bytes = total;
    progress.total_known = true;
    progress.finished = settled;
    return progress;
}

/// @brief 收集器：把闸的每次发射按次序落到一张表里，供逐值断言。
/// @param sink 目标表（生命周期由用例持有，闸只在用例体内被驱动）。
/// @return 可直接交给 `ProgressGate` 的回调。
[[nodiscard]] auto recorder(std::vector<Progress> &sink) -> aurora::net::http::ProgressHandler {
    return [&sink](const Progress &progress) -> void { sink.push_back(progress); };
}

/// @brief 一帧恒定的帧号源（用可变态推进，用例自己决定何时「换帧」）。
struct FakeFrames {
    std::uint64_t frame{1};  ///< 当前帧号
};

/// @brief 造一枚以 `frames` 为帧号源的闸。
/// @param frames 帧号状态。
/// @param sink 发射收集表。
/// @return 填好的闸（移动返回，故调用处直接初始化）。
[[nodiscard]] auto gated_with_frames(FakeFrames &frames, std::vector<Progress> &sink) -> ProgressGate {
    return ProgressGate{recorder(sink), [&frames]() -> std::uint64_t { return frames.frame; }};
}

}  // namespace

AURORA_TEST_CASE(observe_alone_never_calls_the_handler) {
    // 写入与发射解耦：字节到达只更新槽值。这条一旦破了，「对端切得碎」就直接等于「回调按 chunk 打爆状态」。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Download, 8, 100));
    gate.observe(sample(ProgressPhase::Download, 16, 100));
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{0});
}

AURORA_TEST_CASE(one_turn_emits_at_most_one_value_per_direction_and_it_is_the_latest) {
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Upload, 10, 100));
    gate.observe(sample(ProgressPhase::Upload, 20, 100));
    gate.observe(sample(ProgressPhase::Upload, 30, 100));
    gate.flush();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(seen.at(0).phase == ProgressPhase::Upload);
    AURORA_TEST_CHECK_EQ(seen.at(0).transferred_bytes, std::uint64_t{30});

    // 下一回合再来一笔：新回合是新机会，脏槽再发一次。
    gate.observe(sample(ProgressPhase::Upload, 40, 100));
    gate.flush();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen.at(1).transferred_bytes, std::uint64_t{40});
}

AURORA_TEST_CASE(a_clean_slot_is_not_re_emitted_on_the_next_chance) {
    // 合流不等于「每回合都要发一笔」：没有新字节就没有发射，静止的下载不该一直敲调用方。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Download, 5, 10));
    gate.flush();
    gate.flush();
    gate.flush();
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{1});
}

AURORA_TEST_CASE(the_frame_source_replaces_turns_as_the_gate) {
    // 装了帧号源即以帧号为闸：同一帧内三次机会只发首发，换帧后才发第二发。这是提案「每帧至多一次」的
    // 严格形态，回合闸是它的缺省回落。
    FakeFrames frames;
    std::vector<Progress> seen;
    ProgressGate gate = gated_with_frames(frames, seen);
    gate.observe(sample(ProgressPhase::Download, 1, 100));
    gate.flush();
    gate.observe(sample(ProgressPhase::Download, 2, 100));
    gate.flush();
    gate.observe(sample(ProgressPhase::Download, 3, 100));
    gate.flush();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen.at(0).transferred_bytes, std::uint64_t{1});

    frames.frame = 2;
    gate.flush();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen.at(1).transferred_bytes, std::uint64_t{3});
    AURORA_TEST_CHECK_EQ(gate.gate(), std::uint64_t{2});
}

AURORA_TEST_CASE(the_turn_gate_advances_one_step_per_flush) {
    // 缺省回合闸：闸号就是 flush 的次数，`gate()` 是可观测的判据口。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    AURORA_TEST_CHECK_EQ(gate.gate(), std::uint64_t{0});
    gate.flush();
    AURORA_TEST_CHECK_EQ(gate.gate(), std::uint64_t{1});
    gate.flush();
    AURORA_TEST_CHECK_EQ(gate.gate(), std::uint64_t{2});
}

AURORA_TEST_CASE(finish_emits_the_pending_value_ignoring_the_gate) {
    // 末值恒发：刚在同一机会里发过也要再发一笔带「收摊」事实的，否则调用方只能靠比字节数猜完成。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Download, 90, 100));
    gate.flush();
    gate.observe(sample(ProgressPhase::Download, 100, 100, true));
    gate.finish();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_FALSE(seen.at(0).finished);
    AURORA_TEST_CHECK_TRUE(seen.at(1).finished);
    AURORA_TEST_CHECK_EQ(seen.at(1).transferred_bytes, std::uint64_t{100});

    // 收尾之后没有脏槽：重复 finish 不再发。
    gate.finish();
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{2});
}

AURORA_TEST_CASE(finish_also_settles_a_slot_that_never_got_its_turn) {
    // 一上来就收摊（连接失败、头部都没到）也要发得出末值：机会没来过不等于可以不报。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Upload, 0, 4096, true));
    gate.finish();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(seen.at(0).phase == ProgressPhase::Upload);
    AURORA_TEST_CHECK_TRUE(seen.at(0).finished);
}

AURORA_TEST_CASE(the_two_directions_have_independent_slots) {
    // 两方向各一枚槽：同一回合里各发一次、互不覆盖。合成一枚槽的话上传会把下载的最新值吃掉。
    std::vector<Progress> seen;
    ProgressGate gate{recorder(seen)};
    gate.observe(sample(ProgressPhase::Upload, 64, 64, true));
    gate.observe(sample(ProgressPhase::Download, 7, 100));
    gate.flush();
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    const bool upload_first = seen.at(0).phase == ProgressPhase::Upload && seen.at(1).phase == ProgressPhase::Download;
    AURORA_TEST_CHECK_TRUE(upload_first);
    AURORA_TEST_CHECK_EQ(seen.at(0).transferred_bytes, std::uint64_t{64});
    AURORA_TEST_CHECK_EQ(seen.at(1).transferred_bytes, std::uint64_t{7});
}

AURORA_TEST_CASE(an_unarmed_gate_does_nothing_at_all) {
    // 空回调 = 整个闸不工作：连回合号都不推进。这是「无观测需求的调用方零行为改动」那条口径的判据——
    // 挂了个空回调不该让状态机白转一圈。
    std::vector<Progress> seen;
    ProgressGate gate{aurora::net::http::ProgressHandler{}};
    AURORA_TEST_CHECK_FALSE(gate.armed());
    gate.observe(sample(ProgressPhase::Download, 1, 2));
    gate.flush();
    gate.finish();
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(gate.gate(), std::uint64_t{0});
}

}  // namespace aurora::test_cases::utest_net_http_progress
