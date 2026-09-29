#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Toast 显示位置。
enum class ToastPosition : std::uint8_t {
    Bottom,  ///< 底部居中（默认）
    Top,  ///< 顶部居中
};

/// @brief Toast/SnackBar 通知宿主：包裹基础内容并在其上叠加自动消失的通知。
///
/// `show(text, duration_ms)` 入队；每帧 `tick` 驱动过期出队；同屏最多显示
/// `AURORA_MAX_VISIBLE` 条（其余排队）。通知不拦截命中（点击穿透）。
///
/// 对标 Flutter `SnackBar`/`ScaffoldMessenger`、Android Toast。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// 本行隐式生成的拷贝/移动构造逐成员复制 `std::deque<Entry>`（每条通知含 std::string），容器与字符串
/// 拷贝即可能 bad_alloc，被本检查判「不应抛出」；该隐式特成员按 [except.spec] 本就 potentially-throwing，
/// 抛出沿栈交给复制方 —— 与 std::function 同族的系统性假告警面（见 .clang-tidy 记录）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class ToastHost : public SingleChild {
  public:
    /// @brief 默认构造：无基础内容（仅通知叠加层）。
    ToastHost() = default;
    /// @brief 以基础内容构造宿主，并开启 gesture-tick 驱动（过期逻辑依赖帧驱动）。
    /// @param content 被包裹的基础内容节点（通知绘制在其上层）。
    explicit ToastHost(Node content) : SingleChild(std::move(content)) {
        // Toast 过期由每帧 tick 驱动（见 tick_gestures），须开启 gesture-tick 驱动，
        // 否则 Widget::tick 在 !needs_gesture_tick_ 时直接返回，过期逻辑永不运行。
        needs_gesture_tick_ = true;
    }

    /// @brief 类型名字符串 "ToastHost"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override { return "ToastHost"; }

    /// @brief 静态属性描述符：position 属性与单孩子策略声明。
    /// @return 名为 "ToastHost" 的完整描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ToastHost",
            .properties =
                {
                    {.name = "position",
                     .type = "ToastPosition",
                     .default_value = "Bottom",
                     .required = false,
                     .note = "通知显示位置"},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::ToastHost(content) /* host.show(\"Saved!\", 3000) */"},
        };
    }
    /// @brief 运行时自描述：转发到静态描述符。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 无信号依赖：通知队列为运行时内部状态。
    /// @param out 信号输出向量；本控件不向其写入任何信号。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 入队一条通知（duration_ms 显示时长，默认 3 秒）。
    /// @param text 通知正文文本。
    /// @param duration_ms 显示时长（毫秒）；负值按 0 处理。
    auto show(std::string text, float duration_ms = 3000.0F) -> void {
        queue_.push_back(Entry{.text = std::move(text),
                               .duration_ms = duration_ms < 0.0F ? 0.0F : duration_ms});  // 时长夹到非负后入队尾部
        mark_needs_paint();  // 新通知立即参与叠加绘制
    }

    /// @brief 当前可见 + 排队中的通知总数。
    /// @return 通知队列长度（含尚未可见的候补）。
    [[nodiscard]] auto pending_count() const -> std::size_t { return queue_.size(); }

    /// @brief 当前可见的通知文本列表（最多 AURORA_MAX_VISIBLE 条）。
    /// @return 队列前缀（即入队顺序）的正文文本列表，条数不超过 AURORA_MAX_VISIBLE；队空时为空列表。
    [[nodiscard]] auto visible_toasts() const -> std::vector<std::string> {
        std::vector<std::string> out;  // 结果列表（按队列前缀=可见顺序）
        const std::size_t n = std::min(queue_.size(), AURORA_MAX_VISIBLE);
        out.reserve(n);  // 预分配避免循环内扩容
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(queue_[i].text);
        }
        return out;
    }

    /// @brief 立即清空所有通知。
    auto clear() -> void {
        queue_.clear();
        mark_needs_paint();
    }

    /// @brief 设置显示位置（链式）。
    /// @param pos 顶部或底部居中。
    /// @return 自身引用，便于链式调用。
    auto set_position(ToastPosition pos) -> ToastHost & {
        position_ = pos;
        return *this;
    }

    /// @brief 序列化：通用属性 + position（"bottom"/"top"）。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性（width/height/show 等）
        props.set("position", position_ == ToastPosition::Bottom ? "bottom" : "top");
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 240.0F};
        }
        if (child_) {
            const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
        // 叠加通知（自底/自顶堆叠）
        Font f;
        f.size_pt = 13.0F;
        const std::size_t n = std::min(queue_.size(), AURORA_MAX_VISIBLE);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string &text = queue_[i].text;
            const float tw = render::FontEngine::measure_width(text, f) + (AURORA_PADDING * 2.0F);
            constexpr float th = AURORA_TOAST_HEIGHT;
            const float x = bounds.origin.x + ((bounds.size.width - tw) * 0.5F);
            const float stack_off = static_cast<float>(i) * (th + 8.0F);
            const float y = position_ == ToastPosition::Bottom
                                ? bounds.origin.y + bounds.size.height - AURORA_MARGIN - th - stack_off
                                : bounds.origin.y + AURORA_MARGIN + stack_off;
            const Rect box{.origin = Point{.x = x, .y = y}, .size = Size{.width = tw, .height = th}};
            p.draw_shadow(box, 0.0F, 2.0F, 6.0F, Color{0, 0, 0, 40});
            p.fill_rect(box, Color{50, 50, 54, 235});
            const Rect text_box{.origin = Point{.x = x + AURORA_PADDING, .y = y + 7.0F},
                                .size = Size{.width = tw - (AURORA_PADDING * 2.0F), .height = th - 14.0F}};
            p.draw_text(text_box, text, f, Color{255, 255, 255, 255});
        }
    }

    /// @brief 帧驱动：可见通知按 dt 递减寿命，过期出队（排队中的候补顶上）。
    /// @param now 本帧时刻（单调时钟）：作首次可见的计时起点，并折算各可见通知的已显示时长。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        SingleChild::tick_gestures(now);
        const std::size_t n = std::min(queue_.size(), AURORA_MAX_VISIBLE);
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            Entry &e = queue_[i];
            if (!e.shown_at.has_value()) {
                e.shown_at = now;  // 首次可见开始计时
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - e.shown_at.value()).count();
            if (static_cast<float>(ms) >= e.duration_ms) {
                e.expired = true;
                changed = true;
            }
        }
        if (changed) {
            queue_.erase(std::ranges::remove_if(queue_, [](const Entry &e) -> bool { return e.expired; }).begin(),
                         queue_.end());
            mark_needs_paint();
        }
    }

  private:
    static constexpr std::size_t AURORA_MAX_VISIBLE = 3;  ///< 同屏最多通知数
    static constexpr float AURORA_TOAST_HEIGHT = 34.0F;  ///< 单条通知高度(dp)
    static constexpr float AURORA_MARGIN = 24.0F;  ///< 距边缘间距(dp)
    static constexpr float AURORA_PADDING = 14.0F;  ///< 文本左右内边距(dp)

    struct Entry {
        std::string text;  ///< 通知正文文本
        float duration_ms = 3000.0F;  ///< 显示时长（毫秒，入队时已夹到非负）
        std::optional<std::chrono::steady_clock::time_point> shown_at;  ///< 首次可见时刻（空 = 尚未开始计时）
        bool expired = false;  ///< 寿命已尽，待本轮出队清理
    };

    std::deque<Entry> queue_;  ///< 通知队列：前 AURORA_MAX_VISIBLE 条可见，其余排队
    ToastPosition position_ = ToastPosition::Bottom;  ///< 显示位置（默认底部居中）
};

}  // namespace aurora
