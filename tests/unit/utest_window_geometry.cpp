/// 测试类型: unit
/// 目标单元: include/aurora/app/window_geometry.h
/// 测试说明: 窗口几何持久化（specification/06-app-platform.md §2.4）——JSON 往返、缺字段/类型不符与
/// 枚举越界的拒绝、尺寸非正与完全屏幕外几何的可用性判定、经 Preferences 的保存/读取往返、
/// 以及多窗口「窗口组」用不同键区分。

#include <optional>
#include <string>
#include <utility>

#include "aurora/app/application.h"
#include "aurora/app/display.h"
#include "aurora/app/window_geometry.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_geometry {

using aurora::testing::require_value;

namespace {

/// @brief 构造一个必然可用的几何：取主显示器工作区内的一小块。
auto usable_geometry() -> WindowGeometry {
    const Display d = app::primary_display();
    return WindowGeometry{
        .origin = Point{.x = d.work_area.origin.x + 40.0F, .y = d.work_area.origin.y + 40.0F},
        .size = Size{.width = 640.0F, .height = 480.0F},
        .mode = WindowMode::Maximized,
        .display_id = d.id,
    };
}

}  // namespace

AURORA_TEST_CASE(geometry_json_round_trip) {
    const WindowGeometry g = usable_geometry();
    const WindowGeometry parsed = require_value(window_geometry_from_json(window_geometry_to_json(g)));

    AURORA_TEST_CHECK_NEAR(parsed.origin.x, g.origin.x, 0.001);
    AURORA_TEST_CHECK_NEAR(parsed.origin.y, g.origin.y, 0.001);
    AURORA_TEST_CHECK_NEAR(parsed.size.width, g.size.width, 0.001);
    AURORA_TEST_CHECK_NEAR(parsed.size.height, g.size.height, 0.001);
    AURORA_TEST_CHECK_EQ(static_cast<int>(parsed.mode), static_cast<int>(WindowMode::Maximized));
    AURORA_TEST_CHECK_EQ(parsed.display_id, g.display_id);
}

AURORA_TEST_CASE(malformed_json_is_rejected) {
    // 非对象。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(Json{42}).has_value());
    // 缺字段（只有部分键）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(Json{{"origin_x", 0.0}}).has_value());
    // 类型不符（字符串代替数值）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(Json{
                                                          {"origin_x", "x"},
                                                          {"origin_y", 0.0},
                                                          {"width", 10.0},
                                                          {"height", 10.0},
                                                          {"mode", 0},
                                                          {"display_id", -1},
                                                      })
                                .has_value());
    // 枚举越界（mode=99）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(Json{
                                                          {"origin_x", 0.0},
                                                          {"origin_y", 0.0},
                                                          {"width", 10.0},
                                                          {"height", 10.0},
                                                          {"mode", 99},
                                                          {"display_id", -1},
                                                      })
                                .has_value());
}

AURORA_TEST_CASE(unusable_geometry_is_rejected) {
    const auto displays = app::list_displays();
    AURORA_TEST_REQUIRE(!displays.empty());

    // 尺寸非正 → 不可用（哪怕位置合法）。
    WindowGeometry zero = usable_geometry();
    zero.size = Size{.width = 0.0F, .height = 0.0F};
    AURORA_TEST_CHECK_FALSE(is_window_geometry_usable(zero, displays));

    // 完全落在屏幕之外（模拟显示器被拔除 / 分辨率变小）→ 不可用。
    WindowGeometry offscreen = usable_geometry();
    offscreen.origin = Point{.x = -100000.0F, .y = -100000.0F};
    AURORA_TEST_CHECK_FALSE(is_window_geometry_usable(offscreen, displays));

    // 正常几何 → 可用；部分越界（左上角在屏外但仍有可见部分）也算可用。
    AURORA_TEST_CHECK_TRUE(is_window_geometry_usable(usable_geometry(), displays));
    WindowGeometry partial = usable_geometry();
    partial.origin = Point{.x = displays.front().work_area.origin.x - 40.0F, .y = -10.0F};
    AURORA_TEST_CHECK_TRUE(is_window_geometry_usable(partial, displays));
}

AURORA_TEST_CASE(save_and_load_via_preferences) {
    preferences::Preferences prefs;  // 未指定文件：仅内存模式（不触碰磁盘）
    const WindowGeometry g = usable_geometry();

    AURORA_TEST_CHECK_FALSE(load_window_geometry(prefs, "main").has_value());  // 首次：无记录
    save_window_geometry(prefs, "main", g);
    const WindowGeometry loaded = require_value(load_window_geometry(prefs, "main"));
    AURORA_TEST_CHECK_NEAR(loaded.size.width, g.size.width, 0.001);
    AURORA_TEST_CHECK_NEAR(loaded.origin.x, g.origin.x, 0.001);

    // 存储了「不可用」几何（显示器已拔除的典型场景）：load 兜底为 nullopt，由调用方回退默认布局。
    WindowGeometry broken = g;
    broken.origin = Point{.x = -50000.0F, .y = -50000.0F};
    save_window_geometry(prefs, "aux", broken);
    AURORA_TEST_CHECK_FALSE(load_window_geometry(prefs, "aux").has_value());
}

AURORA_TEST_CASE(window_group_uses_distinct_keys) {
    preferences::Preferences prefs;
    auto group = prefs.group("windows");  // 窗口组：一组窗口共用一个分组，键区分各窗口

    WindowGeometry main_geo = usable_geometry();
    WindowGeometry aux_geo = usable_geometry();
    aux_geo.origin = Point{.x = main_geo.origin.x + 80.0F, .y = main_geo.origin.y + 60.0F};

    save_window_geometry(group, "main", main_geo);
    save_window_geometry(group, "aux", aux_geo);

    const WindowGeometry l_main = require_value(load_window_geometry(group, "main"));
    const WindowGeometry l_aux = require_value(load_window_geometry(group, "aux"));
    // 两个窗口的几何互不覆盖。
    AURORA_TEST_CHECK_NE(l_main.origin.x, l_aux.origin.x);
    AURORA_TEST_CHECK_FALSE(load_window_geometry(group, "missing").has_value());
}

AURORA_TEST_CASE(application_restores_and_saves_geometry_across_runs) {
    preferences::Preferences prefs;  // 仅内存模式
    const Display d = app::primary_display();
    // 模拟「上次运行结束时保存的几何」：与默认尺寸不同，便于断言恢复确实生效。
    const WindowGeometry stored{
        .origin = Point{.x = d.work_area.origin.x + 120.0F, .y = d.work_area.origin.y + 90.0F},
        .size = Size{.width = 700.0F, .height = 500.0F},
        .mode = WindowMode::Normal,
        .display_id = d.id,
    };
    save_window_geometry(prefs, "main", stored);

    WindowOptions o;
    o.size = Size{.width = 320.0F, .height = 240.0F};  // 默认尺寸：若未恢复则断言会失败
    o.persist_id = "main";
    o.max_frames = 2;
    o.power_saving = false;
    HeadlessOptions ho;
    static_cast<WindowOptions &>(ho) = o;
    auto win = create_window(ho);
    AURORA_TEST_REQUIRE(win.ok());
    auto *const surface = dynamic_cast<HeadlessSurface *>(&win.value()->surface());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    Application app{Scene{Node{std::make_shared<Text>("main")}}, std::move(win.value()), o};
    app.set_window_geometry_store(&prefs);  // 存储晚于主窗口登记 → 应补做一次恢复

    // 打开即恢复：位置与尺寸取自存储（headless 下 scale = 1，逻辑尺寸 == 物理尺寸）。
    AURORA_TEST_CHECK_NEAR(surface->position().x, stored.origin.x, 0.001);
    AURORA_TEST_CHECK_NEAR(surface->position().y, stored.origin.y, 0.001);
    AURORA_TEST_CHECK_NEAR(surface->size().width, stored.size.width, 0.001);
    AURORA_TEST_CHECK_NEAR(surface->size().height, stored.size.height, 0.001);

    // 关闭 → 帧末自动持久化当前几何。注意单窗口场景下宿主会被「保留最后一个」规则留下，
    // 若把保存写在回收分支里就会漏掉这条最典型的路径。
    app.close_window(app.main_window());
    app.run();

    const WindowGeometry saved = require_value(load_window_geometry(prefs, "main"));
    AURORA_TEST_CHECK_NEAR(saved.origin.x, stored.origin.x, 0.001);
    AURORA_TEST_CHECK_NEAR(saved.size.height, stored.size.height, 0.001);
}

}  // namespace aurora::test_cases::utest_window_geometry
