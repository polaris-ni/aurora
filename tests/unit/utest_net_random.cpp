/// 测试类型: unit
/// 目标单元: include/aurora/net/random.h + src/aurora/net/random.cpp
/// 测试说明: 系统熵源公共面的取值契约回归（`RandomSource::fill` 的四条形态约束里可在单测陈述的那三条）。
///           熵本身不可断言——它是真随机，任何逐值断言都会在下一枚取值上失效——故这里只断言**填满的形态**：
///           ① 空请求成功且一个字节都不动（「调用方没要东西」与「熵源没给东西」是两件事，把空段判成失败
///           会让所有以零长缓冲探路的写法在每条腿上误报）；② **每一枚字节都被写到**：缓冲先铺一字节哨兵，
///           填完后仍等于哨兵的枚数必须停在随机期望（约 `size / 256`）附近而不是停在半程——少填、填错偏移、
///           或整段没进系统调用，都会在这里留下一大段哨兵；③ 逐次取值互不相同（同一枚缓冲填两次不该相等，
///           这条挡住「把缺省源写成常数」那一型退化）；④ 长度跨过两个单调用上限时照样填满——游标步长写成
///           固定值或双倍跨那一型会整段跳过，而只多要一枚字段的请求判不出这种跳过（被跳过的只剩末字节，
///           与随机流里本就该出现的哨兵无从分辨）。
///           `default_random_source()` 交出的是同一枚进程级对象，故它恒不返回空引用——这一处也钉住，因为
///           「每次新分配一个源」的写法看着无害，却会把单例承诺变成每次调用的构造开销。
///           取不到熵那条分支不在这里判：本用例跑到的每条腿都有熵源，失败路径要的是没有 WebCrypto 的环境，
///           它的判据是 `net-platform-unavailable` 而非某个取值。

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "aurora/net/random.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_random {
namespace {

using aurora::net::default_random_source;

/// @brief 铺缓冲用的哨兵字节：随机流里每 256 枚才出现一次，故「没填到」的区间会成段留下。
constexpr std::byte AURORA_SENTINEL{0x5A};

/// @brief 数出缓冲里仍等于哨兵的字节数。
/// @param bytes 待数的缓冲。
/// @return 未被写动的字节数（随机填充下期望约为 `bytes.size() / 256`）。
[[nodiscard]] auto count_unchanged(std::span<const std::byte> bytes) -> std::size_t {
    std::size_t left = 0;
    for (const std::byte unit : bytes) {
        if (unit == AURORA_SENTINEL) {
            ++left;
        }
    }
    return left;
}

/// @brief 铺满哨兵后取一次 `fill`。
/// @param bytes 目标缓冲（就地填充）。
/// @return 无返回值（填充结论由调用方的断言交出）。
auto fill_with_sentinel(std::span<std::byte> bytes) -> void {
    for (std::byte &unit : bytes) {
        unit = AURORA_SENTINEL;
    }
    const auto filled = default_random_source().fill(bytes);
    AURORA_TEST_CHECK(static_cast<bool>(filled));
}

}  // namespace

AURORA_TEST_CASE(an_empty_request_succeeds_and_touches_nothing) {
    std::array<std::byte, 4> buffer{AURORA_SENTINEL, AURORA_SENTINEL, AURORA_SENTINEL, AURORA_SENTINEL};
    const std::span<std::byte> nothing{};  // 调用方没要东西：与「熵源没给东西」是两件事
    const auto filled = default_random_source().fill(nothing);
    AURORA_TEST_CHECK(static_cast<bool>(filled));
    // 零长请求连缓冲都不该碰：这里的断言是「哨兵还在」，写成 `buffer[0] != 0` 那种形式判不出多余写入。
    AURORA_TEST_CHECK_EQ(count_unchanged(buffer), std::size_t{4});
}

AURORA_TEST_CASE(fill_reaches_every_byte_of_a_four_kilobyte_request) {
    constexpr std::size_t request = 4096;
    std::vector<std::byte> buffer(request, AURORA_SENTINEL);
    fill_with_sentinel(buffer);
    // 期望约 16 枚（4096 / 256）；只填前半会留下约 2048 枚，故上限取八分之一（512）既能挡住半程停手，
    // 又远在任何合法随机流的抖动之外（二项分布的标准差约 4 枚）。
    AURORA_TEST_CHECK_LE(count_unchanged(buffer), request / 8U);
}

AURORA_TEST_CASE(a_request_larger_than_two_caps_is_still_filled) {
    // 两整段加一枚（65,536 × 2 + 1）：切段循环的游标若写成固定步长或双倍跨（跳过一整段那一型），中间那 64 KiB
    // 会整段留着哨兵，这里就现形。只多要一枚字段的写法（65,537）判不出这种跳过——被跳过的只剩末字节，
    // 与随机流里本就该出现的哨兵撞车无从分辨，故长度要跨过两个完整段。
    constexpr std::size_t request = 131073;
    std::vector<std::byte> buffer(request, AURORA_SENTINEL);
    fill_with_sentinel(buffer);
    AURORA_TEST_CHECK_LE(count_unchanged(buffer), request / 8U);
}

AURORA_TEST_CASE(successive_fills_of_the_same_length_are_not_the_same_bytes) {
    std::array<std::byte, 32> first{};
    std::array<std::byte, 32> second{};
    fill_with_sentinel(first);
    fill_with_sentinel(second);
    AURORA_TEST_CHECK(first != second);
    // 逐字节全等不同是不成立的期望（32 枚里撞一枚的概率约 12%），故只断言「不是同一串」。
}

AURORA_TEST_CASE(the_default_source_is_one_process_wide_object) {
    auto &first = default_random_source();
    auto &second = default_random_source();
    AURORA_TEST_CHECK_EQ(&first, &second);
    // 引用相等之外还要能各自取值：同一枚对象被两次并发调取是本库的正常用法（多 `Client` 各一条腿）。
    std::array<std::byte, 8> probe{};
    AURORA_TEST_CHECK(static_cast<bool>(first.fill(probe)));
    AURORA_TEST_CHECK(static_cast<bool>(second.fill(probe)));
}

}  // namespace aurora::test_cases::utest_net_random
