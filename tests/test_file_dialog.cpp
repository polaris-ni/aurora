// test_file_dialog.cpp — FileDialog 1:1 测试：headless 预设打开/保存/文件夹、
// 取消返回空、Options.filters、多格式无状态断言。
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

namespace file_dialog = aurora::file_dialog;

static void test_file_dialog() {
    // 自动化/headless 环境：关闭真实系统对话框，hook 为空时直接返回空（等价取消），
    // 避免 CTest 在 Windows 上卡在 GUI 等待（见 AGENTS.md：避免引入 GUI 交互测试）。
    file_dialog::interactive = false;

    // 预设打开结果：正常返回 2 个路径。
    file_dialog::headless_open_result = {"C:/test/a.txt", "C:/test/b.png"};
    auto r1 = file_dialog::open_file();
    AURORA_TEST_CHECK(r1.ok());
    AURORA_TEST_CHECK(r1.value().size() == 2);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(r1.value()[0] == "C:/test/a.txt");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(r1.value()[1] == "C:/test/b.png");

    // 带 filters 的 Options 不影响 headless 预设返回。
    file_dialog::Options opts{.title = "Open", .filters = {{.name = "图像", .extensions = {"*.png", "*.jpg"}}}};
    auto r1b = file_dialog::open_file(opts);
    AURORA_TEST_CHECK(r1b.ok() && r1b.value().size() == 2);
    AURORA_TEST_CHECK(opts.filters.size() == 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(opts.filters[0].extensions.size() == 2);

    // 取消：清空预设 → 空列表（无选择）。
    file_dialog::headless_open_result.clear();
    auto r2 = file_dialog::open_file();
    AURORA_TEST_CHECK(r2.ok());
    AURORA_TEST_CHECK(r2.value().empty());

    // 保存文件：预设路径正常返回。
    file_dialog::headless_save_result = "C:/output/out.txt";
    auto r3 = file_dialog::save_file();
    AURORA_TEST_CHECK(r3.ok());
    AURORA_TEST_CHECK(r3.value() == "C:/output/out.txt");

    // 保存文件带 Options：预设优先。
    file_dialog::headless_save_result = "C:/output/out2.txt";
    auto r3b = file_dialog::save_file(file_dialog::Options{.title = "Save"});
    AURORA_TEST_CHECK(r3b.ok() && r3b.value() == "C:/output/out2.txt");

    // 保存取消：清空预设 → 空字符串。
    file_dialog::headless_save_result.clear();
    auto r4 = file_dialog::save_file();
    AURORA_TEST_CHECK(r4.ok());
    AURORA_TEST_CHECK(r4.value().empty());

    // 选择文件夹：headless 始终返回空字符串。
    file_dialog::headless_open_result = {"C:/x"};
    auto r5 = file_dialog::open_folder();
    AURORA_TEST_CHECK(r5.ok());
    AURORA_TEST_CHECK(r5.value().empty());

    // 多次清空后安全：打开/保存均返回空。
    file_dialog::headless_open_result.clear();
    file_dialog::headless_save_result.clear();
    AURORA_TEST_CHECK(file_dialog::open_file().value().empty());
    AURORA_TEST_CHECK(file_dialog::save_file().value().empty());
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_file_dialog ===\n");
    test_file_dialog();
}