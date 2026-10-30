/// 测试类型: unit
/// 目标单元: include/aurora/widget/skeleton.h
/// 测试说明: 覆盖 Skeleton——size_hint 与占满/回退布局语义、tick 驱动 shimmer 相位推进与回绕、
/// duration 覆写与非正容错、属性序列化往返与 describe 元数据、shimmer 相位经单一 dt 漏斗的应用内
/// 全局时间缩放（倍率折半 / 冻结相位不动且不请求重绘）

#include <chrono>
#include <memory>
#include <string>

#include "aurora/animation/animator.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/skeleton.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_skeleton {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const Skeleton s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Skeleton");
    const Size hint = s.size_hint();
    AURORA_TEST_CHECK_NEAR(hint.width, 0.0F, 1e-4F);  // 0 = 占满约束宽度
    AURORA_TEST_CHECK_NEAR(hint.height, 16.0F, 1e-4F);

    Json props = Json::object();
    s.serialize_props(props);
    // 默认底色/高亮色未显式设置：不落盘（跟随主题角色，迁移批）。
    AURORA_TEST_CHECK_FALSE(props.contains("color"));
    AURORA_TEST_CHECK_FALSE(props.contains("highlight"));
    AURORA_TEST_CHECK_NEAR(require_field<double>(props, "duration"), 1.5, 1e-4);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "height"), 16.0F, 1e-4F);
}

AURORA_TEST_CASE(zero_width_fills_constraint) {
    Skeleton s;
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);  // 宽 0 → 占满约束
    AURORA_TEST_CHECK_NEAR(s.size().height, 16.0F, 1e-4F);  // 默认高 16
}

AURORA_TEST_CASE(explicit_size_used_when_positive) {
    Skeleton s{Size{.width = 120.0F, .height = 20.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 20.0F, 1e-4F);

    // set_size 只更新占位尺寸本身、不标布局脏；AURORA_ENABLE_LAYOUT_CACHE 下约束不变时
    // Widget::layout 直接命中缓存返回旧尺寸，须经公开脏标记 mark_needs_layout() 才会真正重排。
    s.set_size(Size{.width = 50.0F, .height = 24.0F});
    s.mark_needs_layout();
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 24.0F, 1e-4F);
}

AURORA_TEST_CASE(zero_height_falls_back_to_16) {
    Skeleton s{Size{.width = 60.0F, .height = 0.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 16.0F, 1e-4F);  // 高 0 → 回退 16
}

AURORA_TEST_CASE(tick_advances_phase_and_wraps) {
    Skeleton s{Size{.width = 120.0F, .height = 16.0F}};  // 显式尺寸构造才开启 gesture-tick 驱动
    // 首帧只登记 last_tick_ 锚点（尚未建立时步长为 0），此后每帧以相邻两 now 之差累计 elapsed_，
    // 相位经 scaled_frame_dt 折算得出（不再直接取墙钟差 / 周期）。固定偏移 1ms 的起点只作假帧钟原点，
    // epoch 已非哨兵——起点取何值都不改变 elapsed 由逐帧差累计的口径，偏移仅为保持确定性。
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);

    s.tick(t0);  // 首帧登记起点，相位仍 0
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.0, 1e-4);

    s.tick(t0 + std::chrono::milliseconds(750));  // 0.75s / 1.5s = 0.5
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.5, 1e-4);

    s.tick(t0 + std::chrono::milliseconds(3000));  // 3.0s / 1.5s = 2 圈 → 回绕 0
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.0, 1e-4);
    AURORA_TEST_CHECK_TRUE(s.phase() >= 0.0 && s.phase() < 1.0);
}

AURORA_TEST_CASE(set_duration_overrides_period) {
    // 同上：首帧登记 last_tick_ 锚点（步长 0），此后按相邻帧差累计相位；起点取固定假帧钟原点即可，
    // epoch 已非哨兵，不再有「每次 tick 重置起点、相位滞后一拍」的旧口径。
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);

    Skeleton custom{Size{.width = 120.0F, .height = 16.0F}};
    custom.set_duration(0.75);
    custom.tick(t0);
    custom.tick(t0 + std::chrono::milliseconds(375));  // 0.375s / 0.75s = 0.5
    AURORA_TEST_CHECK_NEAR(custom.phase(), 0.5, 1e-4);

    Skeleton fallback{Size{.width = 120.0F, .height = 16.0F}};
    fallback.set_duration(-1.0);  // 非正 → 回落默认 1.5s
    fallback.tick(t0);
    fallback.tick(t0 + std::chrono::milliseconds(750));
    AURORA_TEST_CHECK_NEAR(fallback.phase(), 0.5, 1e-4);

    Json props = Json::object();
    fallback.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<double>(props, "duration"), 1.5, 1e-4);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Skeleton src{Size{.width = 80.0F, .height = 22.0F}};
    src.set_color(Color(1, 2, 3, 4)).set_highlight(Color(5, 6, 7, 8)).set_duration(2.5);

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "width"), 80.0F, 1e-4F);

    Skeleton dst;
    dst.deserialize_props(props);
    Json out = Json::object();
    dst.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "width"), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "height"), 22.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "color"), 1)->as_or<int>(-1), 2);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "highlight"), 3)->as_or<int>(-1), 8);
    AURORA_TEST_CHECK_NEAR(require_field<double>(out, "duration"), 2.5, 1e-4);

    // 反序列化后的占位尺寸直接驱动布局。
    LayoutEngine::layout(dst, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 22.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Skeleton::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Skeleton");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_duration = false;
    bool has_highlight = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "duration") {
            has_duration = true;
        }
        if (std::string{p.name} == "highlight") {
            has_highlight = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_duration);
    AURORA_TEST_CHECK_TRUE(has_highlight);
}

// ---- 主题跟随（迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

AURORA_TEST_CASE(skeleton_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light
    Skeleton s;
    AURORA_TEST_CHECK(s.resolve_base(bare) == Color(226, 226, 226));  // light surface_variant
    AURORA_TEST_CHECK(s.resolve_highlight(bare) == Color(255, 255, 255));  // light surface_container_lowest
    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(s.resolve_base(dark) == Color(71, 71, 71));  // dark surface_variant
    AURORA_TEST_CHECK(s.resolve_highlight(dark) == Color(14, 14, 14));  // dark surface_container_lowest
}

AURORA_TEST_CASE(skeleton_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    Skeleton s;
    s.set_color(Color(1, 2, 3, 4));
    s.set_highlight(Color(5, 6, 7, 8));
    AURORA_TEST_CHECK(s.resolve_base(dark) == Color(1, 2, 3, 4));
    AURORA_TEST_CHECK(s.resolve_highlight(dark) == Color(5, 6, 7, 8));
}

// ---- 应用内全局时间缩放：Skeleton shimmer 相位经单一 dt 漏斗 ----

// @brief 判据 10 覆盖矩阵相位行：Skeleton 相位累加吃折算后的 dt，time_scale=2.0 达到同一相位所需
//        帧数为 1.0 的一半（默认周期 1.5s，每帧裸步长 0.1s：越过相位 0.5 需 elapsed≥0.75）。
AURORA_TEST_CASE(skeleton_shimmer_phase_frames_halve_with_time_scale) {
    const auto fake_t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);
    // 返回从锚点后推进到「相位首次 >= 0.5」所需的帧数（不含登记锚点的首帧）。
    auto frames_to_phase = [&](double scale) -> int {
        aurora::Animator anim;
        anim.set_time_scale(scale);
        const aurora::testing::ScopedCurrentAnimator scope{anim};
        Skeleton s{Size{.width = 120.0F, .height = 16.0F}};  // 缺省周期 1.5s
        s.tick(fake_t0);  // 首帧登记 last_tick_，步长 0，相位不动
        auto now = fake_t0;
        int frames = 0;
        while (s.phase() < 0.5 && frames < 200) {
            now += std::chrono::milliseconds(100);  // 裸帧间隔 0.1s，折算后为 0.1×scale
            s.tick(now);
            ++frames;
        }
        return frames;
    };

    const int f_1x = frames_to_phase(1.0);  // 0.1×1×8 = 0.8 ≥ 0.75 → 8 帧
    const int f_2x = frames_to_phase(2.0);  // 0.1×2×4 = 0.8 ≥ 0.75 → 4 帧
    AURORA_TEST_REQUIRE_GT(f_1x, 0);
    AURORA_TEST_CHECK_EQ(f_1x, 2 * f_2x);  // 帧数恰半：出现 1/4 即双层缩放，出现等量即漏缩放
}

// @brief 判据 11(b) 冻结行：time_scale=0 时相位不前进、on_dirty 重绘请求计数为 0；恢复后相位续走。
AURORA_TEST_CASE(skeleton_freeze_holds_phase_and_requests_no_frames) {
    const auto fake_t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);
    aurora::Animator anim;
    anim.set_time_scale(1.0);
    const aurora::testing::ScopedCurrentAnimator scope{anim};

    Skeleton s{Size{.width = 120.0F, .height = 16.0F}};
    s.tick(fake_t0);
    s.tick(fake_t0 + std::chrono::milliseconds(100));  // 推进一帧，相位已非零
    const double frozen_phase = s.phase();
    AURORA_TEST_CHECK_GT(frozen_phase, 0.0);

    anim.set_time_scale(0.0);  // 冻结：折算步长归零
    int dirty_count = 0;
    s.on_dirty = [&dirty_count](bool /*layout*/) -> void { ++dirty_count; };
    auto now = fake_t0 + std::chrono::milliseconds(100);
    for (int i = 0; i < 5; ++i) {
        now += std::chrono::milliseconds(100);
        s.tick(now);
    }
    AURORA_TEST_CHECK_NEAR(s.phase(), frozen_phase, 0.0);  // 相位停在冻结前
    AURORA_TEST_CHECK_EQ(dirty_count, 0);  // dt 归零不标脏、不请求帧

    anim.set_time_scale(1.0);  // 解冻续跑：相位重新前进（冻结非终态）
    now += std::chrono::milliseconds(100);
    s.tick(now);
    AURORA_TEST_CHECK_GT(s.phase(), frozen_phase);  // 解冻后相位越过冻结前的值
}

}  // namespace aurora::test_cases::utest_skeleton
