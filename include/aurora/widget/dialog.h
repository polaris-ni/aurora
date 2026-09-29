#pragma once

#include <functional>
#include <string>

#include "aurora/event/focus.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 对话框控件。
///
/// 模态覆盖层：显示/隐藏由 `open_` 控制，关闭时不渲染。
/// 内容居中显示在半透明遮罩之上。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Dialog : public Container {
  public:
    Dialog() = default;
    /// @brief 以内容节点构造（对话框初始为关闭态，需 `show()` 才渲染）。
    /// @param content 置于遮罩中央的内容根节点，收为本类唯一子节点。
    explicit Dialog(Node content) { children_.push_back(std::move(content)); }

    /// @brief 控件类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Dialog"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Dialog"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "Dialog" 的描述表：属性 `open`、事件 `on_close`、子节点策略 "single"。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Dialog",
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Visible",
                     .json_type = "boolean"},
                },
            .events = {"on_close"},
            .children_policy = "single",
            .examples = {R"(au::Dialog(au::alert("Title", "Message")))"},
        };
    }
    /// @brief 实例级自描述：对话框无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：`open` 是普通 bool 字段，无响应式属性可登记。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 显示对话框：压入焦点作用域——Tab 焦点关在本层内、焦点自动移入首个可聚焦控件。
    /// 派发回调（点击处理器等）内调用时可取到当前 FocusManager；无焦点管理器时降级为仅置位。
    auto show() -> void {
        if (!open_ && current_focus_manager() != nullptr) {
            current_focus_manager()->push_scope(this);
        }
        open_ = true;
    }

    /// @brief 关闭对话框：弹出焦点作用域并恢复打开前焦点。
    auto close() -> void {
        if (open_ && current_focus_manager() != nullptr) {
            current_focus_manager()->pop_scope();
        }
        open_ = false;
        if (on_close_) {
            on_close_();
        }
    }

    /// @brief 是否打开。
    /// @return `show()` 之后、`close()` 之前为 true（关闭态不渲染、布局返回零尺寸）。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 设置关闭回调（`close()` 末尾同步调用）。
    /// @param cb 关闭时执行的闭包；传空 `std::function` 即清除既有回调。
    auto set_on_close(std::function<void()> cb) -> void { on_close_ = std::move(cb); }

    /// @brief 设置内容（替换而非追加：先清空既有子节点）。
    /// @param content 置于遮罩中央的内容根节点。
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
/// @param title 标题文本（18 号加粗）。
/// @param message 消息正文（14 号）。
/// @param on_ok 点击 OK 按钮时同步调用的闭包；传空则按钮不挂回调。
/// @return 内容节点（垂直 Column，间距 12：标题 + 消息 + 标签为 "OK" 的按钮），可直接交给 `Dialog` 承载。
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
/// @param title 标题文本（18 号加粗）。
/// @param message 消息正文（14 号）。
/// @param on_result 结果回调：点 "Yes" 传 true、点 "No" 传 false；传空则两个按钮都不挂回调。
/// @return 内容节点（垂直 Column，间距 12：标题 + 消息 + 一行 "Yes"/"No" 按钮，行内间距 8）。
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
