#include "aurora/perf/scroll_bench.h"

#include <cmath>
#include <memory>
#include <utility>

#include "aurora/core/string_util.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/perf/stopwatch.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/scroll.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

namespace aurora {

namespace {

/**
 * @brief 垂直滚动控件探针：读取被测树中主滚动容器的真实偏移量。
 *
 * 存在的意义是**自证**：没有它，harness 可能在事件根本没命中滚动容器的情况下，
 * 照样输出一组「非常流畅」的读数（因为每帧都是 idle 跳帧）。有了偏移量对比，
 * 「没滚动」这件事会直接体现在 `moved_frames = 0` 上。
 *
 * 三类控件的偏移访问器命名历史不一致（`Scroll::offset_y` vs
 * `LazyList/GridView::scroll_offset`），此处统一收口，不改动既有公共 API。
 */
class ScrollProbe {
  public:
    /// @brief 前序深度优先查找**最外层**垂直滚动容器（页面级滚动器优先于内部嵌套列表）。
    [[nodiscard]] static auto find(Widget &root) -> ScrollProbe {
        ScrollProbe p;
        (void)search(root, p);
        return p;
    }

    [[nodiscard]] auto valid() const -> bool { return node_ != nullptr; }

    /// @brief 当前滚动偏移（逻辑 dp）；未定位到控件时恒为 0。
    [[nodiscard]] auto offset() const -> float {
        if (scroll_ != nullptr) {
            return scroll_->offset_y();
        }
        if (lazy_ != nullptr) {
            return lazy_->scroll_offset();
        }
        if (grid_ != nullptr) {
            return grid_->scroll_offset();
        }
        return 0.0F;
    }

    /// @brief 滚动容器**自身**的视口高（逻辑 dp）。
    /// @note 与窗口视口高不是一回事：`AppShell` 的顶栏 / 底栏会挤占，实测差出百余 dp。
    ///       判断「内容够不够滚」必须用这个数，用窗口高会得出错误结论。
    [[nodiscard]] auto viewport_h() const -> float { return node_ != nullptr ? node_->size().height : 0.0F; }

  private:
    [[nodiscard]] static auto search(Widget &w, ScrollProbe &out) -> bool {
        if (auto *s = dynamic_cast<Scroll *>(&w)) {
            out.scroll_ = s;
            out.node_ = &w;
            return true;
        }
        if (auto *l = dynamic_cast<LazyList *>(&w)) {
            out.lazy_ = l;
            out.node_ = &w;
            return true;
        }
        if (auto *g = dynamic_cast<GridView *>(&w)) {
            out.grid_ = g;
            out.node_ = &w;
            return true;
        }
        bool found = false;
        w.for_each_child([&found, &out](const Widget &child) -> void {
            if (found) {
                return;  // for_each_child 无早停，命中后余下子树直接掠过
            }
            found = search(const_cast<Widget &>(child), out);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        });
        return found;
    }

    Widget *node_ = nullptr;  ///< 命中的控件本体（取自身尺寸用）
    Scroll *scroll_ = nullptr;
    LazyList *lazy_ = nullptr;
    GridView *grid_ = nullptr;
};

/// @brief 落定退出原因的可读名（报告用）。
[[nodiscard]] auto settle_reason_name(ScrollBenchHarness::Result::SettleReason r) -> const char * {
    using R = ScrollBenchHarness::Result::SettleReason;
    switch (r) {
        case R::Disabled:
            return "disabled";
        case R::Idle:
            return "idle";
        case R::TimeBudget:
            return "time";
        case R::FrameCap:
            return "frame-cap";
    }
    return "?";
}

}  // namespace

// ---------------------------------------------------------------------------
// ScrollBenchHarness::Result
// ---------------------------------------------------------------------------

auto ScrollBenchHarness::Result::geometry_stable() const -> bool {
    return std::fabs(max_offset - max_offset_end) < 0.5F;
}

auto ScrollBenchHarness::Result::content_screens() const -> float {
    if (scroll_viewport_h <= 0.0F) {
        return 0.0F;
    }
    return (scroll_viewport_h + max_offset) / scroll_viewport_h;
}

auto ScrollBenchHarness::Result::reversal_ratio() const -> double {
    if (report.frame_count == 0) {
        return 0.0;
    }
    return static_cast<double>(reversals) / static_cast<double>(report.frame_count);
}

auto ScrollBenchHarness::Result::trustworthy() const -> bool {
    return scrollable_found && settled && report.frame_count > 0 && moved_frames == report.frame_count &&
           idle_frames == 0 && max_offset > 0.5F && geometry_stable() && reversal_ratio() <= kMaxReversalRatio;
}

auto ScrollBenchHarness::Result::to_markdown() const -> std::string {
    std::string out = report.to_markdown();

    out += "\n| 滚动自证 | 值 | 判定 |\n|------|----|----|\n";
    out += aurora::internal::string_format("| scrollable found | %s | %s |\n", scrollable_found ? "yes" : "no",
                                           scrollable_found ? "ok" : "**FAIL**");
    out += aurora::internal::string_format(
        "| moved frames | %zu / %zu | %s |\n", moved_frames, report.frame_count,
        (report.frame_count > 0 && moved_frames == report.frame_count) ? "ok" : "**FAIL**");
    out += aurora::internal::string_format("| idle (skipped) frames | %zu | %s |\n", idle_frames,
                                           idle_frames == 0 ? "ok" : "**FAIL**");
    out += aurora::internal::string_format("| reversals | %zu（%.1f%%） | %s |\n", reversals, reversal_ratio() * 100.0,
                                           reversal_ratio() <= kMaxReversalRatio ? "ok" : "**FAIL 内容太短**");
    out += aurora::internal::string_format(
        "| scrolled | %.1f dp（%.1f dp/帧） | — |\n", scrolled_px,
        report.frame_count > 0 ? scrolled_px / static_cast<double>(report.frame_count) : 0.0);
    out +=
        aurora::internal::string_format("| step calibration | %.2f dp/unit | %s |\n", static_cast<double>(dp_per_unit),
                                        dp_per_unit > 0.0F ? "ok" : "**FAIL 未标定**");
    out += aurora::internal::string_format("| scroll extent | %.1f dp | %s |\n", static_cast<double>(max_offset),
                                           max_offset > 0.5F ? "ok" : "**FAIL 不可滚**");
    out += aurora::internal::string_format("| scroll viewport | %.1f dp（窗口 %.0f dp） | 内容 %.2f 屏%s |\n",
                                           static_cast<double>(scroll_viewport_h), static_cast<double>(viewport.height),
                                           static_cast<double>(content_screens()),
                                           content_screens() < 2.0F ? "（偏短）" : "");
    out += aurora::internal::string_format("| geometry stable | %.1f → %.1f dp | %s |\n",
                                           static_cast<double>(max_offset), static_cast<double>(max_offset_end),
                                           geometry_stable() ? "ok" : "**FAIL 采样期内容仍在变**");
    out += aurora::internal::string_format("| final offset | %.1f dp | — |\n", static_cast<double>(final_offset));
    out += aurora::internal::string_format("| settle | %zu frames / %.0f ms（%s） | %s |\n", settle_frames, settle_ms,
                                           settle_reason_name(settle_reason), settled ? "ok" : "**FAIL 撞帧数上限**");
    out += aurora::internal::string_format("| **trustworthy** | %s | |\n",
                                           trustworthy() ? "**yes**" : "**NO — 读数不可信**");
    return out;
}

auto ScrollBenchHarness::Result::to_json() const -> std::string {
    const std::string prefix = aurora::internal::string_format(
        "{\"scrollable_found\":%s,\"moved_frames\":%zu,\"idle_frames\":%zu,\"reversals\":%zu,"
        "\"reversal_ratio\":%.4f,\"scrolled_px\":%.1f,\"final_offset\":%.1f,\"max_offset\":%.1f,"
        "\"max_offset_end\":%.1f,\"geometry_stable\":%s,\"scroll_viewport_h\":%.1f,"
        "\"content_screens\":%.3f,\"dp_per_unit\":%.3f,\"viewport_h\":%.1f,\"settle_frames\":%zu,"
        "\"settle_ms\":%.0f,\"settled\":%s,\"settle_reason\":\"%s\",\"trustworthy\":%s,\"report\":",
        scrollable_found ? "true" : "false", moved_frames, idle_frames, reversals, reversal_ratio(), scrolled_px,
        static_cast<double>(final_offset), static_cast<double>(max_offset), static_cast<double>(max_offset_end),
        geometry_stable() ? "true" : "false", static_cast<double>(scroll_viewport_h),
        static_cast<double>(content_screens()), static_cast<double>(dp_per_unit), static_cast<double>(viewport.height),
        settle_frames, settle_ms, settled ? "true" : "false", settle_reason_name(settle_reason),
        trustworthy() ? "true" : "false");
    return prefix + report.to_json() + "}";
}

auto ScrollBenchHarness::Result::csv_header() -> std::string {
    return PerfReport::csv_header() +
           ",scrollable_found,moved_frames,idle_frames,reversals,scrolled_px,max_offset,max_offset_end"
           ",scroll_viewport_h,dp_per_unit,settle_frames,settle_reason,trustworthy";
}

auto ScrollBenchHarness::Result::to_csv_row() const -> std::string {
    const std::string suffix = aurora::internal::string_format(
        ",%d,%zu,%zu,%zu,%.1f,%.1f,%.1f,%.1f,%.3f,%zu,%s,%d", scrollable_found ? 1 : 0, moved_frames, idle_frames,
        reversals, scrolled_px, static_cast<double>(max_offset), static_cast<double>(max_offset_end),
        static_cast<double>(scroll_viewport_h), static_cast<double>(dp_per_unit), settle_frames,
        settle_reason_name(settle_reason), trustworthy() ? 1 : 0);
    return report.to_csv_row() + suffix;
}

// ---------------------------------------------------------------------------
// ScrollBenchHarness::run
// ---------------------------------------------------------------------------

auto ScrollBenchHarness::run(Node root, Size viewport) -> Result { return run(std::move(root), viewport, Config{}); }

auto ScrollBenchHarness::run(Node root, Size viewport, const Config &cfg) -> Result {
    Result res;
    res.viewport = viewport;
    if (!root || viewport.width <= 0.0F || viewport.height <= 0.0F) {
        return res;  // 非法输入：scrollable_found = false，调用方经 trustworthy() 识别
    }

    const float scale = cfg.scale > 0.0F ? cfg.scale : 1.0F;
    auto surface = std::make_unique<HeadlessSurface>(std::string{}, viewport);
    surface->painter().set_scale(scale);
    (void)surface->begin_frame(static_cast<int>(viewport.width), static_cast<int>(viewport.height));
    Window win{std::move(surface)};

    // 首帧：驱动一次完整 layout，动态子树（如 AppShell 在 on_layout 内构建的卡片）在此建成，
    // 之后才谈得上「在树里找滚动容器」。
    (void)win.present_root(root);

    // 落定阶段：不滚动、只空转帧，等骨架屏 / 入场动画 / 延迟内容全部就位。
    //
    // 判据取「连续无脏」**或**「墙钟达标」，两者都算正常落定。只认前者是错的：
    // `demo_google_play` 的 BannerCarousel 每帧 `mark_needs_paint` 自驱动轮播，这类树
    // 原理上永远不会 idle，硬等 idle 只会等到上限然后误报「未收敛」。骨架屏究竟退没
    // 退场，改由采样前后的行程复测（`geometry_stable()`）事后证伪，不在这里猜。
    if (cfg.settle_ms > 0.0) {
        const Stopwatch settle_sw;
        int consecutive_idle = 0;
        while (std::cmp_less(res.settle_frames, cfg.settle_max_frames)) {
            (void)win.present_root(root);
            ++res.settle_frames;
            consecutive_idle = win.is_idle_frame() ? consecutive_idle + 1 : 0;
            if (consecutive_idle >= cfg.settle_idle_frames) {
                res.settled = true;
                res.settle_reason = Result::SettleReason::Idle;
                break;
            }
            if (settle_sw.elapsed_ms() >= cfg.settle_ms) {
                res.settled = true;
                res.settle_reason = Result::SettleReason::TimeBudget;
                break;
            }
        }
        res.settle_ms = settle_sw.elapsed_ms();
    } else {
        res.settled = true;  // 显式关闭落定阶段：不作告警
        res.settle_reason = Result::SettleReason::Disabled;
    }

    // 落定后才谈得上在树里找滚动容器：动态子树（AppShell 的骨架 → 真实内容）此时才定型。
    ScrollProbe probe = ScrollProbe::find(root.widget());
    res.scrollable_found = probe.valid();
    res.scroll_viewport_h = probe.viewport_h();

    const Point center{.x = viewport.width * 0.5F, .y = viewport.height * 0.5F};
    int dir = 1;  ///< +1 = 向下滚（内容上移）；触边由 auto_reverse 翻转
    float velocity = cfg.delta_per_frame * (cfg.fling ? cfg.fling_boost : 1.0F);

    // 滚轮约定（全库一致）：delta_y 正方向为「向上滚动」，故向下滚需取负号。
    const auto dispatch_scroll = [&root, &center, &dir](float amount) -> void {
        ScrollEvent e;
        e.position = center;
        e.delta_y = -static_cast<float>(dir) * amount;
        (void)EventDispatcher::dispatch(root.widget(), e);
    };

    // 行程复测：一次性拉到底读出最大偏移，再拉回顶。滚动控件内部会把目标 clamp 到
    // [0, content_h - viewport_h]，因此这两下不渲染、不改变采样起点，却能拿到「这棵树
    // 到底能滚多远」。采样前后各测一次：两次不等 = 内容在采样期间还在长（骨架屏没退场
    // 就开测就是这个症状），`geometry_stable()` 据此把读数判为不可信。
    const auto measure_extent = [&probe, &dispatch_scroll, &dir]() -> float {
        if (!probe.valid()) {
            return 0.0F;
        }
        constexpr float kHugeDelta = 1.0e6F;  // NOLINT(readability-identifier-naming)
        const int saved_dir = dir;
        dir = 1;
        dispatch_scroll(kHugeDelta);  // 拉到底
        const float extent = probe.offset();
        dir = -1;
        dispatch_scroll(kHugeDelta);  // 拉回顶，恢复采样起点
        dir = saved_dir;
        return extent;
    };

    res.max_offset = measure_extent();

    // 步长标定：把「每帧滚多少 dp」翻译成控件认的滚轮单位。
    //
    // 必须实测而不能写死：`Scroll::step` 默认 16 dp/单位，`LazyList` / `GridView` 各有各的
    // 步长，同一个滚轮增量在不同控件上位移不同。不标定就意味着「同一份配置在两个场景里
    // 其实在按不同速度滚」，两组读数没有可比性。
    // 从顶部下发一个单位增量读位移即可；若读数正好等于行程说明被 clamp 了（行程比一个
    // 单位还短），减半重试。
    res.dp_per_unit = 0.0F;
    if (probe.valid() && res.max_offset > 0.0F) {
        float unit = 1.0F;
        for (int attempt = 0; attempt < 8; ++attempt) {
            dispatch_scroll(unit);  // dir 恒为 +1，起点为顶部
            const float moved = probe.offset();
            dir = -1;
            dispatch_scroll(1.0e6F);  // 回顶
            dir = 1;
            if (moved > 0.0F && moved < res.max_offset - 0.01F) {
                res.dp_per_unit = moved / unit;
                break;
            }
            if (moved <= 0.0F) {
                break;  // 一个单位都推不动：控件不响应，交由 moved_frames 判伪
            }
            unit *= 0.5F;
        }
    }
    // 标定失败（不可滚 / 不响应）时退化为 1:1，读数会由 trustworthy() 判伪，不掩盖问题。
    const float dp_per_unit = res.dp_per_unit > 0.0F ? res.dp_per_unit : 1.0F;

    PerfSession sess{cfg.name, static_cast<std::size_t>(cfg.frames > 0 ? cfg.frames : 1)};
    sess.set_frame_budget_ms(cfg.frame_budget_ms);

    const int warmup = cfg.warmup_frames > 0 ? cfg.warmup_frames : 0;
    const int frames = cfg.frames > 0 ? cfg.frames : 0;

    for (int i = 0; i < warmup + frames; ++i) {
        const bool sampling = i >= warmup;
        // 配置里是 dp，控件收的是滚轮单位，按标定系数换算后再下发。
        const float amount = (cfg.fling ? velocity : cfg.delta_per_frame) / dp_per_unit;

        Stopwatch sw;
        const float before = probe.offset();
        dispatch_scroll(amount);
        float after = probe.offset();
        // 触顶/触底：本帧没滚动起来 → 立刻反向重发，使这一帧仍是**真实滚动帧**，
        // 而不是一个会污染 p99 的 idle 帧。
        if (probe.valid() && after == before && cfg.auto_reverse) {
            dir = -dir;
            ++res.reversals;
            dispatch_scroll(amount);
            after = probe.offset();
        }
        (void)win.present_root(root);
        const double frame_ms = sw.elapsed_ms();

        if (sampling) {
            sess.record_frame(frame_ms);
            if (after != before) {
                ++res.moved_frames;
                res.scrolled_px += std::fabs(static_cast<double>(after) - static_cast<double>(before));
            }
            if (win.is_idle_frame()) {
                ++res.idle_frames;
            }
        }

        if (cfg.fling) {
            velocity *= cfg.fling_decay;
            if (velocity < cfg.fling_cutoff) {
                velocity = cfg.delta_per_frame * cfg.fling_boost;  // 甩完一次，紧接下一次
            }
        }
    }

    res.report = sess.report();
    res.final_offset = probe.offset();
    // 采样后复测行程：与采样前不一致说明内容几何在采样期间还在变（典型：骨架屏中途退场）。
    res.max_offset_end = measure_extent();
    return res;
}

}  // namespace aurora
