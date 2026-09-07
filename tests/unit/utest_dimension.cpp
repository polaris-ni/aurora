/// 测试类型: unit
/// 目标单元: include/aurora/core/dimension.h
/// 测试说明: 强类型尺寸意图工厂（px/dp/percent/fill/auto_length）、to_string 快照与长度字面量单元测试

#include <string>

#include "aurora/core/dimension.h"
#include "aurora/core/types.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_dimension {

// 编译期契约：工厂返回的意图种类必须可在常量表达式中确定。
static_assert(px(1.0F).kind == LengthKind::Fixed);
static_assert(dp(1.0F).kind == LengthKind::Fixed);
static_assert(percent(0.5F).kind == LengthKind::Fraction);
static_assert(fill().kind == LengthKind::Expand);
static_assert(auto_length().kind == LengthKind::WrapContent);

AURORA_TEST() {
    // ---- 1. px / dp 生成 Fixed 意图，数值原样透传 ----
    {
        const auto w = px(120.0F);
        AURORA_TEST_CHECK(w.kind == LengthKind::Fixed);
        AURORA_TEST_CHECK(w.value == 120.0F);

        const auto h = dp(48.0F);
        AURORA_TEST_CHECK(h.kind == LengthKind::Fixed);
        AURORA_TEST_CHECK(h.value == 48.0F);
    }

    // ---- 2. dp 当前等价于逻辑像素（与 dp(v) 语义一致的 px） ----
    {
        AURORA_TEST_CHECK(dp(16.0F).value == px(16.0F).value);
        AURORA_TEST_CHECK(dp(16.0F).kind == px(16.0F).kind);
    }

    // ---- 3. percent 生成 Fraction 意图（0~1 比例） ----
    {
        const auto p = percent(0.8F);
        AURORA_TEST_CHECK(p.kind == LengthKind::Fraction);
        AURORA_TEST_CHECK(p.value == 0.8F);
    }

    // ---- 4. fill / auto_length 生成无数值语义的意图 ----
    {
        const auto f = fill();
        AURORA_TEST_CHECK(f.kind == LengthKind::Expand);
        AURORA_TEST_CHECK(f.value == 0.0F);

        const auto a = auto_length();
        AURORA_TEST_CHECK(a.kind == LengthKind::WrapContent);
        AURORA_TEST_CHECK(a.value == 0.0F);
    }

    // ---- 5. 默认 Length 即 WrapContent（与 auto_length 一致） ----
    {
        const Length d{};
        AURORA_TEST_CHECK(d.kind == LengthKind::WrapContent);
        AURORA_TEST_CHECK(d.kind == auto_length().kind);
    }

    // ---- 6. to_string 覆盖四种意图的可读快照 ----
    {
        AURORA_TEST_CHECK(to_string(auto_length()) == "auto");
        AURORA_TEST_CHECK(to_string(fill()) == "fill");

        const std::string px_s = to_string(px(120.0F));
        AURORA_TEST_CHECK(px_s.rfind("px(", 0) == 0);
        AURORA_TEST_CHECK(px_s.find("120") != std::string::npos);

        const std::string pct_s = to_string(percent(0.5F));
        AURORA_TEST_CHECK(pct_s.rfind("percent(", 0) == 0);
        AURORA_TEST_CHECK(pct_s.find("0.5") != std::string::npos);
    }

    // ---- 7. 长度字面量仅在 TU 内 using 后可用，语义与工厂一致 ----
    {
        using namespace aurora::literals;  // NOLINT(build/namespaces_literals) —— 测试点即字面量
        const auto a = 120_dp;
        AURORA_TEST_CHECK(a.kind == LengthKind::Fixed);
        AURORA_TEST_CHECK(a.value == 120.0F);

        const auto b = 8_px;
        AURORA_TEST_CHECK(b.kind == LengthKind::Fixed);
        AURORA_TEST_CHECK(b.value == 8.0F);

        // 无符号整型重载（120ULL 走 unsigned long long 分支）
        const auto c = 240_dp;
        AURORA_TEST_CHECK(c.value == 240.0F);
    }

    // ---- 8. 不同意图互不相等（kind 参与比较） ----
    {
        AURORA_TEST_CHECK(px(0.0F).kind != fill().kind);
        AURORA_TEST_CHECK(percent(1.0F).kind != px(1.0F).kind);
    }
}

}  // namespace aurora::test_cases::utest_dimension
