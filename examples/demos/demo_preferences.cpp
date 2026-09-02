// Preferences 演示：将 Switch 绑定到「深色模式」配置项，切换即主动 flush 到 JSON 文件。
// 重新启动应用时，Switch 会自动反映上次持久化的值（构造时从文件加载）。
//
// 另演示 Binding 的可靠删除路径：点击「删除 dark_mode 配置项」经注入的删除回调
// 调用 prefs.remove("dark_mode")（墓碑语义，随 flush 持久化并跨进程传播），随后隐藏
// 绑定的 Switch（避免已销毁的上游 State 被访问），对应「控件卸载时同步 remove」的用例。

#include <filesystem>

#include "aurora/preferences/preferences.h"

#include "demo_common.h"

auto main() -> int {
    try {
        const auto dir = std::filesystem::temp_directory_path() / "aurora_prefs_demo";
        std::filesystem::create_directories(dir);
        au::preferences::Preferences prefs = au::preferences::Preferences::with_location("demo_settings", dir);

        auto show_switch = std::make_shared<au::State<bool>>(true);

        // Switch 双向绑定到 "dark_mode"；切换经 on_change 写回并主动 flush 到文件。
        au::Switch sw{ prefs.binding<bool>("dark_mode", false), [&prefs](bool v) -> void {
                          prefs.set("dark_mode", v);
                          (void)prefs.flush(); // 主动刷新到文件
                      } };

        // 分组演示：把界面偏好放到命名分组 "appearance" 下，以嵌套 JSON 持久化。
        au::Switch appearance_sw{ prefs.group("appearance").binding<bool>("dark_mode", false),
                                  [&prefs](bool v) -> void {
                                      prefs.group("appearance").set("dark_mode", v);
                                      (void)prefs.flush();
                                  } };

        // 删除该配置项按钮：经 Binding 删除路径移除 dark_mode 并落盘，
        // 随即隐藏 Switch（避免已销毁的 State 被复用），演示「控件卸载时同步 remove」。
        au::Button del_btn{ au::ButtonProps{ .label = au::LocalizedString{ "Delete dark_mode config item" } } };
        del_btn.on_click = [&prefs, show_switch]() -> void {
            prefs.remove("dark_mode");
            (void)prefs.flush();     // 墓碑随 flush 持久化并跨进程传播
            show_switch->set(false); // 隐藏绑定的 Switch，避免 dangling State 被访问
        };

        au::Node root = au::Column{
            GradientTitle{ "Preferences demo" },
            gap(12.0f),
            au::Show{ show_switch, au::Row{ std::move(sw), au::Text{ "Dark mode (persisted in " +
                                                                     prefs.file_path().filename().string() + "）" } } },
            gap(8.0f),
            au::Row{ std::move(appearance_sw), au::Text{ "Dark mode (group appearance, nested persistence)" } },
            gap(8.0f),
            std::move(del_btn),
            gap(8.0f),
            au::Text{ "After clicking delete, dark_mode is removed from the config file (tombstone-safe deletion); restart app to restore default switch state." },
            au::Text{ "Re-run after closing the window, undeleted switch states will be restored" },
        };
        return run_demo(Card{ std::move(root) }, "Preferences · Aurora Demo", 560.0f, 420.0f);
    } catch (const std::exception &e) {
        AURORA_LOG_ERROR("demo_preferences", "unhandled exception: ", e.what());
        return 1;
    } catch (...) {
        AURORA_LOG_ERROR("demo_preferences", "unhandled unknown exception");
        return 1;
    }
}
