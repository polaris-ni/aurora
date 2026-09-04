// 验证选择器三件套：DatePicker（月历/翻月/闰年）、TimePicker（回卷）、ColorPicker（色板）。

#include <memory>

#include "aurora/widget/pickers.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::ColorPicker;
using aurora::Constraints;
using aurora::Date;
using aurora::DatePicker;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::TimeOfDay;
using aurora::TimePicker;

namespace {

template <typename W>
auto layout_widget(std::shared_ptr<W> &w) -> void {
    BuildContext ctx;
    w->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 400.0F, .height = 400.0F};
    w->layout(c, ctx);
}

}  // namespace

AURORA_TEST() {
    // ==================== Date ====================

    // ---- 1. Date 合法性与闰年 ----
    {
        AURORA_TEST_CHECK(Date{.year = 2026, .month = 7, .day = 25}.is_valid());
        AURORA_TEST_CHECK(!Date{.year = 2026, .month = 13, .day = 1}.is_valid());
        AURORA_TEST_CHECK(!Date{.year = 2026, .month = 2, .day = 29}.is_valid());  // 平年
        AURORA_TEST_CHECK(Date{.year = 2024, .month = 2, .day = 29}.is_valid());  // 闰年
        AURORA_TEST_CHECK(!Date{.year = 2100, .month = 2, .day = 29}.is_valid());  // 百年非闰
        AURORA_TEST_CHECK(Date{.year = 2000, .month = 2, .day = 29}.is_valid());  // 400 年闰
        AURORA_TEST_CHECK(Date{.year = 2026, .month = 7, .day = 25}.to_string() == "2026-07-25");
        AURORA_TEST_CHECK(Date::days_in_month(2026, 4) == 30);
    }

    // ---- 2. DatePicker 选中与回调 ----
    {
        auto dp = std::make_shared<DatePicker>(Date{.year = 2026, .month = 7, .day = 25});
        AURORA_TEST_CHECK(dp->selected_date() == (Date{2026, 7, 25}));
        AURORA_TEST_CHECK(dp->view_year() == 2026 && dp->view_month() == 7);

        Date last{};
        dp->set_on_change([&last](Date d) -> void { last = d; });
        dp->select(Date{.year = 2026, .month = 8, .day = 1});
        AURORA_TEST_CHECK(dp->selected_date() == (Date{.year = 2026, .month = 8, .day = 1}));
        AURORA_TEST_CHECK(last == (Date{.year = 2026, .month = 8, .day = 1}));
        AURORA_TEST_CHECK(dp->view_month() == 8);  // 视图跟随

        // 非法忽略
        dp->select(Date{.year = 2026, .month = 2, .day = 30});
        AURORA_TEST_CHECK(dp->selected_date() == (Date{2026, 8, 1}));
    }

    // ---- 3. 翻月（跨年回卷）----
    {
        auto dp = std::make_shared<DatePicker>(Date{.year = 2026, .month = 12, .day = 15});
        dp->next_month();
        AURORA_TEST_CHECK(dp->view_year() == 2027 && dp->view_month() == 1);
        dp->prev_month();
        AURORA_TEST_CHECK(dp->view_year() == 2026 && dp->view_month() == 12);
    }

    // ---- 4. grid_day 网格映射（2026-07-01 是周三，weekday=3）----
    {
        auto dp = std::make_shared<DatePicker>(Date{.year = 2026, .month = 7, .day = 25});
        AURORA_TEST_CHECK(dp->grid_day(0, 3) == 1);  // 第一行周三 = 1 号
        AURORA_TEST_CHECK(dp->grid_day(0, 2) == 0);  // 周二为空
        // 最后一天 31 号存在
        bool found31 = false;
        for (int r = 0; r < 6 && !found31; ++r) {
            for (int c = 0; c < 7; ++c) {
                if (dp->grid_day(r, c) == 31) {
                    found31 = true;
                }
            }
        }
        AURORA_TEST_CHECK(found31);
    }

    // ---- 5. DatePicker 序列化往返 ----
    {
        auto dp = std::make_shared<DatePicker>(Date{.year = 2025, .month = 3, .day = 9});
        aurora::Json props;
        dp->serialize_props(props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["year"].get<int>() == 2025);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["month"].get<int>() == 3);

        auto dp2 = std::make_shared<DatePicker>();
        dp2->deserialize_props(props);
        AURORA_TEST_CHECK(dp2->selected_date() == (Date{.year = 2025, .month = 3, .day = 9}));
    }

    // ==================== TimePicker ====================

    // ---- 6. 基本调节与回卷 ----
    {
        auto tp = std::make_shared<TimePicker>(TimeOfDay{.hour = 23, .minute = 58});
        AURORA_TEST_CHECK(tp->selected_time().to_string() == "23:58");

        tp->add_minutes(3);  // 23:58 + 3 = 00:01
        AURORA_TEST_CHECK(tp->selected_time() == (TimeOfDay{.hour = 0, .minute = 1}));

        tp->add_hours(-1);  // 00:01 - 1h = 23:01
        AURORA_TEST_CHECK(tp->selected_time() == (TimeOfDay{.hour = 23, .minute = 1}));

        tp->add_minutes(-2);  // 23:01 - 2m = 22:59
        AURORA_TEST_CHECK(tp->selected_time() == (TimeOfDay{.hour = 22, .minute = 59}));
    }

    // ---- 7. 点击交互（左上=时+1，右下=分-1）----
    {
        auto tp = std::make_shared<TimePicker>(TimeOfDay{.hour = 10, .minute = 30});
        layout_widget(tp);

        MouseEvent e1;
        e1.action = MouseAction::Press;
        e1.local_position = Point{.x = 20.0F, .y = 10.0F};  // 左上：时 +1
        tp->on_pointer_event(e1);
        AURORA_TEST_CHECK(tp->selected_time().hour == 11);

        MouseEvent e2;
        e2.action = MouseAction::Press;
        e2.local_position = Point{.x = 100.0F, .y = 60.0F};  // 右下：分 -1
        tp->on_pointer_event(e2);
        AURORA_TEST_CHECK(tp->selected_time().minute == 29);
    }

    // ---- 8. TimePicker 序列化与回调 ----
    {
        auto tp = std::make_shared<TimePicker>(TimeOfDay{.hour = 8, .minute = 15});
        TimeOfDay last{};
        tp->set_on_change([&last](TimeOfDay t) -> void { last = t; });
        tp->select(TimeOfDay{.hour = 9, .minute = 45});
        AURORA_TEST_CHECK(last == (TimeOfDay{.hour = 9, .minute = 45}));

        aurora::Json props;
        tp->serialize_props(props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["hour"].get<int>() == 9);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["minute"].get<int>() == 45);
    }

    // ==================== ColorPicker ====================

    // ---- 9. 默认色板与选择 ----
    {
        auto cp = std::make_shared<ColorPicker>();
        AURORA_TEST_CHECK(cp->palette().size() == 16);

        Color last{};
        cp->set_on_change([&last](Color c) -> void { last = c; });
        cp->select(Color(220, 53, 69, 255));
        AURORA_TEST_CHECK(cp->selected_color().m_r == 220);
        AURORA_TEST_CHECK(last.m_r == 220);

        // 相同颜色不重复回调
        last = Color{};
        cp->select(Color(220, 53, 69, 255));
        AURORA_TEST_CHECK(last.m_r == 0);
    }

    // ---- 10. 点击色板格选色（8 列 28dp 网格）----
    {
        auto cp = std::make_shared<ColorPicker>();
        layout_widget(cp);

        // 点击第二行第一格（index 8 = 青色 Color(0,190,190)）
        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{.x = 10.0F, .y = 40.0F};  // row=1, col=0
        cp->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(cp->selected_color().m_g == 190);

        // 自定义色板
        cp->set_palette({Color(1, 2, 3, 255), Color(4, 5, 6, 255)});
        AURORA_TEST_CHECK(cp->palette().size() == 2);
    }

    // ---- 11. 渲染与序列化 ----
    {
        auto dp = std::make_shared<DatePicker>(Date{.year = 2026, .month = 7, .day = 25});
        auto cp = std::make_shared<ColorPicker>(Color(0, 122, 255, 255));
        layout_widget(dp);
        layout_widget(cp);

        aurora::Painter p;
        p.begin(400, 400);
        BuildContext ctx;
        dp->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 224.0F, .height = 200.0F}}, ctx);
        cp->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 220.0F}, .size = Size{.width = 224.0F, .height = 56.0F}},
                  ctx);
        AURORA_TEST_CHECK(p.width() == 400);

        aurora::Json props;
        cp->serialize_props(props);
        auto cp2 = std::make_shared<ColorPicker>();
        cp2->deserialize_props(props);
        AURORA_TEST_CHECK(cp2->selected_color().m_b == 255);
    }
}