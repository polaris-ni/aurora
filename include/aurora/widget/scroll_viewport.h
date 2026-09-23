#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
    [[nodiscard]] static auto page(ScrollSnapAlignment align = ScrollSnapAlignment::Start) -> ScrollSnap {
        return {.paging = true, .extent = 0.0F, .alignment = align};
    }

    bool paging = false;  ///< true = 分页模式（周期取视口高）
    float extent = 0.0F;  ///< 吸附周期（dp）；非分页且 <= 0 时 = 吸附关闭
    ScrollSnapAlignment alignment = ScrollSnapAlignment::Start;  ///< 对齐方位

    /// @brief 本配置是否生效（分页恒生效；否则须有正周期）。
    [[nodiscard]] auto enabled(float viewport_h) const -> bool { return paging ? viewport_h > 0.0F : extent > 0.0F; }
};

/// @brief 收位滑动动画（snap 收位 / 程序化 scroll-to 的共用时序数学）。
///
/// 150ms easeOutCubic 从 `from` 收敛到 `to`；由宿主控件的 `tick_gestures` 逐帧推进
/// （Dismissible/ReorderableList 自驱动先例，不占 `Animator`）。reduce-motion 的短路
/// 属宿主职责（对齐 `AnimationController::tick` 语义：直落端点、不产生中间帧）。
struct ScrollGlide {
    [[nodiscard]] static constexpr auto default_duration_s() -> double { return 0.15; }

    float from = 0.0F;  ///< 起点偏移
    float to = 0.0F;  ///< 终点偏移
    double elapsed_s = 0.0;  ///< 已累计时长
    double duration_s = default_duration_s();  ///< 总时长（可测调）
    bool active = false;  ///< 是否滑动中

    /// @brief 启动/重定向：起点取当前值；目标等于当前值则不启动（保持既有态）。
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
    [[nodiscard]] auto max_offset() const -> float { return std::max(0.0F, content_h - viewport_h); }

    /// @brief 滚轮增量驱动滚动：夹取到 [0, max_offset]，offset 变化时返回 true。
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
    static auto clamp_offset(float offset, float delta_y, float step, float content_h, float viewport_h) -> float {
        const float max_off = std::max(0.0F, content_h - viewport_h);
        return std::clamp(offset - (delta_y * step), 0.0F, max_off);
    }

    /// @brief 垂直滚动 clamp 后的未消费余量（供消费方回传 `ScrollEvent::remaining_y`）：
    ///        `delta_y - 已吃掉量`。到顶/到底被夹掉的部分即余量（保留符号）；
    ///        `step <= 0` 视为不可滚，全量退为余量。
    static auto remaining_offset(float before, float after, float delta_y, float step) -> float {
        if (step <= 0.0F) {
            return delta_y;
        }
        return delta_y - ((before - after) / step);
    }

    /// @brief 吸附收位目标：当前 offset 在 `snap` 下最近的条目对齐点（夹到 [0, max_offset]）。
    ///        吸附关闭（非分页且 extent<=0，或分页但视口未定）时退化为普通夹取。
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
                const double k = std::lround((offset + (viewport_h / 2.0F) - (ext / 2.0F)) / ext);
                candidate = static_cast<float>(k * ext) + (ext / 2.0F) - (viewport_h / 2.0F);
                break;
            }
            case ScrollSnapAlignment::End: {
                // 条目 k 的后沿 (k+1)·ext 贴视口后沿：offset = (k+1)·ext − viewport
                const double k = std::lround((offset + viewport_h) / ext);
                candidate = static_cast<float>(k * ext) - viewport_h;
                break;
            }
        }
        // 越界条目（内容末尾不足一个周期）夹到 max_off——末端可达性优先于严格对齐。
        return std::clamp(candidate, 0.0F, max_off);
    }
};

}  // namespace aurora
