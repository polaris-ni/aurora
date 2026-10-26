/// 测试类型: unit
/// 目标单元: include/aurora/app/window_geometry.h
/// 测试说明: 窗口几何持久化（specification/06-app-platform.md §2.4）——JSON 往返、缺字段/类型不符与
/// 枚举越界的拒绝、尺寸非正与完全屏幕外几何的可用性判定、经 Preferences 的保存/读取往返、
/// 以及多窗口「窗口组」用不同键区分。

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/display.h"
#include "aurora/app/window_geometry.h"
#include "aurora/core/log.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"
#include "framework/json_literals.h"

namespace aurora::test_cases::utest_window_geometry {

using aurora::testing::json_obj;
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

/// @brief RAII：接管 Logger sink 捕获日志行；析构恢复级别与默认 stderr，
/// 不把 Logger 全局状态泄漏给其他用例（与 utest_log 的 CapturedLogger 同构，本 TU 独立一份）。
class CapturedLog {
  public:
    CapturedLog() : level_{aurora::Logger::instance().level()} {
        aurora::Logger::instance().set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLog() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_sink(nullptr);  // 恢复默认 stderr
    }
    CapturedLog(const CapturedLog &) = delete;
    auto operator=(const CapturedLog &) & = delete;
    CapturedLog(CapturedLog &&) = delete;
    auto operator=(CapturedLog &&) & = delete;

    /// @brief 已捕获行里是否有一条同时含全部给定片段（级别标签 + 文案）。
    [[nodiscard]] auto has_line_with(std::initializer_list<const char *> fragments) const -> bool {
        for (const std::string &line : lines) {
            bool all = true;
            for (const char *f : fragments) {
                if (line.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> lines;  // NOLINT(*-non-private-member-variables-in-classes) 捕获到的日志行

  private:
    aurora::LogLevel level_;
};

/// @brief 造一枚带 persist_id 的无头窗口（几何恢复用例底座）。
auto make_persisted_window(const std::string &persist_id, float w, float h) -> std::unique_ptr<Window> {
    WindowOptions o;
    o.size = Size{.width = w, .height = h};
    o.persist_id = persist_id;
    HeadlessOptions ho;
    static_cast<WindowOptions &>(ho) = o;
    auto win = create_window(ho);
    if (!win.ok()) {
        return nullptr;
    }
    return std::move(win.value());
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
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(json::Value{42}).has_value());
    // 缺字段（只有部分键）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(json_obj({{"origin_x", 0.0}})).has_value());
    // 类型不符（字符串代替数值）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(json_obj({
                                                          {"origin_x", "x"},
                                                          {"origin_y", 0.0},
                                                          {"width", 10.0},
                                                          {"height", 10.0},
                                                          {"mode", 0},
                                                          {"display_id", -1},
                                                      }))
                                .has_value());
    // 枚举越界（mode=99）。
    AURORA_TEST_CHECK_FALSE(window_geometry_from_json(json_obj({
                                                          {"origin_x", 0.0},
                                                          {"origin_y", 0.0},
                                                          {"width", 10.0},
                                                          {"height", 10.0},
                                                          {"mode", 99},
                                                          {"display_id", -1},
                                                      }))
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

AURORA_TEST_CASE(geometry_json_round_trip_covers_fullscreen_and_minimized) {
    // 回归保护（不占验收名额）：六键稳定，FullScreen / Minimized 两档与既有 Maximized 同权往返。
    for (const WindowMode m : {WindowMode::FullScreen, WindowMode::Minimized}) {
        WindowGeometry g = usable_geometry();
        g.mode = m;
        const WindowGeometry parsed = require_value(window_geometry_from_json(window_geometry_to_json(g)));
        AURORA_TEST_CHECK_EQ(static_cast<int>(parsed.mode), static_cast<int>(m));
    }
}

AURORA_TEST_CASE(application_restores_window_mode_from_storage) {
    const Display d = app::primary_display();

    // 同一底座参数化三档：Maximized / FullScreen（只追几何态）与 Normal（位置尺寸照常）。
    // expect_size_restored = 落盘的位置与尺寸是否应下发到 surface。
    const auto run_for = [d](WindowMode stored_mode, bool expect_size_restored) -> void {
        preferences::Preferences prefs;  // 仅内存模式
        WindowGeometry stored = usable_geometry();
        stored.mode = stored_mode;
        save_window_geometry(prefs, "main", stored);
        save_window_geometry(prefs, "aux", stored);

        auto win = make_persisted_window("main", 320.0F, 240.0F);
        AURORA_TEST_REQUIRE_NOT_NULL(win.get());
        auto *const surface = dynamic_cast<HeadlessSurface *>(&win->surface());
        AURORA_TEST_REQUIRE_NOT_NULL(surface);

        WindowOptions o;
        o.size = Size{.width = 320.0F, .height = 240.0F};
        o.persist_id = "main";
        o.max_frames = 2;
        o.power_saving = false;
        Application app{Scene{Node{std::make_shared<Text>("main")}}, std::move(win), o};

        auto win2 = make_persisted_window("aux", 320.0F, 240.0F);
        AURORA_TEST_REQUIRE_NOT_NULL(win2.get());
        auto *const surface2 = dynamic_cast<HeadlessSurface *>(&win2->surface());
        AURORA_TEST_REQUIRE_NOT_NULL(surface2);
        WindowOptions o2 = o;
        o2.persist_id = "aux";
        const WindowId id_aux = app.open_window(std::move(win2), Scene{Node{std::make_shared<Text>("aux")}}, o2);
        // 存储晚于两窗登记：均经 set_window_geometry_store 补做恢复。
        app.set_window_geometry_store(&prefs);

        // 主窗断言双层：应用级 window_mode State（mode sink 只聚合主窗）+ 宿主快照。
        AURORA_TEST_CHECK_EQ(app.window_mode().get(), stored_mode);
        WindowHost *const main_host = app.window_host(app.main_window());
        AURORA_TEST_REQUIRE_NOT_NULL(main_host);
        AURORA_TEST_CHECK_EQ(main_host->window()->window_mode(), stored_mode);
        // 第二窗只断言宿主快照：register_host 的 mode sink 仅在 raw == main_host_ 时转发，
        // 非主窗的几何态永不进应用级 State（既有口径，对两层同时断言必红）。
        WindowHost *const aux_host = app.window_host(id_aux);
        AURORA_TEST_REQUIRE_NOT_NULL(aux_host);
        AURORA_TEST_CHECK_EQ(aux_host->window()->window_mode(), stored_mode);

        if (expect_size_restored) {
            // Normal：位置与尺寸照常恢复（headless 下 scale = 1，逻辑尺寸 == 物理尺寸）。
            AURORA_TEST_CHECK_NEAR(surface->size().width, stored.size.width, 0.001);
            AURORA_TEST_CHECK_NEAR(surface->position().x, stored.origin.x, 0.001);
            AURORA_TEST_CHECK_NEAR(surface2->position().x, stored.origin.x, 0.001);
        } else {
            // Maximized / FullScreen：位置与尺寸都不下发，两窗 surface 均保持创建默认值。
            AURORA_TEST_CHECK_NEAR(surface->size().width, 320.0F, 0.001);
            AURORA_TEST_CHECK_NEAR(surface->size().height, 240.0F, 0.001);
            AURORA_TEST_CHECK_NEAR(surface->position().x, 0.0F, 0.001);
            AURORA_TEST_CHECK_NEAR(surface->position().y, 0.0F, 0.001);
            AURORA_TEST_CHECK_NEAR(surface2->size().width, 320.0F, 0.001);
            AURORA_TEST_CHECK_NEAR(surface2->position().x, 0.0F, 0.001);
        }
        (void)d;
    };
    run_for(WindowMode::Maximized, false);
    run_for(WindowMode::FullScreen, false);
    run_for(WindowMode::Normal, true);
}

AURORA_TEST_CASE(minimized_mode_restores_as_normal_with_info_log) {
    CapturedLog log;  // 须先于恢复动作构造，否则捕不到 INFO
    preferences::Preferences prefs;
    WindowGeometry stored = usable_geometry();
    stored.mode = WindowMode::Minimized;  // 落盘最小化
    save_window_geometry(prefs, "main", stored);

    auto win = make_persisted_window("main", 320.0F, 240.0F);
    AURORA_TEST_REQUIRE_NOT_NULL(win.get());
    WindowOptions o;
    o.size = Size{.width = 320.0F, .height = 240.0F};
    o.persist_id = "main";
    o.max_frames = 2;
    o.power_saving = false;
    Application app{Scene{Node{std::make_shared<Text>("main")}}, std::move(win), o};
    app.set_window_geometry_store(&prefs);

    // 政策钉死：Minimized 恢复为 Normal（启动即看不见 + 与 WindowState::Hidden 耦合），
    // 且走 Normal 分支——位置与尺寸照常恢复（用户可见地「打开」）。
    AURORA_TEST_CHECK_EQ(app.window_mode().get(), WindowMode::Normal);
    WindowHost *const host = app.window_host(app.main_window());
    AURORA_TEST_REQUIRE_NOT_NULL(host);
    auto *const surface = dynamic_cast<HeadlessSurface *>(&host->window()->surface());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    AURORA_TEST_CHECK_NEAR(surface->size().width, stored.size.width, 0.001);
    AURORA_TEST_CHECK_NEAR(surface->position().x, stored.origin.x, 0.001);

    // 一条 INFO（tag "app"）可归因：用户最小化是合法操作，记 WARN 会让每次启动刷噪声。
    AURORA_TEST_CHECK_TRUE(log.has_line_with({"INF", "app", "stored window mode is Minimized; restoring as Normal"}));
}

AURORA_TEST_CASE(offscreen_geometry_load_returns_nullopt_with_warn) {
    CapturedLog log;
    preferences::Preferences prefs;
    // 屏外几何：is_window_geometry_usable 宽松判定（尺寸为正 + 与任一 work_area 相交）拒绝 →
    // load 回 nullopt，apply 不执行，mode 天然保持默认 Normal。
    WindowGeometry broken = usable_geometry();
    broken.origin = Point{.x = -50000.0F, .y = -50000.0F};
    save_window_geometry(prefs, "main", broken);
    AURORA_TEST_CHECK_FALSE(load_window_geometry(prefs, "main").has_value());
    AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "app", "stored window geometry is off-screen"}));
}

AURORA_TEST_CASE(missing_display_degrades_mode_to_normal_with_warn) {
    CapturedLog log;
    preferences::Preferences prefs;
    // display_id 指向的显示器已不在：is_window_geometry_usable 完全不看 display_id（宽松口径），
    // load 照常成功；降级判定在恢复腿（apply）——不拒绝恢复，但几何态归 Normal + WARN。
    WindowGeometry stored = usable_geometry();
    stored.mode = WindowMode::Maximized;
    stored.display_id = 9999;  // 任何当前显示器之外的 id
    save_window_geometry(prefs, "main", stored);

    auto win = make_persisted_window("main", 320.0F, 240.0F);
    AURORA_TEST_REQUIRE_NOT_NULL(win.get());
    WindowOptions o;
    o.size = Size{.width = 320.0F, .height = 240.0F};
    o.persist_id = "main";
    o.max_frames = 2;
    o.power_saving = false;
    Application app{Scene{Node{std::make_shared<Text>("main")}}, std::move(win), o};
    app.set_window_geometry_store(&prefs);

    AURORA_TEST_CHECK_EQ(app.window_mode().get(), WindowMode::Normal);
    WindowHost *const host = app.window_host(app.main_window());
    AURORA_TEST_REQUIRE_NOT_NULL(host);
    auto *const surface = dynamic_cast<HeadlessSurface *>(&host->window()->surface());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);
    // 归 Normal 后位置与尺寸照常恢复（物理坐标相交就摆得回）。
    AURORA_TEST_CHECK_NEAR(surface->size().width, stored.size.width, 0.001);
    AURORA_TEST_CHECK_NEAR(surface->position().x, stored.origin.x, 0.001);
    AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "app", "stored window display is gone; restoring as Normal"}));
}

AURORA_TEST_CASE(out_of_range_mode_via_store_is_rejected) {
    preferences::Preferences prefs;
    // 经 store 的端到端越界：直接往存储写一条 mode=99 的 JSON（模拟损坏/手改文件），
    // load 必须拒（格式错误），恢复腿不执行。
    prefs.set<json::Value>("main", json_obj({
                                       {"origin_x", 10.0},
                                       {"origin_y", 10.0},
                                       {"width", 640.0},
                                       {"height", 480.0},
                                       {"mode", 99},
                                       {"display_id", -1},
                                   }));
    AURORA_TEST_CHECK_FALSE(load_window_geometry(prefs, "main").has_value());
}

}  // namespace aurora::test_cases::utest_window_geometry
