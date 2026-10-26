#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

#include "aurora/core/types.h"

namespace aurora {

/// @brief 吸附对齐方位：条目边沿与视口边沿的贴合方式（语义对齐 CSS scroll-snap-align）。
enum class ScrollSnapAlignment : std::uint8_t {
    Start,  ///< 条目前沿贴视口前沿（offset = k·extent）
    Center,  ///< 条目中心贴视口中心
    End,  ///< 条目后沿贴视口后沿（offset = (k+1)·extent − viewport）
};

/// @brief 滚动吸附配置（snap/paging）：收位目标吸附到 `extent` 的整数倍条目。
///
/// `paging = true` 时以视口尺寸为周期（一页 = 一屏，`extent` 取值被忽略），
/// 等价于 CSS `scroll-snap-type: y mandatory` 的分页语义。
struct ScrollSnap {
    /// @brief 分页工厂：以视口高为周期的吸附（常配 Start 或 Center 对齐）。
    /// @param align 条目边沿与视口边沿的贴合方位，默认 Start。
    /// @return paging=true、extent=0 的 ScrollSnap，对齐方式取 align。
    [[nodiscard]] static auto page(ScrollSnapAlignment align = ScrollSnapAlignment::Start) -> ScrollSnap {
        return {.paging = true, .extent = 0.0F, .alignment = align};
    }

    bool paging = false;  ///< true = 分页模式（周期取视口高）
    float extent = 0.0F;  ///< 吸附周期（dp）；非分页且 <= 0 时 = 吸附关闭
    ScrollSnapAlignment alignment = ScrollSnapAlignment::Start;  ///< 对齐方位

    /// @brief 本配置是否生效（分页恒生效；否则须有正周期）。
    /// @param viewport_h 当前视口高度（分页模式下为生效判据）。
    /// @return 分页时视口高 > 0 才为 true；非分页时 extent > 0 即为 true。
    [[nodiscard]] auto enabled(float viewport_h) const -> bool { return paging ? viewport_h > 0.0F : extent > 0.0F; }
};

/// @brief 收位滑动动画（snap 收位 / 程序化 scroll-to 的共用时序数学）。
///
/// 150ms easeOutCubic 从 `from` 收敛到 `to`；由宿主控件的 `tick_gestures` 逐帧推进
/// （Dismissible/ReorderableList 自驱动先例，不占 `Animator`）。reduce-motion 的短路
/// 属宿主职责（对齐 `AnimationController::tick` 语义：直落端点、不产生中间帧）。
struct ScrollGlide {
    /// @brief 默认滑动总时长：150ms easeOutCubic 收敛。
    /// @return 0.15 秒。
    [[nodiscard]] static constexpr auto default_duration_s() -> double { return 0.15; }

    float from = 0.0F;  ///< 起点偏移
    float to = 0.0F;  ///< 终点偏移
    double elapsed_s = 0.0;  ///< 已累计时长
    double duration_s = default_duration_s();  ///< 总时长（可测调）
    bool active = false;  ///< 是否滑动中

    /// @brief 启动/重定向：起点取当前值；目标等于当前值则不启动（保持既有态）。
    /// @param current 启动时的当前偏移，作为滑动起点。
    /// @param target 滑动目标偏移。
    auto start(float current, float target) -> void {
        if (current == target) {
            return;  // 已在目标：不重置进行中的滑动，也不新起
        }
        from = current;
        to = target;
        elapsed_s = 0.0;
        active = true;
    }

    /// @brief 推进 dt 秒，返回本帧偏移；到达终点时置 active=false 并返回精确 `to`。
    /// @param dt_s 本帧时长（秒），累计进 elapsed_s。
    /// @return 本帧 easeOutCubic 插值偏移；未激活或已收位时返回终点 `to`。
    auto tick(double dt_s) -> float {
        if (!active) {
            return to;
        }
        elapsed_s += dt_s;
        const double u = std::clamp(elapsed_s / duration_s, 0.0, 1.0);
        if (u >= 1.0) {
            active = false;
            return to;
        }
        const double eased = 1.0 - std::pow(1.0 - u, 3.0);
        return static_cast<float>(from + ((to - from) * eased));
    }
};

/// @brief 滚动视口内核：全库滚动共享的 offset/clamp/符号约定。
///
/// 抽取自 Scroll 组件的滚动数学，供两类滚动方复用：
///  - 真实滚动控件（Scroll / LazyList / LazyRow / GridView）：自带内容测量与离屏缓冲，
///    通过 `clamp_offset` 复用同一夹取与符号约定；
///  - 通用容器 `OverflowStrategy::Scroll`（Widget 基类内嵌实例）：仅做「裁剪 + 内容平移」
///    的轻量滚动，复用同一 `apply_scroll` 语义，保证滚轮手感全库一致。
///
/// 符号约定（与全库滚动一致，见 Scroll::on_scroll 注释）：`delta_y` 正方向为「向上滚动」，
/// 此时 offset 应减小；offset 增大表示内容上移露出下方内容。
/// @note Thread: main-thread only
struct ScrollViewport {
    float offset_y = 0.0F;  ///< 当前滚动偏移（内容坐标，0 = 顶部）
    float content_h = 0.0F;  ///< 内容自然高度
    float viewport_h = 0.0F;  ///< 视口高度
    float step = 16.0F;  ///< 每单位滚轮增量的滚动像素（与 ScrollProps::step 默认一致）

    /// @brief 最大可滚动偏移：内容超出视口的部分，不足时为 0（不可滚）。
    /// @return max(0, content_h - viewport_h)。
    [[nodiscard]] auto max_offset() const -> float { return std::max(0.0F, content_h - viewport_h); }

    /// @brief 滚轮增量驱动滚动：夹取到 [0, max_offset]，offset 变化时返回 true。
    /// @param delta_y 滚轮增量；正方向为向上滚动（offset 减小）。
    /// @return 实际发生滚动（新目标与当前 offset 不同）时为 true。
    auto apply_scroll(float delta_y) -> bool {
        const float target = clamp_offset(offset_y, delta_y, step, content_h, viewport_h);
        if (target == offset_y) {
            return false;
        }
        offset_y = target;
        return true;
    }

    /// @brief 共享夹取数学：由 (offset, delta_y, step, content_h, viewport_h) 算出新 offset。
    ///        无状态静态形式，供真实滚动控件在自有状态上复用同一约定。
    /// @param offset 当前偏移。
    /// @param delta_y 滚轮增量；正方向为向上滚动（offset 减小）。
    /// @param step 每单位增量的滚动像素。
    /// @param content_h 内容自然高度。
    /// @param viewport_h 视口高度。
    /// @return 夹取到 [0, max(0, content_h-viewport_h)] 的新 offset。
    static auto clamp_offset(float offset, float delta_y, float step, float content_h, float viewport_h) -> float {
        const float max_off = std::max(0.0F, content_h - viewport_h);
        return std::clamp(offset - (delta_y * step), 0.0F, max_off);
    }

    /// @brief 垂直滚动 clamp 后的未消费余量（供消费方回传 `ScrollEvent::remaining_y`）：
    ///        `delta_y - 已吃掉量`。到顶/到底被夹掉的部分即余量（保留符号）；
    ///        `step <= 0` 视为不可滚，全量退为余量。
    /// @param before 消费前的偏移。
    /// @param after 消费后被夹取的偏移。
    /// @param delta_y 原始滚轮增量。
    /// @param step 每单位增量的滚动像素；<= 0 视为不可滚。
    /// @return 未被消费的余量（保留原增量符号）。
    static auto remaining_offset(float before, float after, float delta_y, float step) -> float {
        if (step <= 0.0F) {
            return delta_y;
        }
        return delta_y - ((before - after) / step);
    }

    /// @brief 吸附收位目标：当前 offset 在 `snap` 下最近的条目对齐点（夹到 [0, max_offset]）。
    ///        吸附关闭（非分页且 extent<=0，或分页但视口未定）时退化为普通夹取。
    /// @param offset 待收位的当前偏移。
    /// @param content_h 内容自然高度。
    /// @param viewport_h 视口高度（分页模式下作为周期）。
    /// @param snap 吸附配置：周期取 extent（分页时取视口高），对齐方式决定目标公式。
    /// @return 最近对齐点偏移，越界条目夹到 max_offset。
    static auto snap_target(float offset, float content_h, float viewport_h, const ScrollSnap &snap) -> float {
        const float max_off = std::max(0.0F, content_h - viewport_h);
        const float plain = std::clamp(offset, 0.0F, max_off);
        const float ext = snap.paging ? viewport_h : snap.extent;
        if (!snap.enabled(viewport_h)) {
            return plain;
        }
        float candidate = 0.0F;
        switch (snap.alignment) {
            case ScrollSnapAlignment::Start:
                // lround 得整条目号，转 float 属真窄化（long 32 位 > float 24 位尾数）——显式写出来，
                // 乘法仍在 float 域做，数值与原式一字不动。
                candidate = static_cast<float>(std::lround(offset / ext)) * ext;
                break;
            case ScrollSnapAlignment::Center: {
                // 条目 k 覆盖 [k·ext, (k+1)·ext)，其中心贴视口中心：offset = k·ext + ext/2 − viewport/2
                // 取整用 round 而非 lround：后者返回 long，在 LP64（Linux / macOS）上宽于 double 的
                // 53 位尾数，赋给 double 属真窄化（Windows 上 long 与 int 同宽才看不出来）。两者同为
                // 「半数远离零取整」，此处 k 只当整数条目号用，数值一字不动。
                const double k = std::round((offset + (viewport_h / 2.0F) - (ext / 2.0F)) / ext);
                candidate = static_cast<float>(k * ext) + (ext / 2.0F) - (viewport_h / 2.0F);
                break;
            }
            case ScrollSnapAlignment::End: {
                // 条目 k 的后沿 (k+1)·ext 贴视口后沿：offset = (k+1)·ext − viewport
                // 同上：round 而非 lround，避免 long → double 的 LP64 窄化。
                const double k = std::round((offset + viewport_h) / ext);
                candidate = static_cast<float>(k * ext) - viewport_h;
                break;
            }
        }
        // 越界条目（内容末尾不足一个周期）夹到 max_off——末端可达性优先于严格对齐。
        return std::clamp(candidate, 0.0F, max_off);
    }
};

/// @brief 近边缘自动滚边触发带宽度（dp）：拖入落点距视口边缘不足此宽度即触发自动滚边。
///
/// 进程内拖拽（`ReorderableList` 的近边缘 auto-scroll）与跨进程系统拖放
/// （`Widget::on_system_drag_auto_scroll` 链上最近可滚动祖先的滚边）共用同一缺省，
/// 两条路径手感一致。0 = 关闭（控件侧以本常量作默认值，仍可各自调参）。
inline constexpr float AURORA_AUTO_SCROLL_THRESHOLD_DP = 48.0F;

/// @brief 自动滚边最大速度（dp/s）：落点侵入触发带全深时的滚动速度（按侵入深度比例衰减）。
inline constexpr float AURORA_AUTO_SCROLL_MAX_SPEED = 600.0F;

/// @brief 拖入自动滚边的新鲜度上限（秒）：宿主停发 `Over` 意图超过此时长即收车。
///
/// 兜底「指针静默离开视口但显式收车送不到本链」的情形——`Leave` 事件的落点可能在窗口外，
/// 命中测试落空时收车意图无从送达滚动祖先，此时由本超时兜底（与在途读取的墙钟超时同款
/// 「不悬挂」口径）。取值须远大于指针采样周期，避免误收车。
inline constexpr double AURORA_DRAG_AUTO_SCROLL_STALE_S = 1.0;

/// @brief 拖入自动滚边状态（跨进程系统拖放路径「谁驱动滚动」的内核）。
///
/// 宿主在 `SystemDropPhase::Over` 态经 `Widget::on_system_drag_auto_scroll` 把落点交给
/// 链上最近可滚动祖先，由后者在自己的帧计时里调 `advance` 取本帧增量、夹取后写入自身
/// offset——**不新增滚动实现**，只补滚边契约。与 `ReorderableList` 的进程内 auto-scroll
/// 共用同一组调参常量（`AURORA_AUTO_SCROLL_THRESHOLD_DP` / `AURORA_AUTO_SCROLL_MAX_SPEED`）。
///
/// 时间基准全部取自调用方帧计时的 `now`（单调时钟；测试可注入假帧钟），本结构不自行读钟——
/// 驱动意图只记「待兑现」标志，由下一帧 `advance` 兑现新鲜度，避免事件时刻与帧时刻两套钟混用。
struct DragAutoScroll {
    /// @brief 记录一次驱动意图（`Over` 落点，窗口逻辑 dp）；首次记录即激活。
    /// @param window_position 落点（窗口逻辑 dp）。
    auto drive(const Point &window_position) -> void {
        position_ = window_position;
        intent_pending_ = true;  // 新鲜度由下一帧 advance 兑现（见结构体说明）
        if (!active) {
            active = true;
            last_tick_.reset();  // 首帧 dt 由调用方兜底（1/60 秒）
        }
    }

    /// @brief 收车：清空全部状态（`Leave` / `Drop` / 内容不可滚 / 落点在带外 / 新鲜度超时）。
    auto stop() -> void {
        active = false;
        position_.reset();
        last_tick_.reset();
        intent_pending_ = false;
    }

    /// @brief 本帧滚动增量（dp，正 = 向下滚 = offset 增大）。
    ///
    /// 未激活 / 新鲜度超时 / 落点在触发带外 / 视口盒不可用时返回 0 并**就地收车**
    /// （`active` 转 false，调用方据此摘除帧计时）。带内按侵入深度比例给速：落点抵满
    /// 视口边缘 = `AURORA_AUTO_SCROLL_MAX_SPEED`，退至触发带界趋近 0。
    /// @param viewport_window_box 视口的窗口逻辑盒（原点即视口左上，落点据此换算本地坐标）。
    /// @param now 本帧时刻（单调时钟）；刷新新鲜度并与上一推进时刻求差得本帧 dt。
    /// @return 本帧增量（dp）；收车时为 0。
    [[nodiscard]] auto advance(const Rect &viewport_window_box, std::chrono::steady_clock::time_point now) -> float {
        if (!active || !position_.has_value()) {
            return 0.0F;
        }
        if (intent_pending_) {
            last_intent_ = now;  // 本帧有新驱动意图：刷新新鲜度
            intent_pending_ = false;
        } else if (std::chrono::duration<double>(now - last_intent_).count() > AURORA_DRAG_AUTO_SCROLL_STALE_S) {
            stop();  // 宿主停发意图超时：收车（指针已静默离开本视口）
            return 0.0F;
        }
        const float local_y = position_->y - viewport_window_box.origin.y;
        const float viewport_h = viewport_window_box.size.height;
        const float band = std::min(AURORA_AUTO_SCROLL_THRESHOLD_DP, viewport_h / 2.0F);
        if (band <= 0.0F) {
            stop();  // 阈值关闭或视口未定尺：收车
            return 0.0F;
        }
        float speed = 0.0F;  // dp/s，正 = 向下滚
        if (local_y < band) {
            speed = -AURORA_AUTO_SCROLL_MAX_SPEED * (1.0F - (std::max(0.0F, local_y) / band));
        } else if (local_y > viewport_h - band) {
            speed = AURORA_AUTO_SCROLL_MAX_SPEED * (1.0F - (std::max(0.0F, viewport_h - local_y) / band));
        }
        if (speed == 0.0F) {
            stop();  // 落点在触发带外：收车
            return 0.0F;
        }
        const double dt =
            last_tick_.has_value() ? std::chrono::duration<double>(now - *last_tick_).count() : (1.0 / 60.0);
        last_tick_ = now;
        return static_cast<float>(speed * dt);
    }

    bool active = false;  ///< 是否正在滚边（调用方据此决定是否继续占帧计时）

  private:
    std::optional<Point> position_;  ///< 最近一次 `Over` 落点（窗口逻辑 dp）
    std::chrono::steady_clock::time_point last_intent_;  ///< 最近一次意图兑现时刻（新鲜度判据）
    std::optional<std::chrono::steady_clock::time_point> last_tick_;  ///< 最近一次推进时刻（dt 判据）
    bool intent_pending_ = false;  ///< 上次 `drive` 尚未被任一帧 `advance` 兑现
};

}  // namespace aurora
