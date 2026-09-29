#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/string_util.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 单选按钮组：互斥选项垂直/水平排列。
///
/// 选项为字符串列表，选中序号存于响应式 `selected()`。
/// 单控件承载整组（组内互斥天然保证），比分散 `Radio` + 手动 group 更 AI 友好。
///
/// 视觉（对标 Material RadioButton / Qt `QRadioButton`+`QButtonGroup`）：
/// - 外圈圆环 + 选中内实心圆点；`active_color` 未显式设置时跟随主题 `Theme::primary`；
/// - 圆点尺寸 / 行高 / 字号 / 文本色均可配置；禁用（`set_enabled(false)`）灰化并忽略点击。
///
/// 继承扩展点（protected 虚函数）：`paint_option`（逐选项行绘制，子类可整行自定义）。
///
/// 本类隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_change_，而其拷贝与 operator()
/// 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
/// 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
/// 刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// NOLINTNEXTLINE(bugprone-exception-escape)
class RadioGroup : public Widget {
  public:
    /// @brief 默认构造：空选项列表。
    RadioGroup() = default;
    /// @brief 以选项列表构造；初始选中序号钳制到 [0, 选项数-1]。
    /// @param options 选项文本列表。
    /// @param initial 初始选中序号（越界自动钳制）。
    /// @param horizontal true 水平排列，false 垂直排列。
    explicit RadioGroup(std::vector<std::string> options, int initial = 0, bool horizontal = false)
        : options_(std::move(options)), horizontal_(horizontal) {
        const int max_idx = static_cast<int>(options_.size()) - 1;
        // 把初始序号钳进 [0, max_idx] 后写入响应值（空列表时下界取 0）。
        selected_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    /// @brief 类型名。
    /// @return 固定为 "RadioGroup"。
    [[nodiscard]] auto type_name() const -> const char * override { return "RadioGroup"; }

    /// @brief 静态描述符：属性矩阵（含 required 的 options）、事件 on_change 与不变式。
    /// @return WidgetDescriptor。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "RadioGroup",
            .properties =
                {
                    {.name = "options",
                     .type = "vector<string>",
                     .default_value = "[]",
                     .required = true,
                     .note = "Option list",
                     .json_type = "array"},
                    {.name = "selected_index",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "Selected index",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "horizontal",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Horizontal layout",
                     .json_type = "boolean"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "Selected ring and dot color (defaults to theme primary)",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{140,140,146,255}",
                     .required = false,
                     .note = "Unselected ring color",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "{30,30,30,255}",
                     .required = false,
                     .note = "Option text color",
                     .json_type = "array"},
                    {.name = "dot_size",
                     .type = "float",
                     .default_value = "16.0",
                     .required = false,
                     .note = "Dot outer radius (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "row_height",
                     .type = "float",
                     .default_value = "28.0",
                     .required = false,
                     .note = "Row height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "13.0",
                     .required = false,
                     .note = "Option font size (pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Interactive (greyed out when disabled)",
                     .json_type = "boolean"},
                },
            .events = {"on_change"},
            .children_policy = "none",
            .invariants = {"selected_index >= 0", "selected_index < options.size()"},
            .examples = {R"(au::RadioGroup({"Yes", "No"}, 0))"},
        };
    }
    /// @brief 返回控件描述符，与 describe_static 同源。
    /// @return 同 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集可观察信号：登记选中值 `selected_`。
    /// @param out 收集输出容器，追加选中值信号视图。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_); }

    /// @brief 选项个数。
    /// @return 选项列表长度。
    [[nodiscard]] auto option_count() const -> std::size_t { return options_.size(); }
    /// @brief 响应式选中序号（可订阅观察）。
    /// @return 选中值状态引用。
    [[nodiscard]] auto selected() -> State<int> & { return selected_; }
    /// @brief 当前选中序号（快照读）。
    /// @return 选中索引。
    [[nodiscard]] auto selected_index() const -> int { return selected_.get(); }

    /// @brief 选中指定序号（越界忽略；触发 on_change）。
    /// @param index 目标序号；仅在界内且变化时写入。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, options_.size()) && index != selected_.get()) {
            selected_.set(index);
            mark_needs_paint();
            if (on_change_) {
                on_change_(index);
            }
        }
    }

    /// @brief 设置选择回调（链式）。
    /// @param cb 选中变化回调，参数为新选中序号。
    /// @return 自身引用（链式调用）。
    auto set_on_change(std::function<void(int)> cb) -> RadioGroup & {
        on_change_ = std::move(cb);
        return *this;
    }

    /// @brief 设置选中态圆环与圆点色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c 选中态颜色。
    /// @return 自身引用（链式调用）。
    auto set_active_color(Color c) -> RadioGroup & {
        active_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置未选中圆环色（链式）。
    /// @param c 圆环颜色。
    /// @return 自身引用（链式调用）。
    auto set_border_color(Color c) -> RadioGroup & {
        border_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选项文本色（链式）。
    /// @param c 文本颜色。
    /// @return 自身引用（链式调用）。
    auto set_text_color(Color c) -> RadioGroup & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置圆点外径 dp（链式）。
    /// @param s 圆点外径；非正值回落默认 16dp。标脏布局。
    /// @return 自身引用（链式调用）。
    auto set_dot_size(float s) -> RadioGroup & {
        dot_size_ = s > 0.0F ? s : 16.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置行高 dp（链式；决定选项间距与命中区）。
    /// @param h 行高；非正值回落默认 28dp。标脏布局。
    /// @return 自身引用（链式调用）。
    auto set_row_height(float h) -> RadioGroup & {
        row_height_ = h > 0.0F ? h : 28.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置选项字号 pt（链式）。
    /// @param s 字号；非正值回落默认 13pt。标脏布局。
    /// @return 自身引用（链式调用）。
    auto set_font_size(float s) -> RadioGroup & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    /// @param v true 可交互；false 禁用。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_enabled(bool v) -> RadioGroup & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    /// @brief 是否启用。
    /// @return 启用状态。
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 按下命中选项行即选中该项：垂直按行高取整、水平按测量宽度累计；禁用态吞掉点击不切换。
    /// @param e 鼠标事件；Press 命中界内或禁用态时被消费。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            const int idx = horizontal_ ? hit_index_horizontal(e.local_position.x)
                                        : static_cast<int>(e.local_position.y / row_height_);
            if (idx >= 0 && std::cmp_less(idx, options_.size())) {
                select(idx);
                e.is_handled = true;
                return;
            }
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 始终参与点击（选项行本身即点击目标）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 序列化整组属性至 JSON（选项/选中序号/排列/颜色/尺寸/启用）。
    /// @param props 输出属性对象，写入本控件键值。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        Json opts = Json::array();
        for (const auto &o : options_) {
            opts.push_back(o);
        }
        props.set("options", opts);
        props.set("selected_index", selected_.get());
        props.set("horizontal", Json{horizontal_});
        if (active_color_.has_value()) {
            props.set("active_color", color_to_json(*active_color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("border_color", color_to_json(border_color_));
        props.set("text_color", color_to_json(text_color_));
        props.set("dot_size", dot_size_);
        props.set("row_height", row_height_);
        props.set("font_size", font_size_);
        props.set("enabled", Json{enabled_});
    }

    /// @brief 从 JSON 重建整组属性（缺失键保持现值）。
    /// @param props 源属性对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("options")) {
            const auto *arr = props.at("options");
            if (arr != nullptr && arr->is_array()) {
                options_.clear();
                for (const auto *o = arr->begin(); o != arr->end(); ++o) {
                    options_.push_back(o->as_or<std::string>(""));
                }
            }
        }
        if (props.contains("selected_index")) {
            selected_.set(props.at("selected_index")->as_or<std::int32_t>(0));
        }
        if (props.contains("horizontal")) {
            horizontal_ = props.at("horizontal")->as_or<bool>(false);
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(*props.at("active_color"));
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(*props.at("text_color"));
        }
        if (props.contains("dot_size")) {
            dot_size_ = props.at("dot_size")->as_or<float>(0.0F);
        }
        if (props.contains("row_height")) {
            row_height_ = props.at("row_height")->as_or<float>(0.0F);
        }
        if (props.contains("font_size")) {
            font_size_ = props.at("font_size")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        Font f;
        f.size_pt = font_size_;
        if (horizontal_) {
            float w = 0.0F;
            for (const auto &o : options_) {
                w += item_width(render::FontEngine::measure_width(o, f));
            }
            return c.constrain(Size{.width = w, .height = row_height_});
        }
        float max_w = 0.0F;
        for (const auto &o : options_) {
            max_w = std::max(max_w, item_width(render::FontEngine::measure_width(o, f)));
        }
        return c.constrain(Size{.width = max_w, .height = static_cast<float>(options_.size()) * row_height_});
    }

    /// @brief 绘制各选项行：先解析状态色（显式设置优先，否则跟随主题 primary；禁用态统一灰化），再逐行调 paint_option。
    /// @param p 绘制器。
    /// @param bounds 绘制区域。
    /// @param ctx 构建上下文（用于主题解析）。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = font_size_;
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = active_color_.value_or(inherit_theme(ctx).primary);
        Color idle = border_color_;
        Color text = text_color_;
        if (!enabled_) {
            accent = Color{176, 176, 180, 255};
            idle = Color{200, 200, 204, 255};
            text = Color{168, 168, 172, 255};
        }
        float x = bounds.origin.x;
        float y = bounds.origin.y;
        for (std::size_t i = 0; i < options_.size(); ++i) {
            const bool sel = std::cmp_equal(i, selected_.get());
            const Rect row{
                .origin = Point{.x = x, .y = y},
                .size = Size{.width = horizontal_ ? item_width(render::FontEngine::measure_width(options_[i], f))
                                                  : bounds.size.width,
                             .height = row_height_}};
            paint_option(p, i, row, sel, accent, idle, text, f);
            if (horizontal_) {
                x += row.size.width;
            } else {
                y += row_height_;
            }
        }
    }

    /// @brief 继承扩展点：绘制单个选项行（圆点 + 文本）。子类可覆盖以自定义整行外观。
    /// @param p 绘制器。
    /// @param index 选项序号。
    /// @param row 该行的矩形区域。
    /// @param selected 该行是否选中。
    /// @param accent 选中态色。
    /// @param idle 未选中圆环色。
    /// @param text 文本色。
    /// @param f 字体。
    virtual auto paint_option(Painter &p, std::size_t index, const Rect &row, bool selected, Color accent, Color idle,
                              Color text, const Font &f) -> void {
        // 圆点：外圈真圆环（draw_rounded_border，radius = 半径即圆）+ 选中内实心圆点，
        // 对标 Material RadioButton。
        const Rect outer{
            .origin = Point{.x = row.origin.x + 4.0F, .y = row.origin.y + ((row_height_ - dot_size_) * 0.5F)},
            .size = Size{.width = dot_size_, .height = dot_size_}};
        // 以外圈圆环绘制未选/选中态边框。
        p.draw_rounded_border(outer, outer.size.width * 0.5F, selected ? 2.0F : 1.5F, selected ? accent : idle);
        if (selected) {
            const float d = outer.size.width * 0.5F;  // 内点直径 = 外圈一半
            const Rect inner{.origin = Point{.x = outer.origin.x + ((outer.size.width - d) * 0.5F),
                                             .y = outer.origin.y + ((outer.size.height - d) * 0.5F)},
                             .size = Size{.width = d, .height = d}};
            p.fill_rounded_rect(inner, d * 0.5F, accent);
        }
        const Rect text_box{.origin = Point{.x = row.origin.x + dot_size_ + 10.0F, .y = row.origin.y + 6.0F},
                            .size = Size{.width = 400.0F, .height = row_height_ - 12.0F}};
        // 在文本框区域绘制选项文字。
        p.draw_text(text_box, options_[index], f, text);
    }

    /// @brief 命中测试：点落在自身范围内即命中本控件（使整组可接收点击）。
    /// @param local 自身坐标系测试点。
    /// @param bounds 自身边界。
    /// @param ctx 构建上下文（当前实现未使用）。
    /// @return 命中时返回自身指针，否则 nullptr。
    auto on_hit_test(const Point &local, const Rect &bounds, [[maybe_unused]] const BuildContext &ctx)
        -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    /// @brief 选项行总宽 = 文本宽 + 圆点外径 + 20dp 左右间距。
    /// @param text_w 实测文本宽度。
    /// @return 单行宽度。
    [[nodiscard]] auto item_width(float text_w) const -> float { return text_w + dot_size_ + 20.0F; }

    /// @brief 水平排列命中：按测量宽度自左向右累计，定位 x 落在哪个选项行。
    /// @param x 自身坐标系横坐标。
    /// @return 命中的选项序号；不落在任何行时为 -1。
    [[nodiscard]] auto hit_index_horizontal(float x) const -> int {
        Font f;
        f.size_pt = font_size_;
        float acc = 0.0F;
        for (std::size_t i = 0; i < options_.size(); ++i) {
            const float w = item_width(render::FontEngine::measure_width(options_[i], f));
            if (x >= acc && x < acc + w) {
                return static_cast<int>(i);
            }
            acc += w;
        }
        return -1;
    }

    /// @brief 选项文本列表（区间式豁免起点：成员可见性按设计豁免，详见类文档）。
    /// NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> options_;
    State<int> selected_{0};  ///< 响应式选中序号
    bool horizontal_ = false;  ///< 是否水平排列
    std::function<void(int)> on_change_;  ///< 选中变化回调（参数为新序号）
    std::optional<Color> active_color_;  ///< 选中态圆环/圆点色；空 = 跟随主题 primary
    Color border_color_ = Color{140, 140, 146, 255};  ///< 未选中圆环色
    Color text_color_ = Color{30, 30, 30, 255};  ///< 选项文本色
    float dot_size_ = 16.0F;  ///< 圆点外径 dp
    float row_height_ = 28.0F;  ///< 行高 dp
    float font_size_ = 13.0F;  ///< 选项字号 pt
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

/// @brief 数字输入框：带上下箭头的数值调节。
///
/// 值域 [min, max]，按 `step` 递增/递减；`value()` 为响应式状态可订阅。
/// 支持前缀/后缀文本（如 "$"、"px"）；上下方向键可调节（获焦后）。
///
/// 视觉：背景 / 边框 / 文本 / 箭头颜色与圆角、字号均可配置；
/// 禁用（`set_enabled(false)`）灰化并忽略交互。
///
/// 继承扩展点（protected 虚函数）：`paint_box` / `paint_value` / `paint_arrows`。
///
/// 对标 Qt `QSpinBox`/`QDoubleSpinBox`、WPF `NumericUpDown`。
///
/// 本类隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_change_，而其拷贝与 operator()
/// 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
/// 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
/// 刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// NOLINTNEXTLINE(bugprone-exception-escape)
class SpinBox : public Widget {
  public:
    /// @brief 默认构造：值 0，范围 [0,100]，步长 1。
    SpinBox() = default;
    /// @brief 以值域与步长构造；初始值钳制到 [min, max]。
    /// @param initial 初始值（越界自动钳制）。
    /// @param min_v 最小值。
    /// @param max_v 最大值；小于 min_v 时回落为 min_v（保证非降）。
    /// @param step 步长；非正值回落为 1。
    SpinBox(double initial, double min_v, double max_v, double step = 1.0)
        : min_(min_v), max_(max_v < min_v ? min_v : max_v), step_(step <= 0.0 ? 1.0 : step) {
        // 把初始值钳进 [min_, max_] 后写入响应值。
        value_.set(std::clamp(initial, min_, max_));
    }

    /// @brief 类型名。
    /// @return 固定为 "SpinBox"。
    [[nodiscard]] auto type_name() const -> const char * override { return "SpinBox"; }

    /// @brief 静态描述符：属性矩阵（值域/步长/前后缀/颜色/圆角/字号/启用）、事件 on_change 与不变式。
    /// @return WidgetDescriptor。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "SpinBox",
            .properties =
                {
                    {.name = "value",
                     .type = "double",
                     .default_value = "0",
                     .required = false,
                     .note = "Current value",
                     .json_type = "number"},
                    {.name = "min",
                     .type = "double",
                     .default_value = "0",
                     .required = false,
                     .note = "Min value",
                     .json_type = "number"},
                    {.name = "max",
                     .type = "double",
                     .default_value = "100",
                     .required = false,
                     .note = "Max value",
                     .json_type = "number"},
                    {.name = "step",
                     .type = "double",
                     .default_value = "1",
                     .required = false,
                     .note = "Step",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "prefix",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Prefix text",
                     .json_type = "string"},
                    {.name = "suffix",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Suffix text",
                     .json_type = "string"},
                    {.name = "decimals",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "Decimals shown",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "background",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "Background color",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{200,200,205,255}",
                     .required = false,
                     .note = "Border color",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "{30,30,30,255}",
                     .required = false,
                     .note = "Value text color",
                     .json_type = "array"},
                    {.name = "arrow_color",
                     .type = "Color",
                     .default_value = "{100,100,105,255}",
                     .required = false,
                     .note = "Arrow color",
                     .json_type = "array"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "4.0",
                     .required = false,
                     .note = "Corner radius (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "13.0",
                     .required = false,
                     .note = "Value font size (pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Interactive (greyed out when disabled)",
                     .json_type = "boolean"},
                },
            .events = {"on_change"},
            .children_policy = "none",
            .invariants = {"min <= max", "step >= 0", "value >= min", "value <= max"},
            .examples = {"au::SpinBox(50, 0, 100, 5)"},
        };
    }
    /// @brief 返回控件描述符，与 describe_static 同源。
    /// @return 同 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集可观察信号：登记当前值 `value_`。
    /// @param out 收集输出容器，追加数值信号视图。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&value_); }

    /// @brief 响应式当前值（可订阅观察）。
    /// @return 值状态引用。
    [[nodiscard]] auto value() -> State<double> & { return value_; }
    /// @brief 当前值（快照读）。
    /// @return 数值。
    [[nodiscard]] auto value_of() const -> double { return value_.get(); }
    /// @brief 最小值。
    /// @return 值域下界。
    [[nodiscard]] auto min_value() const -> double { return min_; }
    /// @brief 最大值。
    /// @return 值域上界。
    [[nodiscard]] auto max_value() const -> double { return max_; }
    /// @brief 步长。
    /// @return 每次递增/递减的量。
    [[nodiscard]] auto step() const -> double { return step_; }

    /// @brief 设置值（钳制到 [min,max]；触发 on_change）。
    /// @param v 目标值；越界钳制后仅在变化时写入。
    auto set_value(double v) -> void {
        const double clamped = std::clamp(v, min_, max_);
        if (clamped != value_.get()) {
            value_.set(clamped);
            mark_needs_paint();
            if (on_change_) {
                on_change_(clamped);
            }
        }
    }

    /// @brief 递增一步。
    auto increment() -> void { set_value(value_.get() + step_); }
    /// @brief 递减一步。
    auto decrement() -> void { set_value(value_.get() - step_); }

    /// @brief 设置前缀文本（链式）。
    /// @param s 前缀（如 "$"）。
    /// @return 自身引用（链式调用）。
    auto set_prefix(std::string s) -> SpinBox & {
        prefix_ = std::move(s);
        return *this;
    }
    /// @brief 设置后缀文本（链式）。
    /// @param s 后缀（如 "px"）。
    /// @return 自身引用（链式调用）。
    auto set_suffix(std::string s) -> SpinBox & {
        suffix_ = std::move(s);
        return *this;
    }
    /// @brief 设置小数位数（链式，0=整数显示）。
    /// @param n 小数位数；钳制到 [0,9]。
    /// @return 自身引用（链式调用）。
    auto set_decimals(int n) -> SpinBox & {
        decimals_ = std::clamp(n, 0, 9);
        return *this;
    }

    /// @brief 设置回调（链式）。
    /// @param cb 数值变化回调，参数为新值。
    /// @return 自身引用（链式调用）。
    auto set_on_change(std::function<void(double)> cb) -> SpinBox & {
        on_change_ = std::move(cb);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    /// @param c 背景颜色。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_background(Color c) -> SpinBox & {
        background_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置边框色（链式）。
    /// @param c 边框颜色。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_border_color(Color c) -> SpinBox & {
        border_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置数值文本色（链式）。
    /// @param c 文本颜色。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_text_color(Color c) -> SpinBox & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置箭头颜色（链式）。
    /// @param c 箭头颜色。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_arrow_color(Color c) -> SpinBox & {
        arrow_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；0 = 直角）。
    /// @param r 圆角半径；负值归 0。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_corner_radius(float r) -> SpinBox & {
        corner_radius_ = r >= 0.0F ? r : 0.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置数值字号 pt（链式）。
    /// @param s 字号；非正值回落默认 13pt。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_font_size(float s) -> SpinBox & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略交互。
    /// @param v true 可交互；false 禁用。标脏绘制。
    /// @return 自身引用（链式调用）。
    auto set_enabled(bool v) -> SpinBox & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    /// @brief 是否启用。
    /// @return 启用状态。
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 当前显示文本（前缀 + 值 + 后缀）。
    /// @return 前缀 + 按 decimals 格式化的数值 + 后缀。
    [[nodiscard]] auto display_text() const -> std::string {
        return prefix_ + internal::string_format("%.*f", decimals_, value_.get()) + suffix_;
    }

    /// @brief 点击箭头区调节（右侧上下两半）；值区点击获焦；禁用态吞掉点击。
    /// @param e 鼠标事件；Press 命中箭头区或值区时被消费。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡），但不调节
            return;
        }
        if (e.action == MouseAction::Press) {
            const float arrow_x = size_.width - AURORA_ARROW_ZONE;
            if (e.local_position.x >= arrow_x) {
                if (e.local_position.y < size_.height * 0.5F) {
                    increment();
                } else {
                    decrement();
                }
                e.is_handled = true;
                return;
            }
            request_focus();  // 值区点击获焦，随后可用上下方向键调节
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 始终参与点击（箭头区与值区均为点击目标）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 上下方向键调节（获焦后）。
    /// @param e 按键事件；启用态下 ArrowUp/ArrowDown 的按下动作被消费。
    auto on_key_event(KeyEvent &e) -> void override {
        if (enabled_ && e.action == KeyAction::Down) {
            if (e.key == static_cast<int>(KeyCode::ArrowUp)) {
                increment();
                e.is_handled = true;
                return;
            }
            if (e.key == static_cast<int>(KeyCode::ArrowDown)) {
                decrement();
                e.is_handled = true;
                return;
            }
        }
        Widget::on_key_event(e);
    }

    /// @brief 序列化全部属性至 JSON（值域/步长/前后缀/小数位/颜色/圆角/字号/启用）。
    /// @param props 输出属性对象，写入本控件键值。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("value", value_.get());
        props.set("min", min_);
        props.set("max", max_);
        props.set("step", step_);
        props.set("prefix", prefix_);
        props.set("suffix", suffix_);
        props.set("decimals", decimals_);
        props.set("background", color_to_json(background_));
        props.set("border_color", color_to_json(border_color_));
        props.set("text_color", color_to_json(text_color_));
        props.set("arrow_color", color_to_json(arrow_color_));
        props.set("corner_radius", corner_radius_);
        props.set("font_size", font_size_);
        props.set("enabled", Json{enabled_});
    }

    /// @brief 从 JSON 重建属性（缺失键保持现值；value 最后写入并钳制到新值域）。
    /// @param props 源属性对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("min")) {
            min_ = props.at("min")->as_or<double>(0.0);
        }
        if (props.contains("max")) {
            max_ = props.at("max")->as_or<double>(0.0);
        }
        if (props.contains("step")) {
            step_ = props.at("step")->as_or<double>(0.0);
        }
        if (props.contains("prefix")) {
            prefix_ = props.at("prefix")->as_or<std::string>("");
        }
        if (props.contains("suffix")) {
            suffix_ = props.at("suffix")->as_or<std::string>("");
        }
        if (props.contains("decimals")) {
            decimals_ = props.at("decimals")->as_or<std::int32_t>(0);
        }
        if (props.contains("background")) {
            background_ = json_to_color(*props.at("background"));
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(*props.at("text_color"));
        }
        if (props.contains("arrow_color")) {
            arrow_color_ = json_to_color(*props.at("arrow_color"));
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props.at("corner_radius")->as_or<float>(0.0F);
        }
        if (props.contains("font_size")) {
            font_size_ = props.at("font_size")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
        if (props.contains("value")) {
            value_.set(std::clamp(props.at("value")->as_or<double>(0.0), min_, max_));
        }
    }

  protected:
    /// @brief 布局：固定期望尺寸 120x30dp，按传入约束钳制。
    /// @param c 布局约束。
    /// @param ctx 构建上下文（当前实现未使用）。
    /// @return 钳制后的尺寸。
    auto on_layout(const Constraints &c, [[maybe_unused]] const BuildContext &ctx) -> Size override {
        return c.constrain(Size{.width = 120.0F, .height = AURORA_BOX_HEIGHT});
    }

    /// @brief 绘制：解析状态色（禁用态统一灰化）后依次绘框、文本与箭头。
    /// @param p 绘制器。
    /// @param bounds 绘制区域。
    /// @param ctx 构建上下文（当前实现未使用）。
    auto on_paint(Painter &p, const Rect &bounds, [[maybe_unused]] const BuildContext &ctx) -> void override {
        Color bg = background_;
        Color border = border_color_;
        Color text = text_color_;
        Color arrow = arrow_color_;
        if (!enabled_) {
            bg = Color{235, 235, 237, 255};
            border = Color{215, 215, 219, 255};
            text = Color{168, 168, 172, 255};
            arrow = Color{190, 190, 194, 255};
        }
        paint_box(p, bounds, bg, border);
        paint_value(p, bounds, text);
        paint_arrows(p, bounds, arrow);
    }

    /// @brief 继承扩展点：绘制值框（背景 + 边框，圆角可配）。
    /// @param p 绘制器。
    /// @param bounds 绘制区域。
    /// @param bg 背景色。
    /// @param border 边框色。
    virtual auto paint_box(Painter &p, const Rect &bounds, Color bg, Color border) -> void {
        if (corner_radius_ > 0.0F) {
            p.fill_rounded_rect(bounds, corner_radius_, bg);
            p.draw_rounded_border(bounds, corner_radius_, 1.0F, border);
        } else {
            p.fill_rect(bounds, bg);
            p.draw_rect(bounds, border);
        }
    }

    /// @brief 继承扩展点：绘制数值文本（前缀 + 值 + 后缀）。
    /// @param p 绘制器。
    /// @param bounds 绘制区域。
    /// @param text 文本色。
    virtual auto paint_value(Painter &p, const Rect &bounds, Color text) -> void {
        Font f;
        f.size_pt = font_size_;
        const Rect text_box{
            .origin = Point{.x = bounds.origin.x + 8.0F, .y = bounds.origin.y + 8.0F},
            .size = Size{.width = bounds.size.width - AURORA_ARROW_ZONE - 12.0F, .height = bounds.size.height - 16.0F}};
        p.draw_text(text_box, display_text(), f, text);
    }

    /// @brief 继承扩展点：绘制上下箭头区。
    /// @param p 绘制器。
    /// @param bounds 绘制区域。
    /// @param arrow 箭头色。
    virtual auto paint_arrows(Painter &p, const Rect &bounds, Color arrow) -> void {
        const float ax = bounds.origin.x + bounds.size.width - AURORA_ARROW_ZONE;
        Font sf;
        sf.size_pt = 9.0F;
        const Rect up_box{
            .origin = Point{.x = ax + 6.0F, .y = bounds.origin.y + 2.0F},
            .size = Size{.width = AURORA_ARROW_ZONE - 8.0F, .height = (bounds.size.height * 0.5F) - 2.0F}};
        const Rect dn_box{
            .origin = Point{.x = ax + 6.0F, .y = bounds.origin.y + (bounds.size.height * 0.5F)},
            .size = Size{.width = AURORA_ARROW_ZONE - 8.0F, .height = (bounds.size.height * 0.5F) - 2.0F}};
        p.draw_text(up_box, "^", sf, arrow);
        p.draw_text(dn_box, "v", sf, arrow);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    static constexpr float AURORA_BOX_HEIGHT = 30.0F;  ///< 输入框高度(dp)
    static constexpr float AURORA_ARROW_ZONE = 22.0F;  ///< 箭头区宽度(dp)

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    State<double> value_{0.0};
    double min_ = 0.0;
    double max_ = 100.0;
    double step_ = 1.0;
    std::string prefix_;
    std::string suffix_;
    int decimals_ = 0;
    std::function<void(double)> on_change_;
    Color background_ = Color{255, 255, 255, 255};  ///< 背景色
    Color border_color_ = Color{200, 200, 205, 255};  ///< 边框色
    Color text_color_ = Color{30, 30, 30, 255};  ///< 数值文本色
    Color arrow_color_ = Color{100, 100, 105, 255};  ///< 箭头颜色
    float corner_radius_ = 4.0F;  ///< 圆角半径 dp；0 = 直角
    float font_size_ = 13.0F;  ///< 数值字号 pt
    bool enabled_ = true;  ///< 禁用态灰化并忽略交互
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
