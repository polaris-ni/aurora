// 标题栏单测：TitleBarStyle 默认值/预设、title_bar_geometry 真值表
// （三布局尺寸规则、隐藏收缩、resizable 自动隐藏、窄窗退化、maximized 无关性）。
// 纯函数测试，无后端依赖。期望值以 src/aurora/window/title_bar_geometry.cpp
// 顶部注释块（唯一权威来源）推导；如与实现冲突以实现为准并回填本文件。
#include <memory>

#include "aurora/widget/title_bar.h"
#include "aurora/window/surface.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "test_harness.h"

namespace au = aurora;

namespace {

auto approx(float a, float b, const float eps = 0.5F) -> bool { return std::fabs(a - b) <= eps; }

auto empty_rect(const au::Rect &r) -> bool { return r.size.width == 0.0F && r.size.height == 0.0F; }

auto same_style(const au::TitleBarStyle &a, const au::TitleBarStyle &b) -> bool {
    return a.height == b.height && a.bg_active == b.bg_active && a.bg_inactive == b.bg_inactive &&
           a.fg_active == b.fg_active && a.fg_inactive == b.fg_inactive && a.hover_tint == b.hover_tint &&
           a.close_hover == b.close_hover && a.button_layout == b.button_layout && a.show_minimize == b.show_minimize &&
           a.show_maximize == b.show_maximize && a.show_close == b.show_close && a.show_title == b.show_title &&
           a.center_title == b.center_title;
}

}  // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_title_bar ===\n");

    // ---- 1. 样式默认值与预设 ----
    {
        au::TitleBarStyle def;
        AURORA_TEST_CHECK(same_style(def, au::TitleBarStyle::adwaita_dark()));
        AURORA_TEST_CHECK(def.button_layout == au::TitleBarButtonLayout::Adwaita);
        AURORA_TEST_CHECK(def.height == 36.0F);
        AURORA_TEST_CHECK(def.show_minimize && def.show_maximize && def.show_close);
        AURORA_TEST_CHECK(def.show_title && !def.center_title);

        constexpr auto light = au::TitleBarStyle::adwaita_light();
        constexpr auto win = au::TitleBarStyle::windows_dark();
        AURORA_TEST_CHECK(light.bg_active != def.bg_active || light.fg_active != def.fg_active);  // 明暗可辨
        AURORA_TEST_CHECK(win.button_layout == au::TitleBarButtonLayout::Windows);

        auto custom = au::TitleBarStyle{};
        custom.height = 40.0F;
        custom.center_title = true;
        AURORA_TEST_CHECK(custom.height == 40.0F && custom.center_title);  // 聚合初始化后可改
    }

    // ---- 2. WindowStyleOptions 集成 ----
    {
        au::WindowStyleOptions opts;
        AURORA_TEST_CHECK(opts.title_bar.height == 36.0F);
        AURORA_TEST_CHECK(opts.title_bar.button_layout == au::TitleBarButtonLayout::Adwaita);
    }

    // ---- 3. Adwaita 布局真值表（width=800, height=36）----
    // btn=clamp(36-10,22,30)=26；gap=4；右缘外边距8。
    {
        constexpr au::TitleBarStyle s;
        const auto g = au::title_bar_geometry(800.0F, s, false, true);
        // 从右往左：close 右边界 800-8=792，盒宽 26 → 左缘 766；垂直居中 y=(36-26)/2=5。
        AURORA_TEST_CHECK(approx(g.close.origin.x, 766.0F) && approx(g.close.origin.y, 5.0F));
        AURORA_TEST_CHECK(approx(g.close.size.width, 26.0F) && approx(g.close.size.height, 26.0F));
        // max = close 左移 (26+4)；min 再左移同距。
        AURORA_TEST_CHECK(approx(g.maximize.origin.x, 736.0F));
        AURORA_TEST_CHECK(approx(g.minimize.origin.x, 706.0F));
        // 图标槽：边长 min(16,36-20)=16，x=12，垂直居中 y=10。
        AURORA_TEST_CHECK(approx(g.icon.origin.x, 12.0F) && approx(g.icon.size.width, 16.0F) &&
                          approx(g.icon.origin.y, 10.0F));
        // 标题区：左=图标右+8=36；右=按钮组内缘-8=706-8=698；全高。
        AURORA_TEST_CHECK(approx(g.title.origin.x, 36.0F) && approx(g.title.size.width, 662.0F));
        AURORA_TEST_CHECK(approx(g.title.origin.y, 0.0F) && approx(g.title.size.height, 36.0F));
    }

    // ---- 4. 隐藏按钮：贴边收缩补位 ----
    {
        au::TitleBarStyle s;
        s.show_minimize = false;
        const auto g = au::title_bar_geometry(800.0F, s, false, true);
        AURORA_TEST_CHECK(empty_rect(g.minimize));  // 显式隐藏 → 空盒
        AURORA_TEST_CHECK(approx(g.close.origin.x, 766.0F));  // 其余按钮位置不变（锚定贴边侧）
        AURORA_TEST_CHECK(approx(g.maximize.origin.x, 736.0F));

        s.show_maximize = false;
        const auto g2 = au::title_bar_geometry(800.0F, s, false, true);
        AURORA_TEST_CHECK(empty_rect(g2.maximize));
        AURORA_TEST_CHECK(empty_rect(g2.minimize));
    }

    // ---- 5. resizable=false → maximize 自动隐藏，min 收缩 ----
    {
        constexpr au::TitleBarStyle s;
        const auto g = au::title_bar_geometry(800.0F, s, false, false);
        AURORA_TEST_CHECK(empty_rect(g.maximize));
        // close 后下一个是 min：762-26=736。
        AURORA_TEST_CHECK(approx(g.minimize.origin.x, 736.0F));
        AURORA_TEST_CHECK(approx(g.close.origin.x, 766.0F));
    }

    // ---- 6. Windows 布局：整高无缝、贴右上角 ----
    {
        au::TitleBarStyle s = au::TitleBarStyle::windows_dark();
        const auto g = au::title_bar_geometry(800.0F, s, false, true);
        constexpr float w = 52.0F;  // round(36*1.44)
        AURORA_TEST_CHECK(approx(g.close.origin.x + g.close.size.width, 800.0F));  // 贴右缘
        AURORA_TEST_CHECK(approx(g.close.origin.y, 0.0F) && approx(g.close.size.height, 36.0F));  // 整高
        AURORA_TEST_CHECK(approx(g.close.size.width, w) && approx(g.close.origin.x, 748.0F));
        AURORA_TEST_CHECK(approx(g.maximize.origin.x, 696.0F));  // 无缝：直接 -52
        AURORA_TEST_CHECK(approx(g.minimize.origin.x, 644.0F));
    }

    // ---- 7. Mac 布局：左侧系 close→min→max ----
    {
        au::TitleBarStyle s;
        s.button_layout = au::TitleBarButtonLayout::Mac;
        const auto g = au::title_bar_geometry(800.0F, s, false, true);
        constexpr float d = 14.0F;  // clamp(36*0.4,11,14)
        AURORA_TEST_CHECK(approx(g.close.origin.x, 8.0F));  // 左缘外边距 8
        AURORA_TEST_CHECK(approx(g.close.size.width, d) && approx(g.close.size.height, d));
        AURORA_TEST_CHECK(approx(g.close.origin.y, 11.0F));  // (36-14)/2
        AURORA_TEST_CHECK(approx(g.minimize.origin.x, 30.0F));  // 8+14+8
        AURORA_TEST_CHECK(approx(g.maximize.origin.x, 52.0F));  // 30+14+8
        // 图标排在按钮组右侧 +12：group_inner_edge=max 右缘 52+14=66 → x=78。
        AURORA_TEST_CHECK(approx(g.icon.origin.x, 78.0F));
        // 标题区镜像：右界 width-8；左界图标右+8=78+16+8=102。
        AURORA_TEST_CHECK(approx(g.title.origin.x, 102.0F) && approx(g.title.size.width, 690.0F));
    }

    // ---- 8. center_title：整宽标题区 ----
    {
        au::TitleBarStyle s;
        s.center_title = true;
        const auto g = au::title_bar_geometry(800.0F, s, false, true);
        AURORA_TEST_CHECK(approx(g.title.origin.x, 0.0F) && approx(g.title.size.width, 800.0F));
    }

    // ---- 9. 窄窗退化：不越界，title 允许为空 ----
    {
        constexpr au::TitleBarStyle s;
        const auto g = au::title_bar_geometry(120.0F, s, false, true);
        AURORA_TEST_CHECK(!empty_rect(g.close) && g.close.origin.x >= 0.0F &&
                          g.close.origin.x + g.close.size.width <= 120.0F);
        AURORA_TEST_CHECK(!empty_rect(g.maximize) && g.maximize.origin.x >= 0.0F);
        AURORA_TEST_CHECK(empty_rect(g.title));  // left=36 > right=18 → 空盒
    }

    // ---- 10. maximized 参数不影响按钮盒 ----
    {
        constexpr au::TitleBarStyle s;
        const auto g1 = au::title_bar_geometry(800.0F, s, false, true);
        const auto g2 = au::title_bar_geometry(800.0F, s, true, true);
        AURORA_TEST_CHECK(approx(g1.maximize.origin.x, g2.maximize.origin.x));
        AURORA_TEST_CHECK(approx(g1.close.origin.x, g2.close.origin.x));
    }

    // ---- 11. TitleBar 控件：描述/序列化往返/Snap 计数/事件安全性 ----
    {
        au::TitleBar bar;
        AURORA_TEST_CHECK(std::string(bar.type_name()) == "TitleBar");
        AURORA_TEST_CHECK(bar.describe_static().name == "TitleBar");
        AURORA_TEST_CHECK(bar.window_controls());
        AURORA_TEST_CHECK(bar.snap_entry_count() == 4);  // 内置：最大化还原/最小化/全屏/关闭

        // 序列化往返。
        bar.set_title("文档").set_subtitle("已修改").set_window_controls(false).set_height(44.0F);
        nlohmann::json j;
        bar.serialize_props(j);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["title"] == "文档");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["window_controls"] == false);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(approx(j["height"], 44.0F));

        au::TitleBar bar2;
        bar2.deserialize_props(j);
        AURORA_TEST_CHECK(bar2.title() == "文档");
        AURORA_TEST_CHECK(bar2.subtitle() == "已修改");
        AURORA_TEST_CHECK(!bar2.window_controls());
        nlohmann::json j2;
        bar2.serialize_props(j2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(approx(j2["height"], 44.0F));

        // Snap 自定义项计数（回调不可序列化，仅计数入 JSON）。
        int fired = 0;
        bar.add_snap_action({.label = "平铺", .on_click = [&] -> void { ++fired; }});
        AURORA_TEST_CHECK(bar.snap_entry_count() == 5);

        // 无环境（headless）：空白区按下安全 no-op 且消费事件（chrome 缺失不崩溃）。
        au::MouseEvent press;
        press.action = au::MouseAction::Press;
        press.button = au::MouseButton::Left;
        press.local_position = au::Point{.x = 200.0F, .y = 10.0F};
        bar.on_pointer_event(press);
        AURORA_TEST_CHECK(press.is_handled_);
    }
}