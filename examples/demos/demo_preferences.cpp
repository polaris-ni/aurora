// Preferences 演示：可编辑开关持有本地 State，写入经 prefs.set 主动 flush 到 JSON 文件；
// 另一枚只读 watch 句柄订阅同一配置项，作为「持久化值」的展示面。
// 重新启动应用时，Switch 会自动反映上次持久化的值（构造时从文件加载）。
//
// 另演示可靠删除路径：点击「删除 dark_mode 配置项」调用 prefs.remove("dark_mode")
// （墓碑语义，随 flush 持久化并跨进程传播），随后隐藏绑定的 Switch；删除后重建 watch
// 句柄会拿到 fallback（旧句柄保留最后值，不悬垂）。

#include <filesystem>
#include <string>

#include "aurora/preferences/preferences.h"
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    try {
        const auto dir = std::filesystem::temp_directory_path() / "aurora_prefs_demo";
        std::filesystem::create_directories(dir);
        au::preferences::Preferences prefs = au::preferences::Preferences::with_location("demo_settings", dir);

        auto show_switch = au::State<bool>{true};

        // 可编辑开关：本地 State 驱动绘制；写入经 prefs.set 落内存、flush 落盘。
        auto dark_mode = au::State<bool>{prefs.get<bool>("dark_mode", false)};
        au::Switch sw{dark_mode, [&prefs](bool v) -> void {
                          prefs.set("dark_mode", v);
                          (void)prefs.flush();  // 主动刷新到文件
                      }};

        // 只读 watch：订阅持久化值的展示面（watch 无写入口，修改只走 prefs.set）。
        const auto persisted = prefs.watch<bool>("dark_mode", false);
        const auto persisted_line = au::computed([persisted]() -> au::LocalizedString {
            return au::LocalizedString{"persisted dark_mode = " + std::string{persisted.get() ? "true" : "false"}};
        });

        // 分组演示：把界面偏好放到命名分组 "appearance" 下，以嵌套 JSON 持久化。
        auto appearance = au::State<bool>{prefs.group("appearance").get<bool>("dark_mode", false)};
        au::Switch appearance_sw{appearance, [&prefs](bool v) -> void {
                                     prefs.group("appearance").set("dark_mode", v);
                                     (void)prefs.flush();
                                 }};

        // 删除该配置项按钮：移除 dark_mode 并落盘，随即隐藏 Switch（避免继续写已删除的键）。
        au::Button del_btn{au::ButtonProps{.label = au::LocalizedString{"Delete dark_mode config item"}}};
        del_btn.on_click = [&prefs, show_switch]() -> void {
            prefs.remove("dark_mode");
            (void)prefs.flush();  // 墓碑随 flush 持久化并跨进程传播
            show_switch.set(false);  // 隐藏可编辑开关，避免继续写已删除的键
        };

        au::Node root = au::Column{
            GradientTitle{"Preferences demo"},
            gap(12.0F),
            au::Show{show_switch, au::Row{std::move(sw), au::Text{"Dark mode (persisted in " +
                                                                  prefs.file_path().filename().string() + ")"}}},
            gap(4.0F),
            au::Text{au::TextProps{.content = persisted_line}},
            gap(8.0F),
            au::Row{std::move(appearance_sw), au::Text{"Dark mode (group appearance, nested persistence)"}},
            gap(8.0F),
            std::move(del_btn),
            gap(8.0F),
            au::Text{"After clicking delete, dark_mode is removed from the config file (tombstone-safe deletion); "
                     "restart app to restore default switch state."},
            au::Text{"Re-run after closing the window, undeleted switch states will be restored"},
        };
        return run_demo(Card{std::move(root)}, "Preferences · Aurora Demo", 560.0F, 440.0F);
    } catch (const std::exception &e) {
        AURORA_LOG_ERROR("demo_preferences", "unhandled exception: ", e.what());
        return 1;
    } catch (...) {
        AURORA_LOG_ERROR("demo_preferences", "unhandled unknown exception");
        return 1;
    }
}
