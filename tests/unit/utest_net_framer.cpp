/// 测试类型: unit
/// 目标单元: include/aurora/net/framer.h
/// 测试说明: aurora::net::Framer 的三型分帧判据：行型的 LF / CRLF 终止与「裸 CR
///           不是终止符」、空行是空帧而不是噪声（SSE 的事件边界）、分隔符型跨 `feed()` 边界的命中与
///           连续命中之间的空帧、长度前缀型的大端 / 小端与「声明值含不含前缀自身」、拉模型背压（回调
///           返回 false 即暂停，剩余字节由下一次空 span `feed()` 续投且不重复投递）、两枚终态失败
///           （`net-framer-overflow` / `net-buffer-limit`）在 `reset()` 前原样回报、`finish()` 的型别
///           分歧（行 / 分隔符补出 `unterminated` 尾巴，长度前缀丢弃未兑现的残片并报字节数）、工厂口径
///           校验（上限为 0、缓冲小于一帧、空分隔符、分隔符超上限、长度字段宽度越界）、移动后落点与
///           空回调的惰性形态。本类型不含 socket 与平台头，故全部判据在三平台逐值同色，用例不设平台分支。

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/framer.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_framer {
namespace {

using aurora::net::Frame;
using aurora::net::FrameOptions;
using aurora::net::Framer;
using aurora::net::FrameSink;
using aurora::net::LengthByteOrder;

/// @brief 帧出口的记录器：抄下交付的帧体与形态，并从第 `reject_from` 帧起返回 false（即暂停）。
struct Recorder {
    std::vector<std::string> frames;  ///< 已交付帧体的抄本（回调返回后视图即失效）
    std::vector<bool> unterminated;  ///< 对应帧是否由 `finish()` 补出
    std::size_t reject_from = std::numeric_limits<std::size_t>::max();  ///< 从第几帧起回答「别再投了」

    /// @brief 取回调本体：收满 `reject_from - 1` 帧后返回 false，本层据此暂停且不丢字节。
    /// @return 可直接交给 `feed()` / `finish()` 的出口。
    auto sink() -> FrameSink {
        return [this](const Frame &frame) -> bool {
            frames.emplace_back(frame.bytes);
            unterminated.push_back(frame.unterminated);
            return frames.size() < reject_from;
        };
    }
};

/// @brief 造一份只改两枚上限的口径（其余字段取缺省）。
/// @param max_frame 单帧上限。
/// @param max_buffer 缓冲硬上限。
/// @return 分帧口径。
auto limits(std::size_t max_frame, std::size_t max_buffer) -> FrameOptions {
    return FrameOptions{.max_frame_bytes = max_frame, .max_buffered_bytes = max_buffer};
}

/// @brief 把字节喂进分帧器并要求成功（这是「本应成功」的前置步骤，不是被测行为）。
/// @param framer 目标分帧器。
/// @param text 待喂字节。
/// @param sink 帧出口。
/// @return 本次交付报告。
auto feed_or_die(Framer &framer, std::span<const char> text, const FrameSink &sink) -> aurora::net::FeedReport {
    auto fed = framer.feed(text, sink);
    AURORA_TEST_REQUIRE_MSG(fed.ok(), fed ? "" : fed.error().code + " " + fed.error().message);
    return fed.value();
}

/// @brief 字面量形态的 `feed_or_die`：长度取 `sizeof - 1`，因此含 `\0` 的二进制输入逐字节如实投递
///        （`std::string_view` 会在首个 `\0` 截断，长度前缀型会被读成空输入而把整条用例走歪）。
/// @tparam N 字面量数组长度（含结尾 `\0`）。
/// @param framer 目标分帧器。
/// @param literal 待喂字节字面量。
/// @param sink 帧出口。
/// @return 本次交付报告。
template <std::size_t N>
auto must_feed(Framer &framer, const char (&literal)[N], const FrameSink &sink) -> aurora::net::FeedReport {
    return feed_or_die(framer, std::span<const char>(literal, N - 1), sink);
}

/// @brief 按 code 判定失败形态（不依赖措辞）。
/// @param error 待判错误。
/// @param slug 期望的 slug。
/// @return 匹配为 true。
auto is_code(const aurora::Error &error, std::string_view slug) -> bool { return error.code == slug; }

}  // namespace

// 行型：LF 与 CRLF 都终止且终止符不进帧体；一帧可以跨多次 feed，一次 feed 也可以出多帧。
AURORA_TEST_CASE(lines_splits_on_lf_and_crlf) {
    auto built = Framer::lines();
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    const auto first = must_feed(framer, "alpha\r\nbeta\n gam", rec.sink());
    AURORA_TEST_CHECK_EQ(first.delivered, std::size_t{2});
    AURORA_TEST_CHECK_FALSE(first.sink_paused);
    AURORA_TEST_CHECK_EQ(first.buffered_bytes, std::size_t{4});  // " gam" 凑不成一帧，留在缓冲里
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("alpha"));
    AURORA_TEST_CHECK_EQ(rec.frames[1], std::string("beta"));
    AURORA_TEST_CHECK_FALSE(rec.unterminated[0]);

    const auto rest = must_feed(framer, "e\r\n", rec.sink());
    AURORA_TEST_CHECK_EQ(rest.delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.frames[2], std::string(" game"));
    AURORA_TEST_CHECK_EQ(framer.buffered_bytes(), std::size_t{0});
}

// 空行是一条空帧：SSE 的事件边界正是空行，当噪声丢掉就等于把所有事件粘成一条。
AURORA_TEST_CASE(lines_keep_empty_frames) {
    auto built = Framer::lines(limits(64, 4096));
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    const auto fed = must_feed(framer, "\n\n\r\n", rec.sink());
    AURORA_TEST_CHECK_EQ(fed.delivered, std::size_t{3});
    AURORA_TEST_CHECK_EQ(rec.frames.size(), std::size_t{3});
    AURORA_TEST_CHECK_TRUE(rec.frames[0].empty());
    AURORA_TEST_CHECK_TRUE(rec.frames[2].empty());  // CRLF 之间同样是空帧
}

// 裸 CR 不是行终止符：单独的 CR 只是帧体里的一个字节。
AURORA_TEST_CASE(bare_cr_is_data_not_a_line_terminator) {
    auto built = Framer::lines();
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    const auto fed = must_feed(framer, "a\rb\n", rec.sink());
    AURORA_TEST_CHECK_EQ(fed.delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("a\rb"));
}

// 分隔符型：分隔符可横跨两次 feed 的边界，连续两次命中之间是空帧。
AURORA_TEST_CASE(delimiter_matches_across_feed_boundary) {
    auto built = Framer::until("||");
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    AURORA_TEST_CHECK_EQ(must_feed(framer, "one|", rec.sink()).delivered, std::size_t{0});
    AURORA_TEST_CHECK_EQ(framer.buffered_bytes(), std::size_t{4});
    const auto second = must_feed(framer, "|two||", rec.sink());
    AURORA_TEST_CHECK_EQ(second.delivered, std::size_t{2});
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("one"));
    AURORA_TEST_CHECK_EQ(rec.frames[1], std::string("two"));
    AURORA_TEST_CHECK_EQ(second.buffered_bytes, std::size_t{0});

    // 分隔符紧挨着即空帧，与行型的空行同判据。
    Recorder empty_side;
    AURORA_TEST_CHECK_EQ(must_feed(framer, "||", empty_side.sink()).delivered, std::size_t{1});
    AURORA_TEST_CHECK_TRUE(empty_side.frames[0].empty());
}

// 拉模型背压：回调返回 false 即暂停，剩余字节不丢也不重复投递，空 span 的 feed 只负责续投。
AURORA_TEST_CASE(sink_pause_then_resume_without_duplicates) {
    auto built = Framer::lines(limits(64, 4096));
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder paused{.reject_from = 1};
    const auto first = must_feed(framer, "a\nb\nc\n", paused.sink());
    AURORA_TEST_CHECK_EQ(first.delivered, std::size_t{1});  // 被拒的那一帧本身已经交付
    AURORA_TEST_CHECK_TRUE(first.sink_paused);
    AURORA_TEST_CHECK_EQ(first.buffered_bytes, std::size_t{4});  // "b\nc\n"

    Recorder resumed;
    const auto second = must_feed(framer, "", resumed.sink());
    AURORA_TEST_CHECK_EQ(second.delivered, std::size_t{2});
    AURORA_TEST_CHECK_FALSE(second.sink_paused);
    AURORA_TEST_CHECK_EQ(resumed.frames[0], std::string("b"));
    AURORA_TEST_CHECK_EQ(resumed.frames[1], std::string("c"));

    // 空回调即「一条都不投」：字节照吃，缓冲不动。
    const auto third = must_feed(framer, "d\n", FrameSink{});
    AURORA_TEST_CHECK_EQ(third.delivered, std::size_t{0});
    AURORA_TEST_CHECK_TRUE(third.sink_paused);
    AURORA_TEST_CHECK_EQ(framer.buffered_bytes(), std::size_t{2});
    Recorder drained;
    AURORA_TEST_CHECK_EQ(must_feed(framer, "", drained.sink()).delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(drained.frames[0], std::string("d"));
}

// 单帧超上限即终态失败：此后每次操作原样回报同一错误，直到 reset() 才回到可用形态。
AURORA_TEST_CASE(oversized_message_is_terminal_until_reset) {
    auto built = Framer::lines(limits(8, 4096));
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    const auto failed = framer.feed(std::span<const char>("0123456789", 10), rec.sink());
    AURORA_TEST_REQUIRE_FALSE(failed.ok());
    AURORA_TEST_CHECK_TRUE(is_code(failed.error(), "net-framer-overflow"));

    // 终态：哪怕现在补上了终止符，也不再交付，而是重报同一条错误。
    const auto again = framer.feed(std::span<const char>("\n", 1), rec.sink());
    AURORA_TEST_REQUIRE_FALSE(again.ok());
    AURORA_TEST_CHECK_EQ(again.error().code, failed.error().code);
    const auto ended = framer.finish(rec.sink());
    AURORA_TEST_REQUIRE_FALSE(ended.ok());
    AURORA_TEST_CHECK_TRUE(is_code(ended.error(), "net-framer-overflow"));
    AURORA_TEST_CHECK_EQ(rec.frames.size(), std::size_t{0});

    framer.reset();
    Recorder fresh;
    AURORA_TEST_CHECK_EQ(must_feed(framer, "ok\n", fresh.sink()).delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(fresh.frames[0], std::string("ok"));
}

// 缓冲硬上限：暂停期间继续喂字节会把缓冲堆过上限，转 `net-buffer-limit`，不存在无界缓冲。
AURORA_TEST_CASE(buffer_cap_rejects_a_slow_consumer) {
    auto built = Framer::until("END", limits(8, 16));
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder refuses{.reject_from = 1};
    const auto first = must_feed(framer, "012END345END678", refuses.sink());
    AURORA_TEST_CHECK_EQ(first.delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(first.buffered_bytes, std::size_t{9});  // "345END678"

    // 再喂 16 字节：交付被拒的那一帧后剩 19 字节，越过 16 的上限。
    const auto second = framer.feed(std::span<const char>("1234567890123456", 16), refuses.sink());
    AURORA_TEST_REQUIRE_FALSE(second.ok());
    AURORA_TEST_CHECK_TRUE(is_code(second.error(), "net-buffer-limit"));

    const auto third = framer.feed(std::span<const char>("x", 1), refuses.sink());
    AURORA_TEST_REQUIRE_FALSE(third.ok());
    AURORA_TEST_CHECK_EQ(third.error().code, second.error().code);  // 同一条终态错误，原样回报
}

// 长度前缀型：大端 / 小端各按口径解码，长度字段与帧体都能跨 feed 拼装。
AURORA_TEST_CASE(length_prefixed_decodes_both_byte_orders) {
    FrameOptions big = limits(64, 4096);
    big.byte_order = LengthByteOrder::Big;
    big.length_field_bytes = 2;
    auto built_big = Framer::length_prefixed(big);
    AURORA_TEST_REQUIRE(built_big.ok());
    Framer framer = std::move(built_big.value());

    Recorder rec;
    // 长度前缀的输入一律写成相邻字面量拼接：`\x` 转义会贪婪吞掉后续十六进制数字，
    // `"\x03abc"` 实际是一个越界转义而不是「0x03 后跟 abc」。
    AURORA_TEST_CHECK_EQ(must_feed(framer, "\x00\x05", rec.sink()).delivered, std::size_t{0});  // 只有长度字段
    AURORA_TEST_CHECK_EQ(must_feed(framer, "hel", rec.sink()).delivered, std::size_t{0});  // 帧体还差两字节
    const auto body = must_feed(framer, "lo", rec.sink());
    AURORA_TEST_CHECK_EQ(body.delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("hello"));
    AURORA_TEST_CHECK_EQ(body.buffered_bytes, std::size_t{0});

    // 下一帧的长度字段本身被拆成两次投递：解码只能在字节凑齐后发生。
    AURORA_TEST_CHECK_EQ(must_feed(framer, "\x00", rec.sink()).delivered, std::size_t{0});
    AURORA_TEST_CHECK_EQ(must_feed(framer, "\x02XY", rec.sink()).delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.frames[1], std::string("XY"));

    FrameOptions little = limits(64, 4096);
    little.byte_order = LengthByteOrder::Little;
    little.length_field_bytes = 1;
    auto built_little = Framer::length_prefixed(little);
    AURORA_TEST_REQUIRE(built_little.ok());
    Framer small = std::move(built_little.value());
    Recorder one;
    AURORA_TEST_CHECK_EQ(must_feed(small,
                                   "\x03"
                                   "abc",
                                   one.sink())
                             .delivered,
                         std::size_t{1});
    AURORA_TEST_CHECK_EQ(one.frames[0], std::string("abc"));
}

// 声明值可含长度字段自身：含时按整帧长度记账；声明不足自身宽度即非法。
AURORA_TEST_CASE(length_prefix_may_count_its_own_width) {
    FrameOptions incl = limits(64, 4096);
    incl.length_field_bytes = 2;
    incl.length_counts_prefix = true;
    auto built = Framer::length_prefixed(incl);
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    // 声明 4 = 2 字节前缀 + 2 字节帧体。
    AURORA_TEST_CHECK_EQ(must_feed(framer,
                                   "\x00\x04"
                                   "ab",
                                   rec.sink())
                             .delivered,
                         std::size_t{1});
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("ab"));

    auto other = Framer::length_prefixed(incl);
    AURORA_TEST_REQUIRE(other.ok());
    Framer bogus = std::move(other.value());
    Recorder none;
    const auto failed = bogus.feed(std::span<const char>("\x00\x01x", 3), none.sink());
    AURORA_TEST_REQUIRE_FALSE(failed.ok());
    AURORA_TEST_CHECK_TRUE(is_code(failed.error(), "net-framer-overflow"));
}

// 声明值超出单帧上限即拒收：长度字段是承诺，超承诺不值得等它兑现。
AURORA_TEST_CASE(length_prefix_over_limit_is_overflow) {
    auto built = Framer::length_prefixed(limits(16, 4096));
    AURORA_TEST_REQUIRE(built.ok());
    Framer framer = std::move(built.value());

    Recorder rec;
    const auto fed = framer.feed(std::span<const char>("\x00\x00\x00\xff", 4), rec.sink());
    AURORA_TEST_REQUIRE_FALSE(fed.ok());
    AURORA_TEST_CHECK_TRUE(is_code(fed.error(), "net-framer-overflow"));
}

// finish() 的型别分歧：行 / 分隔符把尾巴补成最后一帧并标注来源，长度前缀丢弃未兑现的残片。
AURORA_TEST_CASE(finish_delivers_tail_or_drops_remainder) {
    auto line_built = Framer::lines();
    AURORA_TEST_REQUIRE(line_built.ok());
    Framer framer = std::move(line_built.value());

    Recorder rec;
    AURORA_TEST_CHECK_EQ(must_feed(framer, "tail-without-terminator", rec.sink()).delivered, std::size_t{0});
    const auto done = framer.finish(rec.sink());
    AURORA_TEST_REQUIRE(done.ok());
    AURORA_TEST_CHECK_EQ(done.value().delivered, std::size_t{1});
    AURORA_TEST_CHECK_TRUE(rec.unterminated[0]);
    AURORA_TEST_CHECK_EQ(rec.frames[0], std::string("tail-without-terminator"));
    AURORA_TEST_CHECK_EQ(framer.buffered_bytes(), std::size_t{0});

    // 尾巴已收口：再来一次 finish 不补帧，也不报错。
    Recorder again;
    AURORA_TEST_CHECK_TRUE(framer.finish(again.sink()).ok());
    AURORA_TEST_CHECK_EQ(again.frames.size(), std::size_t{0});

    FrameOptions wide = limits(64, 4096);
    wide.length_field_bytes = 4;
    auto prefix_built = Framer::length_prefixed(wide);
    AURORA_TEST_REQUIRE(prefix_built.ok());
    Framer truncated = std::move(prefix_built.value());
    Recorder silent;
    AURORA_TEST_CHECK_EQ(must_feed(truncated, "\x00\x00\x00\x10only-eight", silent.sink()).delivered, std::size_t{0});
    const auto dropped = truncated.finish(silent.sink());
    AURORA_TEST_REQUIRE(dropped.ok());
    AURORA_TEST_CHECK_EQ(dropped.value().delivered, std::size_t{0});
    AURORA_TEST_CHECK_EQ(dropped.value().dropped_bytes, std::size_t{14});  // 4 字节长度字段 + 10 字节残体
    AURORA_TEST_CHECK_EQ(truncated.buffered_bytes(), std::size_t{0});
}

// 工厂口径校验：上限为 0、缓冲小于一帧、空分隔符、分隔符超上限、长度字段宽度越界一律当场拒绝。
AURORA_TEST_CASE(factories_reject_invalid_limits) {
    AURORA_TEST_CHECK_TRUE(is_code(Framer::lines(limits(0, 4096)).error(), "net-framer-config-invalid"));
    AURORA_TEST_CHECK_TRUE(is_code(Framer::lines(limits(64, 0)).error(), "net-framer-config-invalid"));
    AURORA_TEST_CHECK_TRUE(is_code(Framer::lines(limits(64, 32)).error(), "net-framer-config-invalid"));
    AURORA_TEST_CHECK_TRUE(is_code(Framer::until("").error(), "net-framer-config-invalid"));
    AURORA_TEST_CHECK_TRUE(is_code(Framer::until("0123456789", limits(8, 4096)).error(), "net-framer-config-invalid"));

    FrameOptions narrow = limits(64, 4096);
    narrow.length_field_bytes = 0;
    AURORA_TEST_CHECK_TRUE(is_code(Framer::length_prefixed(narrow).error(), "net-framer-config-invalid"));
    narrow.length_field_bytes = 9;
    AURORA_TEST_CHECK_TRUE(is_code(Framer::length_prefixed(narrow).error(), "net-framer-config-invalid"));

    // 8 字节是本型长度字段的上界，合法。
    FrameOptions widest = limits(64, 4096);
    widest.length_field_bytes = 8;
    AURORA_TEST_CHECK_TRUE(Framer::length_prefixed(widest).ok());
}

// 移动语义：状态随对象一并移交，落点上的调用是惰性成功而不是崩溃或悬挂。
AURORA_TEST_CASE(moved_from_framer_is_inert) {
    auto built = Framer::lines();
    AURORA_TEST_REQUIRE(built.ok());
    Framer source = std::move(built.value());

    Recorder refuses{.reject_from = 1};
    AURORA_TEST_CHECK_EQ(must_feed(source, "keep-me\nnext\n", refuses.sink()).delivered, std::size_t{1});

    Framer target = std::move(source);
    AURORA_TEST_CHECK_EQ(target.buffered_bytes(), std::size_t{5});  // 未交付的 "next\n" 随状态一并移交
    Recorder drained;
    AURORA_TEST_CHECK_EQ(must_feed(target, "", drained.sink()).delivered, std::size_t{1});
    AURORA_TEST_CHECK_EQ(drained.frames[0], std::string("next"));

    // 本用例的判据就是「被移动后的落点仍然可调且惰性」，故刻意在被移动对象上调用；use-after-move 的
    // 常规告诫在此正是被测行为，逐条豁免而不是改写用例。
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    Recorder untouched;
    const auto inert = source.feed(std::span<const char>("more\n", 5), untouched.sink());
    AURORA_TEST_REQUIRE(inert.ok());
    AURORA_TEST_CHECK_EQ(inert.value().delivered, std::size_t{0});
    AURORA_TEST_CHECK_EQ(source.buffered_bytes(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(source.finish(untouched.sink()).ok());
    AURORA_TEST_CHECK_EQ(untouched.frames.size(), std::size_t{0});
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

}  // namespace aurora::test_cases::utest_net_framer
