#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/commands.h"
#include "aurora/core/font.h"
#include "aurora/core/log.h"
#include "aurora/core/types.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 命令面板：模态居中浮层，按关键字模糊检索并执行命令。
 *
 * 视觉沿用库既有模态范式（对照 `Dialog`）：半透明遮罩 + 居中圆角卡片；顶部搜索框、中部结果列表
 * （当前项高亮）、底部选中命令的快捷键提示。输入即时过滤，结果按（得分降序, 标题升序）排序。
 *
 * **键位与焦点**：面板打开时把焦点作用域压到自身（`FocusManager::push_scope`），子树内唯一的
 * 可聚焦控件是搜索框——由此左右方向键仍落到搜索框做光标移动，而上下方向键不会引发焦点跳转。
 * 由于派发器把 Enter/Space 归为「激活键」并在焦点路由前消费，面板按下述分工接管：
 * 搜索框的 `on_submit` 承担 Enter（执行选中项），Esc/↑/↓ 经打开期临时注册的快捷键绑定接管
 * （依赖 `CommandRegistry` 已 `bind_shortcuts`；未绑定时这几键不可用，面板以 WARN 提示）。
 * Space 只经文本输入落字，不会触发执行。
 *
 * @note Thread: main-thread only
 * @note Side-effects: paints; `open`/`close` 修改焦点作用域与快捷键表
 * @note Rebuildable: no（依赖外部 `CommandRegistry` 数据源）
 */
class CommandPalette : public Container {
  public:
    /// @param commands 命令注册表（非拥有，须比面板长寿）；可为 nullptr，此时面板为空列表。
    explicit CommandPalette(CommandRegistry *commands = nullptr) : commands_(commands) {
        auto field = std::make_shared<TextInput>();
        field->set_placeholder("Type a command...");
        field->set_on_changed([this](const std::string &value) -> void {
            query_ = value;
            selected_ = 0;
            rebuild_results();
            mark_needs_paint();
        });
        field->set_on_submit([this](const std::string & /*value*/) -> void { (void)execute_selected(); });
        field_raw_ = field.get();
        children_.emplace_back(std::move(field));
        set_focusable(false);  // 面板本身不参与焦点序：子树内仅搜索框可聚焦（见类注释）
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "CommandPalette"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "CommandPalette",
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Whether the command palette is shown",
                     .json_type = "boolean"},
                    {.name = "max_results",
                     .type = "int",
                     .default_value = "50",
                     .required = false,
                     .note = "Maximum number of listed results",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "1"},
                },
            .events = {"on_execute", "on_close"},
            .children_policy = "none",
            .examples = {R"(auto palette = au::CommandPalette(&app.commands()); palette.open();)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 后置注入命令注册表；打开中调用会重建键位绑定。
    auto set_commands(CommandRegistry *commands) -> void {
        if (open_) {
            remove_key_bindings();
        }
        commands_ = commands;
        if (open_) {
            selected_ = 0;
            rebuild_results();
            install_key_bindings();
        }
    }

    /// @brief 结果列表上限（超出部分截断）；默认 50。
    auto set_max_results(std::size_t n) -> void {
        max_results_ = n == 0 ? 1 : n;
        rebuild_results();
        mark_needs_layout();
    }

    /// @brief 执行回调：在 `invoke` 之后触发，参数为命令 id（宿主可接管副作用）。
    auto set_on_execute(std::function<void(const std::string &)> cb) -> void { on_execute_ = std::move(cb); }

    /// @brief 关闭回调：Esc、点击遮罩、执行后均触发。
    auto set_on_close(std::function<void()> cb) -> void { on_close_ = std::move(cb); }

    /// @brief 打开面板：压入焦点作用域、清空查询、重建结果并安装键位绑定（已打开时为 no-op）。
    auto open() -> void {
        if (open_) {
            return;
        }
        if (field_raw_ != nullptr) {
            field_raw_->set_value("");
        }
        query_.clear();
        selected_ = 0;
        rebuild_results();
        if (current_focus_manager() != nullptr) {
            current_focus_manager()->push_scope(this);  // 焦点自动移入子树内首个可聚焦控件（搜索框）
        }
        install_key_bindings();
        if (commands_ == nullptr) {
            AURORA_LOG_WARN("widget", "CommandPalette opened without a CommandRegistry: no commands to show.");
        } else if (commands_->shortcuts() == nullptr) {
            AURORA_LOG_WARN("widget",
                            "CommandPalette has no ShortcutRegistry bound: Esc/Up/Down navigation disabled. "
                            "Call CommandRegistry::bind_shortcuts() first.");
        }
        open_ = true;
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 关闭面板：卸载键位绑定、弹出焦点作用域并触发关闭回调（未打开时为 no-op）。
    auto close() -> void {
        if (!open_) {
            return;
        }
        remove_key_bindings();
        if (current_focus_manager() != nullptr) {
            current_focus_manager()->pop_scope();
        }
        open_ = false;
        mark_needs_layout();
        mark_needs_paint();
        if (on_close_) {
            on_close_();
        }
    }

    auto toggle() -> void {
        if (open_) {
            close();
        } else {
            open();
        }
    }

    [[nodiscard]] auto is_open() const -> bool { return open_; }
    [[nodiscard]] auto query() const -> std::string { return query_; }

    /// @brief 当前过滤结果（按得分排序；`rebuild_results` 之后稳定）。
    [[nodiscard]] auto results() const -> const std::vector<const Command *> & { return results_; }

    [[nodiscard]] auto selected_index() const -> std::size_t { return selected_; }

    /// @brief 当前选中命令的 id；无结果时为空串。
    [[nodiscard]] auto selected_id() const -> std::string {
        if (selected_ >= results_.size()) {
            return {};
        }
        return results_[selected_]->id;
    }

    /// @brief 执行当前选中项并关闭面板；返回命令是否真的被执行（未选中/未启用/无 action = false）。
    auto execute_selected() -> bool {
        if (selected_ >= results_.size()) {
            return false;
        }
        const std::string id = results_[selected_]->id;
        const bool invoked = commands_ != nullptr && commands_->invoke(id);
        close();
        if (on_execute_) {
            on_execute_(id);
        }
        return invoked;
    }

    /// @brief 移动选中项（±1，越界环绕）；无结果时为 no-op。
    auto move_selection(int delta) -> void {
        if (results_.empty()) {
            return;
        }
        const auto n = static_cast<long long>(results_.size());
        long long next = static_cast<long long>(selected_) + delta;
        if (next < 0) {
            next = n - 1;
        } else if (next >= n) {
            next = 0;
        }
        selected_ = static_cast<std::size_t>(next);
        mark_needs_paint();
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!open_) {
            return;
        }
        // 模态：卡片内外的点击都不再外传。
        e.is_handled = true;
        const int row = row_index_at(e.local_position);
        if (row < 0) {
            if (e.action == MouseAction::Press && !card_box_.contains(e.local_position)) {
                close();  // 点击遮罩关闭
            }
            return;
        }
        if (e.action == MouseAction::Press) {
            selected_ = static_cast<std::size_t>(row);
            mark_needs_paint();
        } else if (e.action == MouseAction::Release && std::cmp_equal(row, selected_)) {
            (void)execute_selected();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (!open_) {
            card_box_ = Rect{};
            return Size{.width = 0.0F, .height = 0.0F};
        }
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = AURORA_MAX_CARD_WIDTH + (AURORA_MASK_MARGIN * 2.0F), .height = 480.0F};
        }
        const float card_w =
            std::clamp(self.width - (AURORA_MASK_MARGIN * 2.0F), AURORA_MIN_CARD_WIDTH, AURORA_MAX_CARD_WIDTH);
        const float list_h =
            results_.empty() ? AURORA_ROW_HEIGHT : static_cast<float>(results_.size()) * AURORA_ROW_HEIGHT;
        const float card_h = AURORA_CARD_PADDING + AURORA_FIELD_HEIGHT + AURORA_GAP + list_h + AURORA_FOOTER_HEIGHT +
                             AURORA_CARD_PADDING;
        card_box_ = Rect{.origin = Point{.x = (self.width - card_w) * 0.5F, .y = (self.height - card_h) * 0.5F},
                         .size = Size{.width = card_w, .height = card_h}};
        if (field_raw_ != nullptr) {
            const Size field_size{.width = card_w - (AURORA_CARD_PADDING * 2.0F), .height = AURORA_FIELD_HEIGHT};
            (void)field_raw_->layout(Constraints{.min = field_size, .max = field_size}, ctx);
        }
        return self;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (!open_) {
            return;
        }
        const Theme &theme = inherit_theme(ctx);
        const Rect card = to_global(card_box_, bounds);

        // ① 半透明遮罩（与 Dialog 同款，视觉统一）
        p.fill_rect(bounds, Color{0, 0, 0, 128});

        // ② 居中圆角卡片
        p.fill_rounded_rect(card, AURORA_CORNER_RADIUS, theme.background);

        // ③ 搜索框（子控件自绘）
        if (field_raw_ != nullptr) {
            field_raw_->paint(p, to_global(field_box(), bounds), ctx);
        }

        // ④ 分隔线
        const float divider_y = card.origin.y + AURORA_CARD_PADDING + AURORA_FIELD_HEIGHT + (AURORA_GAP * 0.5F);
        p.fill_rect(Rect{.origin = Point{.x = card.origin.x, .y = divider_y},
                         .size = Size{.width = card.size.width, .height = 1.0F}},
                    Color{0, 0, 0, 40});

        // ⑤ 结果列表 / 空态
        const Font row_font{.size_pt = AURORA_ROW_FONT_SIZE};
        const float list_top = card.origin.y + AURORA_CARD_PADDING + AURORA_FIELD_HEIGHT + AURORA_GAP;
        constexpr Color muted{150, 150, 152, 255};
        if (results_.empty()) {
            p.draw_text(Rect{.origin = Point{.x = card.origin.x + AURORA_CARD_PADDING, .y = list_top},
                             .size = Size{.width = card.size.width - (AURORA_CARD_PADDING * 2.0F),
                                          .height = AURORA_ROW_HEIGHT}},
                        "No matching commands", row_font, muted);
        } else {
            const float text_h = render::FontEngine::measure_height(row_font);
            for (std::size_t i = 0; i < results_.size(); ++i) {
                const float row_y = list_top + (static_cast<float>(i) * AURORA_ROW_HEIGHT);
                const bool is_selected = (i == selected_);
                if (is_selected) {
                    p.fill_rounded_rect(
                        Rect{.origin = Point{.x = card.origin.x + (AURORA_CARD_PADDING * 0.5F), .y = row_y},
                             .size = Size{.width = card.size.width - AURORA_CARD_PADDING, .height = AURORA_ROW_HEIGHT}},
                        AURORA_ROW_CORNER_RADIUS, theme.primary.with_alpha(48));
                }
                const Command &cmd = *results_[i];
                const float text_y = row_y + ((AURORA_ROW_HEIGHT - text_h) * 0.5F);
                const Color title_color = cmd.action ? theme.text : muted;
                p.draw_text(
                    Rect{.origin = Point{.x = card.origin.x + AURORA_CARD_PADDING, .y = text_y},
                         .size = Size{.width = (card.size.width * 0.7F) - AURORA_CARD_PADDING, .height = text_h}},
                    cmd.title, row_font, title_color);
                if (!cmd.category.empty()) {
                    p.draw_text(Rect{.origin = Point{.x = card.origin.x + (card.size.width * 0.7F), .y = text_y},
                                     .size = Size{.width = (card.size.width * 0.3F) - (AURORA_CARD_PADDING * 2.0F),
                                                  .height = text_h}},
                                cmd.category, row_font, muted);
                }
            }
        }

        // ⑥ 底部：选中命令的快捷键提示
        const std::string hint = selected_shortcut_text();
        if (!hint.empty()) {
            p.draw_text(Rect{.origin = Point{.x = card.origin.x + AURORA_CARD_PADDING,
                                             .y = card.origin.y + card.size.height - AURORA_FOOTER_HEIGHT},
                             .size = Size{.width = card.size.width - (AURORA_CARD_PADDING * 2.0F),
                                          .height = AURORA_FOOTER_HEIGHT - AURORA_CARD_PADDING}},
                        hint, row_font, muted);
        }
    }

    /// @brief 模态命中：打开时本控件占满命中（吞掉点击，点击遮罩即关闭）；关闭时不占位。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        (void)local;
        (void)bounds;
        (void)ctx;
        return open_ ? this : nullptr;
    }

  private:
    /// @brief 搜索框在容器局部坐标系中的矩形（不含 bounds.origin）。
    [[nodiscard]] auto field_box() const -> Rect {
        return Rect{
            .origin =
                Point{.x = card_box_.origin.x + AURORA_CARD_PADDING, .y = card_box_.origin.y + AURORA_CARD_PADDING},
            .size = Size{.width = card_box_.size.width - (AURORA_CARD_PADDING * 2.0F), .height = AURORA_FIELD_HEIGHT}};
    }

    [[nodiscard]] static auto to_global(const Rect &local, const Rect &bounds) -> Rect {
        return Rect{.origin = Point{.x = bounds.origin.x + local.origin.x, .y = bounds.origin.y + local.origin.y},
                    .size = local.size};
    }

    /// @brief 容器局部坐标命中的结果行下标；未命中返回 -1。
    [[nodiscard]] auto row_index_at(const Point &local) const -> int {
        if (local.x < card_box_.origin.x || local.x > card_box_.origin.x + card_box_.size.width) {
            return -1;
        }
        const float list_top = card_box_.origin.y + AURORA_CARD_PADDING + AURORA_FIELD_HEIGHT + AURORA_GAP;
        const float dy = local.y - list_top;
        if (dy < 0.0F) {
            return -1;
        }
        const auto index = static_cast<std::size_t>(dy / AURORA_ROW_HEIGHT);
        if (index >= results_.size()) {
            return -1;
        }
        return static_cast<int>(index);
    }

    [[nodiscard]] auto selected_shortcut_text() const -> std::string {
        if (selected_ >= results_.size()) {
            return {};
        }
        const Command &cmd = *results_[selected_];
        return cmd.default_binding.has_value() ? cmd.default_binding->to_string() : std::string{};
    }

    /// @brief 重建过滤结果（按当前查询与启用条件；超出上限则截断）。
    auto rebuild_results() -> void {
        results_.clear();
        if (commands_ != nullptr) {
            results_ = commands_->search(query_, true);
            if (results_.size() > max_results_) {
                results_.resize(max_results_);
            }
        }
        if (selected_ >= results_.size()) {
            selected_ = results_.empty() ? 0 : results_.size() - 1;
        }
    }

    /// @brief 安装打开期临时键位（Esc / ↑ / ↓）；无可用快捷键表时静默跳过。
    auto install_key_bindings() -> void {
        ShortcutRegistry *sr = commands_ != nullptr ? commands_->shortcuts() : nullptr;
        if (sr == nullptr || !key_bindings_.empty()) {
            return;
        }
        key_bindings_.push_back(sr->add(
            KeyCombo{KeyCode::Escape}, [this]() -> void { close(); }, ShortcutScope::Global, "Close command palette"));
        key_bindings_.push_back(sr->add(
            KeyCombo{KeyCode::ArrowUp}, [this]() -> void { move_selection(-1); }, ShortcutScope::Global,
            "Select previous command"));
        key_bindings_.push_back(sr->add(
            KeyCombo{KeyCode::ArrowDown}, [this]() -> void { move_selection(1); }, ShortcutScope::Global,
            "Select next command"));
    }

    auto remove_key_bindings() -> void {
        ShortcutRegistry *sr = commands_ != nullptr ? commands_->shortcuts() : nullptr;
        if (sr != nullptr) {
            for (const int id : key_bindings_) {
                sr->remove(id);
            }
        }
        key_bindings_.clear();
    }

    static constexpr float AURORA_MASK_MARGIN = 24.0F;
    static constexpr float AURORA_MIN_CARD_WIDTH = 280.0F;
    static constexpr float AURORA_MAX_CARD_WIDTH = 560.0F;
    static constexpr float AURORA_CARD_PADDING = 12.0F;
    static constexpr float AURORA_FIELD_HEIGHT = 36.0F;
    static constexpr float AURORA_ROW_HEIGHT = 30.0F;
    static constexpr float AURORA_FOOTER_HEIGHT = 24.0F;
    static constexpr float AURORA_GAP = 8.0F;
    static constexpr float AURORA_CORNER_RADIUS = 8.0F;
    static constexpr float AURORA_ROW_CORNER_RADIUS = 4.0F;
    static constexpr float AURORA_ROW_FONT_SIZE = 14.0F;

    CommandRegistry *commands_ = nullptr;  ///< 非拥有
    TextInput *field_raw_ = nullptr;  ///< 子搜索框的非拥有视图
    std::vector<const Command *> results_;  ///< 当前过滤结果（按得分排序）
    std::size_t selected_ = 0;  ///< 当前选中下标
    std::size_t max_results_ = 50;  ///< 结果上限
    std::string query_;  ///< 当前查询串
    bool open_ = false;  ///< 是否显示
    Rect card_box_ = {};  ///< 卡片矩形（容器局部坐标，布局期计算）
    std::vector<int> key_bindings_;  ///< 打开期临时快捷键绑定 id
    std::function<void(const std::string &)> on_execute_;  ///< 执行回调
    std::function<void()> on_close_;  ///< 关闭回调
};

}  // namespace aurora
