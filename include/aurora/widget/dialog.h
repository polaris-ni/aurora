#pragma once

#include <functional>
#include <string>

#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 对话框控件。
 *
 * 模态覆盖层：显示/隐藏由 `open_` 控制，关闭时不渲染。
 * 内容居中显示在半透明遮罩之上。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Dialog : public Container {
  public:
    Dialog() = default;
    explicit Dialog(Node content) { children_.push_back(std::move(content)); }

    [[nodiscard]] auto type_name() const -> const char * override { return "Dialog"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Dialog",
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "是否显示",
                     .json_type = "boolean"},
                },
            .events = {"on_close"},
            .children_policy = "single",
            .examples = {R"(au::Dialog(au::alert("Title", "Message")))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 显示对话框。
    auto show() -> void { open_ = true; }

    /// @brief 关闭对话框。
    auto close() -> void {
        open_ = false;
        if (on_close_) {
            on_close_();
        }
    }

    /// @brief 是否打开。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 设置关闭回调。
    auto set_on_close(std::function<void()> cb) -> void { on_close_ = std::move(cb); }

    /// @brief 设置内容。
    auto set_content(Node content) -> void {
        children_.clear();
        children_.push_back(std::move(content));
    }

    // Widget 接口
  protected:
    [[nodiscard]] auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (!open_) {
            return Size{.width = 0.0F, .height = 0.0F};
        }
        // 对话框占满父约束
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 400.0F, .height = 300.0F};
        }
        // 布局子节点（居中）
        for (auto &child : children_) {
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            inner.max = Size{.width = self.width * 0.8F, .height = self.height * 0.8F};
            child->layout(inner, ctx);
        }
        return self;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (!open_) {
            return;
        }
        // 半透明遮罩
        p.fill_rect(bounds, Color(0, 0, 0, 128));
        // 内容居中
        if (!children_.empty()) {
            const Size cs = children_[0]->size();
            const float x = bounds.origin.x + ((bounds.size.width - cs.width) * 0.5F);
            const float y = bounds.origin.y + ((bounds.size.height - cs.height) * 0.5F);
            const Rect content_box{.origin = Point{.x = x, .y = y}, .size = cs};
            children_[0]->paint(p, content_box, ctx);
        }
    }

  private:
    bool open_ = false;
    std::function<void()> on_close_;
};

// ---------- 便捷工厂 ----------

/// @brief 构建警告对话框（标题 + 消息 + 确定按钮）。
[[nodiscard]] inline auto alert(const std::string &title, const std::string &message, std::function<void()> on_ok = {})
    -> Node {
    auto ok_btn = Button(ButtonProps{.label = "OK"});
    if (on_ok) {
        ok_btn.set_on_click(std::move(on_ok));
    }

    auto content = Column(ColumnProps{
        .children =
            {
                std::move(Text(title).font_size(18).bold()),
                std::move(Text(message).font_size(14)),
                std::move(ok_btn),
            },
        .gap = 12,
    });
    return {std::move(content)};
}

/// @brief 构建确认对话框（标题 + 消息 + 确定/取消按钮）。
[[nodiscard]] inline auto confirm(const std::string &title, const std::string &message,
                                  const std::function<void(bool)> &on_result = {}) -> Node {
    auto yes_btn = Button(ButtonProps{.label = "Yes"});
    auto no_btn = Button(ButtonProps{.label = "No"});
    if (on_result) {
        yes_btn.set_on_click([on_result]() -> void { on_result(true); });
        no_btn.set_on_click([on_result]() -> void { on_result(false); });
    }

    auto buttons = Row(RowProps{
        .children = {std::move(yes_btn), std::move(no_btn)},
        .gap = 8,
    });

    auto content = Column(ColumnProps{
        .children =
            {
                std::move(Text(title).font_size(18).bold()),
                std::move(Text(message).font_size(14)),
                std::move(buttons),
            },
        .gap = 12,
    });
    return {std::move(content)};
}

}  // namespace aurora
