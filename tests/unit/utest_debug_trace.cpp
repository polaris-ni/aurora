/// 测试类型: unit
/// 目标单元: include/aurora/debug/debug_trace.h
/// 测试说明: why_trace 脏标记热路径埋点（DirtyKind 枚举语义、debug 构建下 record_dirty 可调用不崩溃）单元测试

#include <cstdint>
#include <type_traits>

#include "aurora/debug/debug_trace.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_debug_trace {

using debug::detail::record_dirty;
using debug::DirtyKind;

// 枚举底层类型与非负可表示性属编译期契约。
static_assert(std::is_enum_v<DirtyKind>);
static_assert(static_cast<int>(DirtyKind::Layout) != static_cast<int>(DirtyKind::Paint));

AURORA_TEST() {
    // ---- 1. 两种脏标记互异且以 uint8_t 承载 ----
    AURORA_TEST_CHECK(static_cast<std::uint8_t>(DirtyKind::Layout) != static_cast<std::uint8_t>(DirtyKind::Paint));
    static_assert(std::is_same_v<std::underlying_type_t<DirtyKind>, std::uint8_t>);

#ifdef AURORA_ENABLE_DEBUG
    // ---- 2. debug 构建：record_dirty 有定义，调用不崩溃（写入 why_trace 缓冲） ----
    record_dirty(DirtyKind::Layout, "TestWidget", 1ULL, false);
    record_dirty(DirtyKind::Paint, "TestWidget", 1ULL, true);
    AURORA_TEST_CHECK(true);
#else
    // Release：机制整体编译掉，本 TU 不 ODR-use record_dirty（无定义需求）。
    AURORA_TEST_CHECK(true);
#endif
}

}  // namespace aurora::test_cases::utest_debug_trace
