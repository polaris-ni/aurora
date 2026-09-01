#pragma once

/**
 * @file scroll_bench.h
 * @brief 滚动性能确定性基准。
 *
 * 为什么需要它：「手滚一下感觉卡」不是可回归的度量。本 harness 用
 * `HeadlessSurface` + **程序化滚轮事件序列**把滚动变成可重复、
 * 可比较、可进 CI 的实验：同一棵树 + 同一段事件序列 → 同一组计数器读数。
 *
 * 验收看 `p99_ms` / `jitter_ms` / `full_redraw_frames` 三项，**不看 `avg_frame_ms`**
 * ——均值会把偶发的 80ms 卡顿摊平成看不见。
 *
 * 自证机制（这类工具最大的风险是「测了个寂寞」）：harness 每帧读取被测滚动控件的
 * 真实偏移量，产出 `moved_frames` / `scrolled_px` / `idle_frames`。若树里根本没有可滚
 * 动控件、或事件没命中它，`scrollable_found = false` 与 `moved_frames = 0` 会直接暴露，
 * 而不是给出一组漂亮却无意义的读数。调用方（工具 / 单测）须先校验这些字段再看性能数。
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "aurora/core/types.h"
#include "aurora/perf/counters.h"
#include "aurora/perf/perf_session.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 滚动性能确定性基准：Headless + 程序化滚动事件序列。
 *
 * 用法：
 * @code
 *   ScrollBenchHarness::Config cfg;      // 默认 warmup 30 + 采样 300 帧，匀速
 *   auto r = ScrollBenchHarness::run(build_content_tree(), Size{ 1100, 760 }, cfg);
 *   if (!r.trustworthy()) { ... }        // 先验伪，再看数
 *   AURORA_LOG_RAW("bench", r.to_markdown(), "\n");
 * @endcode
 *
 * @note Thread: main-thread only（单线程 UI 模型）
 * @note Side-effects: 驱动一份独立的 `HeadlessSurface`；不写文件、不写日志。
 *       会读写进程级 `RenderCounters` / `Profiler` 单例（帧作用域由 `present_root` 管理）。
 */
class ScrollBenchHarness {
  public:
    /// @brief 采样配置。
    struct Config {
        int frames = 300; ///< 采样帧数（不含 warmup）

        /// @brief 每帧滚动距离，单位**逻辑 dp**（正值 = 向下滚，露出下方内容）。
        ///
        /// 刻意不用「滚轮单位」：`Scroll::step` 默认 16 dp/单位，而 `LazyList` / `GridView`
        /// 各有自己的步长，同一个滚轮增量在不同控件上位移不同，跨场景不可比。harness 在
        /// 采样前用一次单位增量实测 dp/unit 做标定，再把这里的 dp 换算回控件单位下发。
        /// 默认 12 dp/帧 ≈ 720 dp/s @60fps，是一个真实的匀速滚动速度。
        float delta_per_frame = 12.0f;
        bool fling = false;     ///< true = 惯性衰减序列；false = 匀速
        int warmup_frames = 30; ///< 预热帧数（不计入统计，用于消化首帧布局/入场动画）

        float scale = 1.0f;             ///< 设备像素缩放（经 `Painter::set_scale` 真实生效）
        bool auto_reverse = true;       ///< 触顶/触底自动反向，保证每帧都是真实滚动帧
        double frame_budget_ms = 16.67; ///< 帧预算，决定 `over_budget_frames`
        std::string name = "scroll";    ///< 会话名，写入报告标题

        // ---- 落定（settle）阶段：warmup 之前，不滚动，只空转帧 ----
        //
        // 真实业务首屏常有骨架屏 / 入场动画 / 延迟出现的内容（`demo_google_play` 是
        // 700ms 骨架屏 + 0.32s 卡片入场）。不等它落定就开滚，量到的是「骨架屏很短、
        // 一滚就到底」的假象——上一版实测 30 帧里反向了 11 次，就是踩了这个坑。
        //
        // 退出条件二选一，**都算正常落定**：
        //  - 连续 `settle_idle_frames` 帧无脏（静态树）；
        //  - 墙钟达到 `settle_ms`（含永动动画的树，例如自动轮播 banner —— 这类树
        //    原理上永远不会 idle，只能按时间给足首屏瞬态）。
        // 只有帧数撞到 `settle_max_frames` 才判未落定。骨架屏是否真的退场，不靠这里
        // 猜，而由采样前后两次行程复测（`Result::geometry_stable()`）事后证伪。
        double settle_ms = 1500.0;    ///< 落定阶段墙钟目标（达到即视为落定）；0 = 关闭
        int settle_idle_frames = 24;  ///< 连续这么多帧无脏即提前判定已落定
        int settle_max_frames = 4000; ///< 落定阶段帧数硬上限（唯一的失败出口）

        // ---- fling 模式参数（`fling = false` 时忽略）----
        float fling_boost = 4.0f;  ///< 起始速度 = `delta_per_frame * fling_boost`（dp/帧）
        float fling_decay = 0.94f; ///< 每帧速度衰减系数，(0,1)
        float fling_cutoff = 0.5f; ///< 速度（dp/帧）低于此值判定为静止，随即发起下一次 fling
    };

    /**
     * @brief 采样结果。
     *
     * 帧统计全部收敛在 `report`（`PerfReport`）里，本结构只额外携带**滚动**相关的
     * 自证字段；`p99_ms` 等读数以转发访问器给出，避免同一份数据在两处
     * 各存一份而失同步。
     */
    struct Result {
        /// @brief 落定阶段的退出原因（写进报告，避免「落定成功」是怎么来的说不清）。
        enum class SettleReason : std::uint8_t {
            Disabled,   ///< `settle_ms <= 0`，调用方主动关闭
            Idle,       ///< 连续无脏收敛（静态树）
            TimeBudget, ///< 墙钟达标（含永动动画的树，正常路径）
            FrameCap,   ///< 撞帧数上限，**未落定**
        };

        PerfReport report{}; ///< 完整帧统计（markdown / json / csv 由它渲染）
        Size viewport{};     ///< 本次采样的视口逻辑尺寸（dp）

        // ---- 自证字段：先验伪，再看性能数 ----
        bool scrollable_found = false;                       ///< 是否在树中定位到垂直可滚动控件
        std::size_t moved_frames = 0;                        ///< 实际产生位移的采样帧数（应 == frames）
        std::size_t idle_frames = 0;                         ///< 被 idle 跳帧优化跳过的采样帧数（应为 0）
        std::size_t reversals = 0;                           ///< 触边反向次数（占比过高说明内容太短，读数无参考价值）
        double scrolled_px = 0.0;                            ///< 累计位移绝对值（逻辑 dp）
        float final_offset = 0.0f;                           ///< 结束时的滚动偏移（逻辑 dp）
        float max_offset = 0.0f;                             ///< 采样**前**实测行程（内容高 − 滚动视口高，逻辑 dp）
        float max_offset_end = 0.0f;                         ///< 采样**后**复测行程；与上者不等 = 采样期内容还在变
        float scroll_viewport_h = 0.0f;                      ///< 滚动容器自身视口高（≠ 窗口视口高，顶栏/底栏会挤占）
        float dp_per_unit = 0.0f;                            ///< 标定所得：一个滚轮单位在被测控件上等于多少 dp
        std::size_t settle_frames = 0;                       ///< 落定阶段耗用帧数
        double settle_ms = 0.0;                              ///< 落定阶段耗用墙钟毫秒
        bool settled = false;                                ///< 落定阶段是否正常结束（false = 撞帧数上限）
        SettleReason settle_reason = SettleReason::FrameCap; ///< 落定退出原因

        /// @brief 触边反向帧占比上限：超过则判定内容太短，整段读数以触边行为为主。
        static constexpr double kMaxReversalRatio = 0.10; // NOLINT(readability-identifier-naming)

        /// @brief 采样期间内容几何是否稳定（采样前后行程一致）。
        ///
        /// 这是识别「骨架屏没退场就开测」的**事后**判据，比在落定阶段猜启发式规则可靠：
        /// 骨架屏与真实内容高度不同，只要采样中途发生切换，两次行程复测必然对不上。
        [[nodiscard]] auto geometry_stable() const -> bool;

        /// @brief 内容有多少屏（滚动容器视口的倍数）。< 2.0 表示内容不足两屏。
        [[nodiscard]] auto content_screens() const -> float;

        /// @brief 触边反向帧占比。
        [[nodiscard]] auto reversal_ratio() const -> double;

        /// @brief 读数是否可信。全部满足才为 true：定位到滚动控件、落定阶段正常结束、
        /// 采样期每帧都真的在滚、无 idle 跳帧、树确实可滚、内容几何稳定、触边反向占比
        /// 不超过 `kMaxReversalRatio`。任一不满足都说明「测了个寂寞」，性能数不该采信。
        [[nodiscard]] auto trustworthy() const -> bool;

        // ---- 汇总读数（转发 `report`，单一数据源）----
        [[nodiscard]] auto avg_frame_ms() const -> double { return report.avg_frame_ms; }
        [[nodiscard]] auto p50_ms() const -> double { return report.p50_ms; }
        [[nodiscard]] auto p95_ms() const -> double { return report.p95_ms; }
        [[nodiscard]] auto p99_ms() const -> double { return report.p99_ms; }
        [[nodiscard]] auto worst_ms() const -> double { return report.worst_ms; }
        [[nodiscard]] auto jitter_ms() const -> double { return report.jitter_ms; }
        /// @brief **主验收指标**：滚动期间退化为整帧重绘的帧数。
        [[nodiscard]] auto full_redraw_frames() const -> std::size_t { return report.full_redraw_frames; }
        [[nodiscard]] auto long_task_count() const -> std::size_t { return report.long_task_count; }
        [[nodiscard]] auto counters_sum() const -> const RenderCounters & { return report.counters_sum; }
        [[nodiscard]] auto counters_max() const -> const RenderCounters & { return report.counters_max; }

        /// @brief Markdown 报告：`report` 的表格 + 滚动自证行。
        [[nodiscard]] auto to_markdown() const -> std::string;
        /// @brief JSON 对象：`report` 的字段 + 滚动自证字段。
        [[nodiscard]] auto to_json() const -> std::string;
        /// @brief CSV 数据行（字段顺序与 `csv_header()` 严格对应）。
        [[nodiscard]] auto to_csv_row() const -> std::string;
        /// @brief CSV 表头。
        [[nodiscard]] static auto csv_header() -> std::string;
    };

    /**
     * @brief 跑一次滚动基准。
     *
     * 流程：建 Headless 窗口 → 首帧 present（触发布局，动态子树在此建成）→ **落定阶段**
     * （空转到骨架屏/入场动画结束）→ 定位可滚动控件 → **行程复测**（拉到底读最大偏移再
     * 拉回顶）→ warmup（照常滚动但不计入统计）→ 采样 `cfg.frames` 帧 → **行程再复测**。
     * 每帧：派发一次滚轮事件（命中视口中心，与真实应用同一条 `EventDispatcher` 路径）
     * → `Window::present_root` → 记录耗时与 `RenderCounters`。
     *
     * @param root     被测内容树（按值取，harness 独占驱动，避免与调用方共享挂载状态）
     * @param viewport 视口逻辑尺寸（dp）
     * @param cfg      采样配置
     * @return 采样结果；先查 `Result::trustworthy()` 再读性能指标
     */
    [[nodiscard]] static auto run(Node root, Size viewport, const Config &cfg) -> Result;

    /// @brief 以默认配置（warmup 30 + 采样 300 帧，匀速）跑一次。
    /// @note 独立重载而非默认实参：`Config` 是嵌套类型，在类体内其默认成员初始化器
    ///       尚未完成，`= Config{}` 会导致 "required before the end of its enclosing class"。
    [[nodiscard]] static auto run(Node root, Size viewport) -> Result;
};

} // namespace aurora
