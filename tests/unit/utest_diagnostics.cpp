/// 测试类型: unit
/// 目标单元: include/aurora/core/diagnostics.h
/// 测试说明: 诊断收集器的累计/取清/计数、slug→表驱动元数据映射、无码退化与降级 Error 严重级、strict_mode 关闭下的
/// degraded 记录、explain 兜底、修复建议收集与应用、注册表与 auto_fix_all、to_json_line 形态、recent 环形缓冲

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/log.h"
#include "aurora/core/strict_mode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_diagnostics {

namespace m = aurora::testing::matchers;

namespace {
/// @brief RAII：用例期间吞掉 Diagnostics 桥接的 Logger 输出，析构时恢复级别/开关/默认 sink，
/// 避免诊断测试向 stderr 泄漏日志或把 Logger 全局状态泄漏给其他用例。
class QuietLogger {
  public:
    QuietLogger() : level_{aurora::Logger::instance().level()}, enabled_{aurora::Logger::instance().is_enabled()} {
        aurora::Logger::instance().set_sink([](std::string_view) -> void { /* 丢弃诊断桥接日志 */ });
    }
    ~QuietLogger() {
        aurora::Logger::instance().set_level(level_);
        aurora::Logger::instance().set_enabled(enabled_);
        aurora::Logger::instance().set_sink(nullptr);  // 恢复默认 stderr
    }
    QuietLogger(const QuietLogger&) = delete;
    auto operator=(const QuietLogger&) -> QuietLogger& = delete;
    QuietLogger(QuietLogger&&) = delete;
    auto operator=(QuietLogger&&) -> QuietLogger& = delete;

  private:
    aurora::LogLevel level_;
    bool enabled_;
};
}  // namespace

/// @brief 修复回调命中计数（静态存储期）：Diagnostics 修复注册表为进程级全局且无注销接口，
/// 回调若捕获栈局部变量，用例结束后被后续用例的 auto_fix_all / apply_fix 执行即为 UB
/// （悬垂写栈地址可能被新用例复用）。全文件修复回调一律命中文件级静态槽位计数器，
/// 各用例只对自己槽位做增量断言。
static auto fix_hits(std::size_t slot) -> std::size_t& {
    static std::array<std::size_t, 3> hits{};
    return hits.at(slot);
}

AURORA_TEST_CASE(report_accumulates_take_clears_and_counts) {
    const QuietLogger quiet;
    (void)Diagnostics::take();  // 清场基线：清空累计诊断
    AURORA_TEST_REQUIRE_EQ(Diagnostics::count(), std::size_t{0});

    Diagnostics::warn("first warning");
    Diagnostics::warn("second warning", "where-x");
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), std::size_t{2});

    const auto taken = Diagnostics::take();
    AURORA_TEST_CHECK_EQ(taken.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), std::size_t{0});  // take 后清空
    AURORA_TEST_CHECK_STREQ(taken[0].message, "first warning");
    AURORA_TEST_CHECK_STREQ(taken[1].where, "where-x");
}

AURORA_TEST_CASE(report_maps_frozen_slug_to_table_metadata) {
    const QuietLogger quiet;
    (void)Diagnostics::take();

    // code 为冻结 slug 时：severity/category/code_enum 由 errors.toml 表驱动注入。
    Diagnostics::warn("depth problem", "widget-x", "nav-depth-exceeded");
    const auto taken = Diagnostics::take();
    AURORA_TEST_REQUIRE_EQ(taken.size(), std::size_t{1});
    const auto& d = taken[0];
    AURORA_TEST_CHECK_STREQ(d.code, "nav-depth-exceeded");
    AURORA_TEST_CHECK(d.code_enum == ErrorCode::NavDepthExceeded);
    AURORA_TEST_CHECK(d.severity == ErrorSeverity::Error);
    AURORA_TEST_CHECK(d.category == ErrorCategory::Navigation);
    AURORA_TEST_CHECK_STREQ(d.severity_str(), "error");
    AURORA_TEST_CHECK_STREQ(d.category_str(), "navigation");
}

AURORA_TEST_CASE(report_without_code_falls_back_to_general_severity) {
    const QuietLogger quiet;
    (void)Diagnostics::take();

    // 无 code：退化为 Warning / General / GeneralUnknown。
    Diagnostics::warn("no code attached");
    auto taken = Diagnostics::take();
    AURORA_TEST_REQUIRE_EQ(taken.size(), std::size_t{1});
    AURORA_TEST_CHECK(taken[0].code.empty());
    AURORA_TEST_CHECK(taken[0].code_enum == ErrorCode::GeneralUnknown);
    AURORA_TEST_CHECK(taken[0].severity == ErrorSeverity::Warning);
    AURORA_TEST_CHECK(taken[0].category == ErrorCategory::General);

    // report(is_degraded=true) 且无 slug：降级路径升为 Error。
    Diagnostics::report("silent degrade", "", "", true);
    taken = Diagnostics::take();
    AURORA_TEST_REQUIRE_EQ(taken.size(), std::size_t{1});
    AURORA_TEST_CHECK(taken[0].severity == ErrorSeverity::Error);
}

AURORA_TEST_CASE(degraded_respects_table_slug_and_strict_mode_off) {
    const QuietLogger quiet;
    // strict_mode 为线程级全局态：先读原值，Off 下验证记录行为，用后恢复（On 时 degraded 会硬失败）。
    const auto original_strict = strict_mode();
    set_strict_mode(StrictMode::Off);
    (void)Diagnostics::take();

    // 表内 slug 元数据优先于 is_degraded：render-degraded 的表严重级为 Warning。
    Diagnostics::degraded("font fallback", "text-widget", "render-degraded");
    auto taken = Diagnostics::take();
    AURORA_TEST_REQUIRE_EQ(taken.size(), std::size_t{1});
    AURORA_TEST_CHECK(taken[0].severity == ErrorSeverity::Warning);
    AURORA_TEST_CHECK(taken[0].code_enum == ErrorCode::RenderDegraded);

    // 无 slug 的 degraded：is_degraded=true → Error。
    Diagnostics::degraded("unknown degrade", "", "");
    taken = Diagnostics::take();
    AURORA_TEST_REQUIRE_EQ(taken.size(), std::size_t{1});
    AURORA_TEST_CHECK(taken[0].severity == ErrorSeverity::Error);

    set_strict_mode(original_strict);
}

AURORA_TEST_CASE(explain_diagnostic_slug_enum_and_unknown_fallback) {
    // slug 重载：解释文本取自表内 hint（单一声明源）。
    const auto by_slug = Diagnostics::explain_diagnostic("nav-depth-exceeded");
    AURORA_TEST_CHECK_EQ(by_slug, std::string{aurora::hint_of(ErrorCode::NavDepthExceeded)});
    AURORA_TEST_CHECK_EQ(Diagnostics::explain_diagnostic(ErrorCode::NavDepthExceeded), by_slug);

    // 未知码：返回兜底说明。
    const auto unknown = Diagnostics::explain_diagnostic("no-such-slug");
    AURORA_TEST_CHECK_THAT(unknown, m::has_substr("Unknown diagnostic code"));
}

AURORA_TEST_CASE(fix_suggestion_collected_and_applied_by_slug) {
    const QuietLogger quiet;
    (void)Diagnostics::take();

    constexpr std::size_t slot = 0;
    const FixSuggestion fix{.code = "utest-fix-depth", .description = "reduce nesting depth", .auto_fix = []() -> void {
                                ++fix_hits(slot);
                            }};
    Diagnostics::report("tree too deep", "root", "nav-depth-exceeded", false, fix);

    // collect_fixes 读取 recent_（take 不清空），可能含其他用例残留：按本用例唯一 code 过滤断言。
    const auto fixes = Diagnostics::collect_fixes();
    bool found = false;
    for (const auto& f : fixes) {
        if (f.code == "utest-fix-depth") {
            found = true;
            AURORA_TEST_CHECK_STREQ(f.description, "reduce nesting depth");
            AURORA_TEST_CHECK_TRUE(f.has_auto_fix());
        }
    }
    AURORA_TEST_CHECK_TRUE(found);

    // 按 slug 应用一次自动修复（槽位增量，免疫注册表残留）；未知码返回 false。
    const auto hits_before = fix_hits(slot);
    AURORA_TEST_CHECK_EQ(Diagnostics::apply_fix("utest-fix-depth"), true);
    AURORA_TEST_CHECK_EQ(fix_hits(slot) - hits_before, std::size_t{1});
    AURORA_TEST_CHECK_EQ(Diagnostics::apply_fix("no-such-fix-code"), false);
    (void)Diagnostics::take();
}

AURORA_TEST_CASE(register_fix_applies_by_error_code) {
    const QuietLogger quiet;
    // 注册表全局共享：使用本文件内唯一的错误码，避免与其他用例的注册项串扰；
    // 回调命中静态槽位（栈局部变量会在用例结束后悬垂）。
    constexpr std::size_t slot = 1;
    Diagnostics::register_fix(
        ErrorCode::PrefsWriteFailed,
        FixSuggestion{.code = "", .description = "", .auto_fix = []() -> void { ++fix_hits(slot); }});
    const auto hits_before = fix_hits(slot);
    AURORA_TEST_CHECK_EQ(Diagnostics::apply_fix(ErrorCode::PrefsWriteFailed), true);
    AURORA_TEST_CHECK_EQ(fix_hits(slot) - hits_before, std::size_t{1});
    // 未注册码：未命中返回 false。
    AURORA_TEST_CHECK_EQ(Diagnostics::apply_fix(ErrorCode::StorageIoError), false);
}

AURORA_TEST_CASE(auto_fix_all_executes_only_auto_fixable_entries) {
    const QuietLogger quiet;
    // 本用例唯一错误码 + 静态槽位：auto_fix_all 每次调用执行注册表中全部带回调条目
    // （含前序用例残留），故不能对返回值做绝对断言，只能要求两次调用间注册表不变
    // （返回值相等）且本用例回调每次调用恰好执行一次。
    constexpr std::size_t slot = 2;
    Diagnostics::register_fix(
        ErrorCode::StorageEncodingMismatch,
        FixSuggestion{.code = "", .description = "", .auto_fix = []() -> void { ++fix_hits(slot); }});
    // 无 auto_fix 回调的注册项不参与执行：按码 apply 也未命中可执行回调。
    Diagnostics::register_fix(ErrorCode::FontMissing,
                              FixSuggestion{.code = "font-hint", .description = "register the font", .auto_fix = {}});

    const auto hits_before = fix_hits(slot);
    const auto before = Diagnostics::auto_fix_all();
    const auto after = Diagnostics::auto_fix_all();
    AURORA_TEST_CHECK_EQ(after, before);  // 两次调用间注册表未变化
    AURORA_TEST_CHECK_EQ(fix_hits(slot) - hits_before, std::size_t{2});  // 每次调用各执行一次
    AURORA_TEST_CHECK_EQ(Diagnostics::apply_fix(ErrorCode::FontMissing), false);  // 无回调不可执行
}

AURORA_TEST_CASE(diagnostic_to_json_line_shape) {
    Diagnostic d;
    d.severity = ErrorSeverity::Warning;
    d.category = ErrorCategory::Widget;
    d.message = "unknown type";
    d.where = "widget-y";
    d.code = "widget-unknown-type";
    d.code_enum = ErrorCode::WidgetUnknownType;
    const auto line = d.to_json_line();
    AURORA_TEST_CHECK_THAT(line, m::has_substr(R"("severity":"warning")"));
    AURORA_TEST_CHECK_THAT(line, m::has_substr(R"("category":"widget")"));
    AURORA_TEST_CHECK_THAT(line, m::has_substr(R"("message":"unknown type")"));
    AURORA_TEST_CHECK_THAT(line, m::has_substr(R"("where":"widget-y")"));
    AURORA_TEST_CHECK_THAT(line, m::has_substr(R"("code":"widget-unknown-type")"));

    // fix 携带 auto_fix 回调时输出 fix_code/fix_desc。
    d.fix = FixSuggestion{.code = "fix-widget-type", .description = "register type", .auto_fix = []() -> void {}};
    const auto with_fix = d.to_json_line();
    AURORA_TEST_CHECK_THAT(with_fix, m::has_substr(R"("fix_code":"fix-widget-type")"));

    // 无 auto_fix 回调的 fix 不写入 JSON 行。
    d.fix = FixSuggestion{.code = "fix-hint-only", .description = "manual fix", .auto_fix = {}};
    const auto without_auto = d.to_json_line();
    AURORA_TEST_CHECK_THAT(without_auto, m::negated(m::has_substr("fix_code")));
}

AURORA_TEST_CASE(get_last_diagnostics_ring_buffer_keeps_recent) {
    const QuietLogger quiet;
    (void)Diagnostics::take();
    for (std::size_t i = 0; i < Diagnostics::AURORA_RECENT_CAP + 2; ++i) {
        Diagnostics::warn("ring-" + std::to_string(i));
    }
    // 环形缓冲只保留最近 AURORA_RECENT_CAP 条，最末为最新一条。
    const auto recent = Diagnostics::get_last_diagnostics();
    AURORA_TEST_CHECK_EQ(recent.size(), Diagnostics::AURORA_RECENT_CAP);
    AURORA_TEST_CHECK_STREQ(recent.back().message, "ring-" + std::to_string(Diagnostics::AURORA_RECENT_CAP + 1));
    // take 只清空累计队列（未封顶），不影响 recent 环形缓冲。
    AURORA_TEST_CHECK_EQ(Diagnostics::take().size(), Diagnostics::AURORA_RECENT_CAP + 2);
}

}  // namespace aurora::test_cases::utest_diagnostics
