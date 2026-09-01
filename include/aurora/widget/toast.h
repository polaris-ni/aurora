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
    Bottom, ///< 底部居中（默认）
    Top,    ///< 顶部居中
};

/**
 * @brief Toast/SnackBar 通知宿主：包裹基础内容并在其上叠加自动消失的通知。
 *
 * `show(text, duration_ms)` 入队；每帧 `tick` 驱动过期出队；同屏最多显示
 * `m_max_visible` 条（其余排队）。通知不拦截命中（点击穿透）。
 *
 * 对标 Flutter `SnackBar`/`ScaffoldMessenger`、Android Toast。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ToastHost : public SingleChild {
  public:
    ToastHost() = default;
    explicit ToastHost(Node content) : SingleChild(std::move(content)) {
        // Toast 过期由每帧 tick 驱动（见 tick_gestures），须开启 gesture-tick 驱动，
        // 否则 Widget::tick 在 !m_needs_gesture_tick 时直接返回，过期逻辑永不运行。
        m_needs_gesture_tick = true;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ToastHost"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ToastHost",
            .properties = {
                { .name = "position", .type = "ToastPosition", .default_value = "Bottom", .required = false,
                  .note = "通知显示位置" },
            },
            .events = {},
            .children_policy = "single",
            .examples = { "au::ToastHost(content) /* host.show(\"Saved!\", 3000) */" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 入队一条通知（duration_ms 显示时长，默认 3 秒）。
    auto show(std::string text, float duration_ms = 3000.0f) -> void {
        m_queue.push_back(Entry{ .text = std::move(text), .duration_ms = duration_ms < 0.0f ? 0.0f : duration_ms });
        mark_needs_paint();
    }

    /// @brief 当前可见 + 排队中的通知总数。
    [[nodiscard]] auto pending_count() const -> std::size_t { return m_queue.size(); }

    /// @brief 当前可见的通知文本列表（最多 m_max_visible 条）。
    [[nodiscard]] auto visible_toasts() const -> std::vector<std::string> {
        std::vector<std::string> out;
        const std::size_t n = std::min(m_queue.size(), m_max_visible);
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(m_queue[i].text);
        }
        return out;
    }

    /// @brief 立即清空所有通知。
    auto clear() -> void {
        m_queue.clear();
        mark_needs_paint();
    }

    /// @brief 设置显示位置（链式）。
    auto set_position(ToastPosition pos) -> ToastHost & {
        m_position = pos;
        return *this;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["position"] = m_position == ToastPosition::Bottom ? "bottom" : "top";
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 320.0f, .height = 240.0f };
        }
        if (m_child) {
            const Constraints inner{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = self };
            m_child.widget().layout(inner, ctx);
            m_child.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = self });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (m_child) {
            m_child.widget().paint(p, bounds, ctx);
        }
        // 叠加通知（自底/自顶堆叠）
        Font f;
        f.size_pt = 13.0f;
        const std::size_t n = std::min(m_queue.size(), m_max_visible);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string &text = m_queue[i].text;
            const float tw = render::FontEngine::measure_width(text, f) + (m_pad * 2.0f);
            constexpr float th = m_toast_height;
            const float x = bounds.origin.x + ((bounds.size.width - tw) * 0.5f);
            const float stack_off = static_cast<float>(i) * (th + 8.0f);
            const float y = m_position == ToastPosition::Bottom
                                ? bounds.origin.y + bounds.size.height - m_margin - th - stack_off
                                : bounds.origin.y + m_margin + stack_off;
            const Rect box{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = tw, .height = th } };
            p.draw_shadow(box, 0.0f, 2.0f, 6.0f, Color{ 0, 0, 0, 40 });
            p.fill_rect(box, Color{ 50, 50, 54, 235 });
            const Rect text_box{ .origin = Point{ .x = x + m_pad, .y = y + 7.0f },
                                 .size = Size{ .width = tw - (m_pad * 2.0f), .height = th - 14.0f } };
            p.draw_text(text_box, text, f, Color{ 255, 255, 255, 255 });
        }
    }

    /// @brief 帧驱动：可见通知按 dt 递减寿命，过期出队（排队中的候补顶上）。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        SingleChild::tick_gestures(now);
        const std::size_t n = std::min(m_queue.size(), m_max_visible);
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            Entry &e = m_queue[i];
            if (!e.shown_at.has_value()) {
                e.shown_at = now; // 首次可见开始计时
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - e.shown_at.value()).count();
            if (static_cast<float>(ms) >= e.duration_ms) {
                e.expired = true;
                changed = true;
            }
        }
        if (changed) {
            m_queue.erase(std::ranges::remove_if(m_queue, [](const Entry &e) -> bool { return e.expired; }).begin(),
                          m_queue.end());
            mark_needs_paint();
        }
    }

  private:
    static constexpr std::size_t m_max_visible = 3; ///< 同屏最多通知数
    static constexpr float m_toast_height = 34.0f;  ///< 单条通知高度(dp)
    static constexpr float m_margin = 24.0f;        ///< 距边缘间距(dp)
    static constexpr float m_pad = 14.0f;           ///< 文本左右内边距(dp)

    struct Entry {
        std::string text;
        float duration_ms = 3000.0f;
        std::optional<std::chrono::steady_clock::time_point> shown_at;
        bool expired = false;
    };

    std::deque<Entry> m_queue;
    ToastPosition m_position = ToastPosition::Bottom;
};

} // namespace aurora
