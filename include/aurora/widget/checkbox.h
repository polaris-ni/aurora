#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

/// @brief Aurora 顶层命名空间；本头在其中提供 Checkbox 复选控件与其绘制虚钩子。
namespace aurora {

/// @brief 复选框（叶控件）：勾选状态 `bool`，点击切换。
///
/// 支持两种值来源（与 TextInput 一致的响应式模式）：
/// - `Reactive<bool>`：内部持有状态，变化触发重绘；
/// - `Binding<bool>`：双向绑定到上游 `State<bool>`，写回上游并随其变化刷新。
/// 变化通过 `onChanged` 回调上报。
///
/// 视觉（对标 Material 3 Checkbox / Fluent CheckBox）：
/// - 未勾选：圆角方框描边（`border_color`），悬停时描边转激活色并铺淡色底；
/// - 勾选：激活色填充圆角方框 + 抗锯齿白色勾号 ✓（`check_color`），悬停/按下加深填充；
/// - 禁用（`set_enabled(false)`）：灰化并忽略点击。
/// `active_color` 未显式设置时自动跟随主题 `Theme::primary`（ThemeScope 换肤即生效）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Checkbox : public LeafWidget {
  public:
    /// @brief 默认构造：未勾选、外观取各成员默认值（勾选色跟随主题 `Theme::primary`）。
    Checkbox() = default;
    /// @brief 以内部响应式状态构造：勾选态存于 `checked`，变化自动触发重绘。
    /// @param checked 初始勾选态的 `Reactive<bool>`（内部持有语义）。
    /// @param on_changed 勾选态变化回调，可空；每次切换（点击/`set_value`/读屏 Toggle）后调用。
    explicit Checkbox(Reactive<bool> checked, std::function<void(bool)> on_changed = {})
        : value_(std::move(checked)), on_changed_(std::move(on_changed)) {}
    /// @brief 以上游信号双向绑定构造：勾选态写入回上游，并随上游变化刷新。
    /// @param binding 绑定到上游 `State<bool>` 的 `Binding<bool>`；非空时优先于内部 `value_`。
    /// @param on_changed 勾选态变化回调，可空；语义同另一构造重载。
    explicit Checkbox(Binding<bool> binding, std::function<void(bool)> on_changed = {})
        : binding_(std::move(binding)), value_(binding_.get()), on_changed_(std::move(on_changed)) {}

    /// @brief 设置勾选态变化回调（链式）。
    /// @param cb 新回调；可为空（清空）。每次切换后以最新勾选态调用。
    /// @return 自身引用，便于链式调用。
    auto set_on_changed(std::function<void(bool)> cb) -> Checkbox & {
        on_changed_ = std::move(cb);
        return *this;
    }

    /// @brief 设置勾选态填充色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c 勾选态填充色。
    /// @return 自身引用，便于链式调用。
    auto set_active_color(Color c) -> Checkbox & {
        active_color_ = c;
        return *this;
    }

    /// @brief 设置未勾选边框色（链式）。
    /// @param c 未勾选态描边色。
    /// @return 自身引用，便于链式调用。
    auto set_border_color(Color c) -> Checkbox & {
        border_color_ = c;
        return *this;
    }

    /// @brief 设置勾号颜色（链式；默认白色）。
    /// @param c 勾号 ✓ 笔画颜色。
    /// @return 自身引用，便于链式调用。
    auto set_check_color(Color c) -> Checkbox & {
        check_color_ = c;
        return *this;
    }

    /// @brief 设置方框边长 dp（链式）。
    /// @param s 方框边长（dp），同时作为布局返回的宽高基准。
    /// @return 自身引用，便于链式调用。
    auto set_size(float s) -> Checkbox & {
        size_ = s;
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；< 0 表示自动 = 边长 × 0.2）。
    /// @param r 圆角半径（dp）；负值走自动半径。
    /// @return 自身引用，便于链式调用。
    auto set_corner_radius(float r) -> Checkbox & {
        corner_radius_ = r;
        return *this;
    }

    /// @brief 设置未勾选描边宽 dp（链式）。
    /// @param w 描边宽度（dp）。
    /// @return 自身引用，便于链式调用。
    auto set_border_width(float w) -> Checkbox & {
        border_width_ = w;
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    /// @param v true 可交互；false 进入禁用态（统一灰化配色、吞掉点击不切换）。
    /// @return 自身引用，便于链式调用。
    auto set_enabled(bool v) -> Checkbox & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    /// @brief 是否处于可交互状态（对应 `set_enabled`）。
    /// @return 启用标志；false 表示禁用态（灰化并忽略点击）。
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 当前勾选态：有绑定时读上游信号，否则读内部 `Reactive<bool>`。
    /// @return 勾选状态；绑定态下等于 `binding_.get()`（实时读取，不缓存）。
    [[nodiscard]] auto value() const -> bool { return binding_.bound() ? binding_.get() : value_.get(); }

    /// @brief 写入勾选态：回写绑定（若有）、触发 `on_changed`、标脏重绘并发无障碍 ValueChanged。
    /// @param v 目标勾选态。
    auto set_value(bool v) -> void {
        if (binding_.bound()) {
            binding_.set(v);
        }
        value_ = v;
        if (on_changed_) {
            on_changed_(v);
        }
        mark_needs_paint();
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    /// @brief 收集本控件的响应式信号：内部 `value_`，绑定态下追加绑定目标信号。
    /// @param out 输出向量：挂载时由基类登记依赖，信号变化即触发本控件刷新。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&value_);
        if (binding_.bound()) {
            out.push_back(binding_.target());
        }
    }

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Checkbox"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Checkbox"; }

    /// @brief 无障碍值：勾选态的字面布尔串（`true` / `false`）。
    /// @return 当前勾选态对应的字面量字符串 "true" 或 "false"。
    /// @note 取字面值而非本地化文案：屏幕阅读器/自动化均可稳定解析，不受 locale 影响。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override { return value() ? "true" : "false"; }

    /// @brief 无障碍状态：复选语义的两位（checkable 恒 true、checked 取当前值）。
    /// @return 基类状态上覆写 checkable=true、checked=当前勾选态的结果。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_state() const -> AccessibilityState override {
        AccessibilityState s = Widget::accessibility_state();
        s.checkable = true;
        s.checked = value();
        s.disabled = !enabled();
        return s;
    }

    /// @brief 读屏 Toggle 动作：翻转勾选态（走 `set_value` 既有路径，`on_changed` 与
    ///        无障碍 ValueChanged 事件一并触发；与真实点击等价）。
    /// @param req 动作请求；仅 `AccessibilityAction::Toggle` 由本控件处理，其余下沉基类。
    /// @return 是否执行；true = 已翻转勾选态，false = 动作非 Toggle 且基类亦不支持。
    /// @note Side-effects: mutates state
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Toggle) {
            set_value(!value());
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 运行时自描述（规格附录 B）：Checkbox 的属性/事件/子节点策略元数据。
    /// @return 描述符：`name = "Checkbox"`，含 checked/各配色/尺寸/enabled 等属性键、事件
    ///         `on_changed`、`children_policy = "none"`（叶控件）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Checkbox",
            .properties =
                {
                    {.name = "checked",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "勾选状态",
                     .json_type = "boolean"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "勾选态填充色（缺省跟随主题 primary）",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{140,140,146,255}",
                     .required = false,
                     .note = "未勾选边框色",
                     .json_type = "array"},
                    {.name = "check_color",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "勾号颜色",
                     .json_type = "array"},
                    {.name = "size",
                     .type = "float",
                     .default_value = "20.0",
                     .required = false,
                     .note = "方框边长(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "-1.0",
                     .required = false,
                     .note = "圆角半径(dp)；<0 自动=边长×0.2",
                     .json_type = "number"},
                    {.name = "border_width",
                     .type = "float",
                     .default_value = "1.5",
                     .required = false,
                     .note = "未勾选描边宽(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否可交互（禁用灰化）",
                     .json_type = "boolean"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {"on_changed"},
            .children_policy = "none",
            .examples = {"au::Checkbox()"},
        };
    }
    /// @brief 运行时多态自描述入口：转发到 `describe_static()`。
    /// @return 与 `describe_static()` 相同的 Checkbox 描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 指针事件处理：Press 记按下态，Release 在按下过的前提下翻转勾选态。
    /// @param e 鼠标事件；本控件处理过的动作一律置 `is_handled`（禁用态也吞掉，避免冒泡到父级）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡触发父级点击），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            pressed_ = true;
            mark_needs_paint();  // 按下态视觉反馈
            e.is_handled = true;
        } else if (e.action == MouseAction::Release) {
            if (pressed_) {
                set_value(!value());
            }
            pressed_ = false;
            mark_needs_paint();
            e.is_handled = true;
        }
    }

    /// @brief 悬停反馈：未勾选描边高亮 + 淡色底；勾选填充加深。
    /// @param entered true 进入本控件命中链，false 离开。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        mark_needs_paint();
    }

    /// @brief 序列化自有属性到 props JSON（结构快照/工具链用）。
    /// @param props 输出对象：先由基类填通用属性，再写 checked、配色（`active_color` 仅在显式
    ///              设置时输出，以保留「跟随主题」语义）、尺寸与 enabled。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("checked", Json{value()});
        if (active_color_.has_value()) {
            props.set("active_color", color_to_json(*active_color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("border_color", color_to_json(border_color_));
        props.set("check_color", color_to_json(check_color_));
        props.set("size", size_);
        props.set("corner_radius", corner_radius_);
        props.set("border_width", border_width_);
        props.set("enabled", Json{enabled_});
    }

    /// @brief 从 props JSON 还原自有属性（与 `serialize_props` 往返闭环）。
    /// @param props 属性 JSON：缺省键保持当前值不改动；`checked` 经 `set_value` 写入（会触发
    ///              `on_changed` 与无障碍事件），`active_color` 缺失即回到「跟随主题 primary」。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("checked")) {
            set_value(props.at("checked")->as_or<bool>(false));
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(*props.at("active_color"));
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("check_color")) {
            check_color_ = json_to_color(*props.at("check_color"));
        }
        if (props.contains("size")) {
            size_ = props.at("size")->as_or<float>(0.0F);
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props.at("corner_radius")->as_or<float>(0.0F);
        }
        if (props.contains("border_width")) {
            border_width_ = props.at("border_width")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = size_, .height = size_});
    }

    /// @brief 三态配色（勾选填充 / 未勾选描边 / 勾号），由 `resolve_palette` 一次性算出。
    struct Palette {
        Color active;  ///< 勾选态填充色（显式设置优先，否则跟随主题 primary）
        Color border;  ///< 未勾选描边色
        Color check;  ///< 勾号颜色
    };

    /// @brief 解析当前状态配色：禁用时统一灰化。子类可只覆盖配色而不改绘制几何。
    /// @param ctx 构建上下文：提供主题作用域，未显式设置 `active_color` 时取 `inherit_theme(ctx).primary`。
    /// @return 三态配色聚合；禁用态返回固定灰化三元组（忽略显式设置的颜色）。
    /// @note Side-effects: none
    [[nodiscard]] virtual auto resolve_palette(const BuildContext &ctx) -> Palette {
        Palette pal{.active = active_color_.value_or(inherit_theme(ctx).primary),
                    .border = border_color_,
                    .check = check_color_};
        if (!enabled_) {
            // 禁用灰化：统一降饱和（对标 Material disabled 38% 透明语义的软件光栅近似）。
            pal.active = Color{176, 176, 180, 255};
            pal.border = Color{200, 200, 204, 255};
            pal.check = Color{245, 245, 247, 255};
        }
        return pal;
    }

    /// @brief 圆角半径：`corner_radius < 0` 表示自动 = 边长 ×0.2。
    /// @param bounds 本控件的绘制矩形，自动半径取其较短边长。
    /// @return 实际用于圆角矩形填充/描边的半径（dp）。
    [[nodiscard]] virtual auto corner_radius(const Rect &bounds) -> float {
        return corner_radius_ >= 0.0F ? corner_radius_ : std::min(bounds.size.width, bounds.size.height) * 0.2F;
    }

    /// @brief 绘制勾选态盒体：激活色填充，悬停 / 按下乘性加深（保留色相）。
    /// @param p 软件光栅画笔。
    /// @param bounds 盒体矩形（本控件分配到的绘制区域）。
    /// @param radius 圆角半径（dp），由 `corner_radius` 算出。
    /// @param active 勾选态填充色（`resolve_palette` 已解析，未含 hot 加深）。
    /// @param hot 是否处于悬停或按下态；true 时按 0.86（悬停）/0.72（按下）乘性加深。
    /// @note Side-effects: paints
    virtual auto paint_checked_box(Painter &p, const Rect &bounds, float radius, Color active, bool hot) -> void {
        if (hot) {
            active = active.shaded(pressed_ ? 0.72F : 0.86F);
        }
        p.fill_rounded_rect(bounds, radius, active);
    }

    /// @brief 绘制勾号 ✓：两段抗锯齿圆帽线段（Material 折点几何），线宽随边长缩放。
    /// @param p 软件光栅画笔。
    /// @param bounds 盒体矩形，勾号三段点按其宽高比例定位。
    /// @param check 勾号颜色（`resolve_palette` 已解析）。
    /// @note Side-effects: paints
    virtual auto paint_check_mark(Painter &p, const Rect &bounds, Color check) -> void {
        const float w = bounds.size.width;
        const float h = bounds.size.height;
        const float lw = std::max(1.6F, w * 0.11F);
        const Point p0{.x = bounds.origin.x + (w * 0.24F), .y = bounds.origin.y + (h * 0.54F)};
        const Point p1{.x = bounds.origin.x + (w * 0.42F), .y = bounds.origin.y + (h * 0.72F)};
        const Point p2{.x = bounds.origin.x + (w * 0.78F), .y = bounds.origin.y + (h * 0.32F)};
        p.draw_line(p0, p1, lw, check);
        p.draw_line(p1, p2, lw, check);
    }

    /// @brief 绘制未勾选态盒体：悬停铺激活色淡底（Fluent 式渐进反馈），描边在 hot 时高亮为激活色。
    /// @param p 软件光栅画笔。
    /// @param bounds 盒体矩形。
    /// @param radius 圆角半径（dp）。
    /// @param pal 当前三态配色（淡底取 `pal.active` 的低 alpha 变体，描边取 `pal.border`）。
    /// @param hot 是否悬停/按下；true 时铺淡底并把描边换成激活色。
    /// @note Side-effects: paints
    virtual auto paint_idle_box(Painter &p, const Rect &bounds, float radius, const Palette &pal, bool hot) -> void {
        if (hot) {
            Color tint = pal.active;
            tint.a = pressed_ ? 56 : 28;
            p.fill_rounded_rect(bounds, radius, tint);
        }
        p.draw_rounded_border(bounds, radius, border_width_, hot ? pal.active : pal.border);
    }

    /// @brief 绘制编排：取状态色 → 按勾选态分派到各虚钩子（子类一般只需覆盖钩子，无需重写本函数）。
    /// @param p 软件光栅画笔。
    /// @param bounds 本控件的绘制矩形（边长受 `size_` 约束）。
    /// @param ctx 构建上下文，供 `resolve_palette` 读取主题色。
    /// @note Side-effects: paints
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Palette pal = resolve_palette(ctx);
        const float radius = corner_radius(bounds);
        const bool hot = enabled_ && (hovered() || pressed_);
        if (value()) {
            paint_checked_box(p, bounds, radius, pal.active, hot);
            paint_check_mark(p, bounds, pal.check);
        } else {
            paint_idle_box(p, bounds, radius, pal, hot);
        }
    }

  private:
    Binding<bool> binding_;  // 声明须在 value_ 之前：Binding 构造器用 binding_.get() 初始化 value_，
                             // 成员按声明顺序初始化，故 binding_ 须先就位，否则 binding_.get() 空指针解引用。
    Reactive<bool> value_;
    std::function<void(bool)> on_changed_;
    std::optional<Color> active_color_;  ///< 勾选态填充色；空 = 跟随主题 primary
    Color border_color_ = Color{140, 140, 146, 255};  ///< 未勾选边框色
    Color check_color_ = colors::AURORA_WHITE;  ///< 勾号颜色
    float size_ = 20.0F;  ///< 方框边长 dp
    float corner_radius_ = -1.0F;  ///< 圆角半径 dp；< 0 自动 = 边长 × 0.2
    float border_width_ = 1.5F;  ///< 未勾选描边宽 dp
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
};

}  // namespace aurora
