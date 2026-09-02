#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/string_util.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 简单日期值（无时区语义；合法性由 Date::is_valid 检查）。
struct Date {
    int year = 2026;
    int month = 1; ///< 1..12
    int day = 1;   ///< 1..31

    [[nodiscard]] static auto days_in_month(int y, int m) -> int {
        static constexpr int aurora_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; // NOLINT
        if (m < 1 || m > 12) {
            return 0;
        }
        if (m == 2) {
            const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
            return leap ? 29 : 28;
        }
        return aurora_days[m - 1]; // NOLINT
    }

    [[nodiscard]] auto is_valid() const -> bool {
        return month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
    }

    [[nodiscard]] auto to_string() const -> std::string {
        return internal::string_format("%04d-%02d-%02d", year, month, day);
    }

    auto operator==(const Date &o) const -> bool = default;
};

/// @brief 简单时刻值（24 小时制）。
struct TimeOfDay {
    int hour = 0;   ///< 0..23
    int minute = 0; ///< 0..59

    [[nodiscard]] auto is_valid() const -> bool { return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59; }

    [[nodiscard]] auto to_string() const -> std::string { return internal::string_format("%02d:%02d", hour, minute); }

    auto operator==(const TimeOfDay &o) const -> bool = default;
};

/**
 * @brief 日期选择器：月历网格选择。
 *
 * 顶部年月导航（< 年月 >），下方 7 列日历网格；点击日期选中。
 * 对标 Qt `QDateEdit`+日历、Flutter `showDatePicker`、SwiftUI `DatePicker`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class DatePicker : public Widget {
  public:
    DatePicker() = default;
    explicit DatePicker(Date initial) {
        if (initial.is_valid()) {
            m_selected.set(initial);
        }
        m_view_year = m_selected.get().year;
        m_view_month = m_selected.get().month;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "DatePicker"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "DatePicker",
            .properties = {
                { .name = "year", .type = "int", .default_value = "2026", .required = false, .note = "选中年", .json_type = "integer" },
                { .name = "month", .type = "int", .default_value = "1", .required = false, .note = "选中月(1..12)", .json_type = "integer", .enum_values = {}, .min_value = "1", .max_value = "12" },
                { .name = "day", .type = "int", .default_value = "1", .required = false, .note = "选中日", .json_type = "integer", .enum_values = {}, .min_value = "1", .max_value = "31" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .invariants = { "month >= 1 && month <= 12", "day >= 1 && day <= 31" },
            .examples = { "au::DatePicker(au::Date{2026, 7, 25})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    [[nodiscard]] auto selected() -> State<Date> & { return m_selected; }
    [[nodiscard]] auto selected_date() const -> Date { return m_selected.get(); }
    [[nodiscard]] auto view_year() const -> int { return m_view_year; }
    [[nodiscard]] auto view_month() const -> int { return m_view_month; }

    /// @brief 选中日期（非法忽略；触发 on_change 并同步视图年月）。
    auto select(Date d) -> void {
        if (!d.is_valid() || d == m_selected.get()) {
            return;
        }
        m_selected.set(d);
        m_view_year = d.year;
        m_view_month = d.month;
        mark_needs_paint();
        if (m_on_change) {
            m_on_change(d);
        }
    }

    /// @brief 视图翻到下一月/上一月（不改变选中）。
    auto next_month() -> void {
        if (++m_view_month > 12) {
            m_view_month = 1;
            ++m_view_year;
        }
        mark_needs_paint();
    }
    auto prev_month() -> void {
        if (--m_view_month < 1) {
            m_view_month = 12;
            --m_view_year;
        }
        mark_needs_paint();
    }

    auto set_on_change(std::function<void(Date)> cb) -> DatePicker & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 点击交互：头部左右箭头翻月；网格点击选日。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action != MouseAction::Press) {
            Widget::on_pointer_event(e);
            return;
        }
        // 头部（高 AURORA_HEADER）
        if (e.local_position.y < AURORA_HEADER) {
            if (e.local_position.x < AURORA_NAV_ZONE) {
                prev_month();
            } else if (e.local_position.x > m_size.width - AURORA_NAV_ZONE) {
                next_month();
            }
            e.handled = true;
            return;
        }
        // 日历网格
        const float cell_w = m_size.width / 7.0f;
        const float cell_h = (m_size.height - AURORA_HEADER) / 6.0f;
        const int col = static_cast<int>(e.local_position.x / cell_w);
        const int row = static_cast<int>((e.local_position.y - AURORA_HEADER) / cell_h);
        const int day = grid_day(row, col);
        if (day >= 1) {
            select(Date{ .year = m_view_year, .month = m_view_month, .day = day });
        }
        e.handled = true;
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 网格 (row, col) 对应的日号（<=0 = 空格）。周日为第 0 列。
    [[nodiscard]] auto grid_day(int row, int col) const -> int {
        const int first_wd = weekday_of_first(m_view_year, m_view_month);
        const int day = (row * 7) + col - first_wd + 1;
        return (day >= 1 && day <= Date::days_in_month(m_view_year, m_view_month)) ? day : 0;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        const Date d = m_selected.get();
        props["year"] = d.year;
        props["month"] = d.month;
        props["day"] = d.day;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        Date d = m_selected.get();
        if (props.contains("year")) {
            d.year = props["year"].get<int>();
        }
        if (props.contains("month")) {
            d.month = props["month"].get<int>();
        }
        if (props.contains("day")) {
            d.day = props["day"].get<int>();
        }
        if (d.is_valid()) {
            m_selected.set(d);
            m_view_year = d.year;
            m_view_month = d.month;
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 224.0f, .height = AURORA_HEADER + (6.0f * 28.0f) });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        Font f;
        f.size_pt = 12.0f;
        p.fill_rect(bounds, Color(255, 255, 255, 255));
        p.draw_rect(bounds, Color(210, 210, 215, 255));

        // 头部：< 年-月 >
        const std::string title = internal::string_format("%04d-%02d", m_view_year, m_view_month);
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 8.0f, .y = bounds.origin.y + 8.0f },
                          .size = Size{ .width = AURORA_NAV_ZONE, .height = AURORA_HEADER - 16.0f } },
                    "<", f, Color(0, 122, 255, 255));
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + (bounds.size.width * 0.5f) - 30.0f,
                                           .y = bounds.origin.y + 8.0f },
                          .size = Size{ .width = 80.0f, .height = AURORA_HEADER - 16.0f } },
                    title, f, Color(30, 30, 30, 255));
        p.draw_text(
            Rect{ .origin = Point{ .x = bounds.origin.x + bounds.size.width - 16.0f, .y = bounds.origin.y + 8.0f },
                  .size = Size{ .width = 12.0f, .height = AURORA_HEADER - 16.0f } },
            ">", f, Color(0, 122, 255, 255));

        // 网格
        const float cell_w = bounds.size.width / 7.0f;
        const float cell_h = (bounds.size.height - AURORA_HEADER) / 6.0f;
        const Date sel = m_selected.get();
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 7; ++col) {
                const int day = grid_day(row, col);
                if (day < 1) {
                    continue;
                }
                const Rect cell{ .origin =
                                     Point{ .x = bounds.origin.x + (static_cast<float>(col) * cell_w),
                                            .y = bounds.origin.y + AURORA_HEADER + (static_cast<float>(row) * cell_h) },
                                 .size = Size{ .width = cell_w, .height = cell_h } };
                const bool is_sel = sel.year == m_view_year && sel.month == m_view_month && sel.day == day;
                if (is_sel) {
                    p.fill_rect(cell, Color(0, 122, 255, 40));
                }
                p.draw_text(Rect{ .origin = Point{ .x = cell.origin.x + 6.0f, .y = cell.origin.y + 5.0f },
                                  .size = Size{ .width = cell_w - 8.0f, .height = cell_h - 8.0f } },
                            std::to_string(day), f, is_sel ? Color(0, 122, 255, 255) : Color(40, 40, 45, 255));
            }
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

  private:
    static constexpr float AURORA_HEADER = 32.0f;   ///< 头部导航高度(dp)
    static constexpr float AURORA_NAV_ZONE = 28.0f; ///< 左右翻月点击区(dp)

    /// @brief 该月 1 号的星期（0=周日；Zeller 公式）。
    [[nodiscard]] static auto weekday_of_first(int y, int m) -> int {
        int yy = y;
        int mm = m;
        if (mm < 3) {
            mm += 12;
            --yy;
        }
        const int k = yy % 100;
        const int j = yy / 100;
        const int h = (1 + (13 * (mm + 1) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7; // 0=周六
        return (h + 6) % 7;                                                            // 转 0=周日
    }

    State<Date> m_selected{ Date{} };
    int m_view_year = 2026;
    int m_view_month = 1;
    std::function<void(Date)> m_on_change;
};

/**
 * @brief 时间选择器：时/分两列上下调节。
 * 对标 Qt `QTimeEdit`、Flutter `showTimePicker`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class TimePicker : public Widget {
  public:
    TimePicker() = default;
    explicit TimePicker(TimeOfDay initial) {
        if (initial.is_valid()) {
            m_selected.set(initial);
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "TimePicker"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TimePicker",
            .properties = {
                { .name="hour", .type="int", .default_value="0", .required=false, .note="时(0..23)", .json_type="integer", .enum_values={}, .min_value="0", .max_value="23" },
                { .name="minute", .type="int", .default_value="0", .required=false, .note="分(0..59)", .json_type="integer", .enum_values={}, .min_value="0", .max_value="59" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .invariants = { "hour >= 0 && hour <= 23", "minute >= 0 && minute <= 59" },
            .examples = { "au::TimePicker(au::TimeOfDay{14, 30})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    [[nodiscard]] auto selected() -> State<TimeOfDay> & { return m_selected; }
    [[nodiscard]] auto selected_time() const -> TimeOfDay { return m_selected.get(); }

    auto select(TimeOfDay t) -> void {
        if (!t.is_valid() || t == m_selected.get()) {
            return;
        }
        m_selected.set(t);
        mark_needs_paint();
        if (m_on_change) {
            m_on_change(t);
        }
    }

    /// @brief 时/分调节（自动回卷）。
    auto add_hours(int dh) -> void {
        TimeOfDay t = m_selected.get();
        t.hour = (((t.hour + dh) % 24) + 24) % 24;
        select(t);
    }
    auto add_minutes(int dm) -> void {
        TimeOfDay t = m_selected.get();
        const int total = ((((t.hour * 60) + t.minute + dm) % 1440) + 1440) % 1440;
        t.hour = total / 60;
        t.minute = total % 60;
        select(t);
    }

    auto set_on_change(std::function<void(TimeOfDay)> cb) -> TimePicker & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 点击交互：左半列=时、右半列=分；上半=+1、下半=-1。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            const bool is_hour = e.local_position.x < m_size.width * 0.5f;
            const bool is_up = e.local_position.y < m_size.height * 0.5f;
            if (is_hour) {
                add_hours(is_up ? 1 : -1);
            } else {
                add_minutes(is_up ? 1 : -1);
            }
            e.handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["hour"] = m_selected.get().hour;
        props["minute"] = m_selected.get().minute;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        TimeOfDay t = m_selected.get();
        if (props.contains("hour")) {
            t.hour = props["hour"].get<int>();
        }
        if (props.contains("minute")) {
            t.minute = props["minute"].get<int>();
        }
        if (t.is_valid()) {
            m_selected.set(t);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 120.0f, .height = 72.0f });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        Font f;
        f.size_pt = 18.0f;
        p.fill_rect(bounds, Color(255, 255, 255, 255));
        p.draw_rect(bounds, Color(210, 210, 215, 255));
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 16.0f, .y = bounds.origin.y + 24.0f },
                          .size = Size{ .width = bounds.size.width - 32.0f, .height = 28.0f } },
                    m_selected.get().to_string(), f, Color(30, 30, 30, 255));
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

  private:
    State<TimeOfDay> m_selected{ TimeOfDay{} };
    std::function<void(TimeOfDay)> m_on_change;
};

/**
 * @brief 颜色选择器：预设色板网格选择。
 * 对标 Qt `QColorDialog`（简化色板模式）、SwiftUI `ColorPicker`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ColorPicker : public Widget {
  public:
    ColorPicker() { m_selected.set(default_palette()[0]); }
    explicit ColorPicker(Color initial) { m_selected.set(initial); }

    [[nodiscard]] auto type_name() const -> const char * override { return "ColorPicker"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ColorPicker",
            .properties = {
                { .name="color", .type="Color", .default_value="black", .required=false, .note="选中颜色", .json_type="array" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .examples = { "au::ColorPicker(au::Color(255, 0, 0, 255))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    [[nodiscard]] auto selected() -> State<Color> & { return m_selected; }
    [[nodiscard]] auto selected_color() const -> Color { return m_selected.get(); }

    auto select(Color c) -> void {
        const Color cur = m_selected.get();
        if (c.m_r == cur.m_r && c.m_g == cur.m_g && c.m_b == cur.m_b && c.m_a == cur.m_a) {
            return;
        }
        m_selected.set(c);
        mark_needs_paint();
        if (m_on_change) {
            m_on_change(c);
        }
    }

    /// @brief 默认 16 色板。
    [[nodiscard]] static auto default_palette() -> const std::vector<Color> & {
        static const std::vector aurora_palette = {
            Color(0, 0, 0, 255),     Color(96, 96, 96, 255),  Color(160, 160, 160, 255), Color(255, 255, 255, 255),
            Color(220, 53, 69, 255), Color(255, 128, 0, 255), Color(255, 200, 0, 255),   Color(40, 167, 69, 255),
            Color(0, 190, 190, 255), Color(0, 122, 255, 255), Color(88, 86, 214, 255),   Color(175, 82, 222, 255),
            Color(255, 45, 85, 255), Color(139, 87, 42, 255), Color(52, 78, 65, 255),    Color(24, 40, 72, 255),
        };
        return aurora_palette;
    }

    /// @brief 自定义色板（链式）。
    auto set_palette(std::vector<Color> palette) -> ColorPicker & {
        if (!palette.empty()) {
            m_palette = std::move(palette);
        }
        mark_needs_layout();
        return *this;
    }
    [[nodiscard]] auto palette() const -> const std::vector<Color> & {
        return m_palette.empty() ? default_palette() : m_palette;
    }

    auto set_on_change(std::function<void(Color)> cb) -> ColorPicker & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 点击色板格选色（8 列网格）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            const auto &pal = palette();
            const float cell = m_size.width / AURORA_COLS;
            const int col = static_cast<int>(e.local_position.x / cell);
            const int row = static_cast<int>(e.local_position.y / cell);
            const std::size_t idx =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(AURORA_COLS)) + static_cast<std::size_t>(col);
            if (col >= 0 && col < static_cast<int>(AURORA_COLS) && idx < pal.size()) {
                select(pal[idx]);
            }
            e.handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["color"] = color_to_json(m_selected.get());
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("color")) {
            m_selected.set(json_to_color(props["color"]));
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const auto &pal = palette();
        const int rows =
            (static_cast<int>(pal.size()) + static_cast<int>(AURORA_COLS) - 1) / static_cast<int>(AURORA_COLS);
        constexpr float cell = 28.0f;
        return c.constrain(Size{ .width = cell * AURORA_COLS, .height = cell * static_cast<float>(rows) });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        const auto &pal = palette();
        const float cell = bounds.size.width / AURORA_COLS;
        const Color sel = m_selected.get();
        for (std::size_t i = 0; i < pal.size(); ++i) {
            const int col = static_cast<int>(i) % static_cast<int>(AURORA_COLS);
            const int row = static_cast<int>(i) / static_cast<int>(AURORA_COLS);
            const Rect swatch{ .origin = Point{ .x = bounds.origin.x + (static_cast<float>(col) * cell) + 2.0f,
                                                .y = bounds.origin.y + (static_cast<float>(row) * cell) + 2.0f },
                               .size = Size{ .width = cell - 4.0f, .height = cell - 4.0f } };
            p.fill_rect(swatch, pal[i]);
            const bool is_sel = pal[i].m_r == sel.m_r && pal[i].m_g == sel.m_g && pal[i].m_b == sel.m_b;
            p.draw_rect(swatch, is_sel ? Color(0, 122, 255, 255) : Color(200, 200, 205, 255));
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

  private:
    static constexpr float AURORA_COLS = 8.0f; ///< 色板列数

    State<Color> m_selected{ Color(0, 0, 0, 255) };
    std::vector<Color> m_palette;
    std::function<void(Color)> m_on_change;
};

} // namespace aurora
