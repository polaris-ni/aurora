/// 测试类型: unit
/// 目标单元: include/aurora/app/file_dialog.h
/// 测试说明: 覆盖 Filter/Options 配置结构体默认值与聚合构造、headless 注入钩子
/// （open/save/folder 预设返回）与 interactive=false 取消语义、钩子优先级；
/// 真实系统对话框路径不测（原生 API），全部走钩子/非交互纯逻辑

#include <string>
#include <vector>

#include "aurora/app/file_dialog.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_file_dialog {

namespace {

/// RAII 恢复 headless 钩子与交互开关，避免污染同进程内后续用例。
struct HookGuard {
    HookGuard() = default;
    HookGuard(const HookGuard&) = delete;
    auto operator=(const HookGuard&) -> HookGuard& = delete;
    HookGuard(HookGuard&&) = delete;
    auto operator=(HookGuard&&) -> HookGuard& = delete;
    ~HookGuard() {
        aurora::file_dialog::headless_open_result.clear();
        aurora::file_dialog::headless_save_result.clear();
        aurora::file_dialog::headless_folder_result.clear();
        aurora::file_dialog::interactive = true;
    }
};

}  // namespace

AURORA_TEST_CASE(filter_and_options_struct_defaults) {
    // Filter：名称 + 扩展名列表的聚合结构。
    const aurora::file_dialog::Filter f{.name = "Images", .extensions = {"*.png", "*.jpg"}};
    AURORA_TEST_CHECK_STREQ(f.name, "Images");
    AURORA_TEST_CHECK_EQ(f.extensions.size(), 2U);
    AURORA_TEST_CHECK_STREQ(f.extensions[0], "*.png");
    AURORA_TEST_CHECK_STREQ(f.extensions[1], "*.jpg");

    // Options：全空默认值。
    const aurora::file_dialog::Options def;
    AURORA_TEST_CHECK_TRUE(def.title.empty());
    AURORA_TEST_CHECK_TRUE(def.initial_dir.empty());
    AURORA_TEST_CHECK_TRUE(def.filters.empty());

    // Options 聚合构造携带过滤器。
    const aurora::file_dialog::Options opts{.title = "打开文件", .initial_dir = "C:/", .filters = {f}};
    AURORA_TEST_CHECK_STREQ(opts.title, "打开文件");
    AURORA_TEST_CHECK_STREQ(opts.initial_dir, "C:/");
    AURORA_TEST_REQUIRE_EQ(opts.filters.size(), 1U);
    AURORA_TEST_CHECK_STREQ(opts.filters[0].name, "Images");
}

AURORA_TEST_CASE(open_file_returns_headless_preset) {
    HookGuard guard;
    aurora::file_dialog::headless_open_result = {"a.png", "b.jpg"};

    // 钩子非空 → 直接返回预设（opts 仅作配置载体，不触发真实对话框）。
    const aurora::file_dialog::Options opts{.title = "Open", .filters = {{.name = "Images", .extensions = {"*.png"}}}};
    const auto r = aurora::file_dialog::open_file(opts);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().size(), 2U);
    AURORA_TEST_CHECK_STREQ(r.value()[0], "a.png");
    AURORA_TEST_CHECK_STREQ(r.value()[1], "b.jpg");
}

AURORA_TEST_CASE(save_and_folder_headless_presets) {
    HookGuard guard;

    aurora::file_dialog::headless_save_result = "out.txt";
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_STREQ(saved.value(), "out.txt");

    aurora::file_dialog::headless_folder_result = "C:/work";
    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_STREQ(folder.value(), "C:/work");
}

AURORA_TEST_CASE(non_interactive_behaves_as_cancel) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;

    // 钩子为空 + 非交互 → 等价取消（空结果，不抛错、不弹窗）。
    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_CHECK_TRUE(opened.value().empty());

    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_TRUE(saved.value().empty());

    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_TRUE(folder.value().empty());
}

AURORA_TEST_CASE(headless_hook_takes_priority_over_interactive) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;

    // 钩子优先于 interactive 开关：预设值照常返回。
    aurora::file_dialog::headless_open_result = {"preset.bin"};
    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_CHECK_EQ(opened.value().size(), 1U);
    AURORA_TEST_CHECK_STREQ(opened.value()[0], "preset.bin");

    aurora::file_dialog::headless_save_result = "hooked.log";
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_STREQ(saved.value(), "hooked.log");
}

}  // namespace aurora::test_cases::utest_file_dialog
