/// 测试类型: integration
/// 目标组合: serialization + codegen + app/validate 全链路（from_json → validate_ui → to_code 回归）
/// 测试说明: 仓库根 CWD 吃 tests/fixtures/ai_compat/*.json；对照 test_ai_compat 的降级/错误用例
///

// 集成级 ai_compat 用例：与 tests/unit/utest_ai_compat.cpp 的区别在于——
// 这里在「仓库根 CWD」下以真实 fixture 文件驱动完整管线，并对每个合法 fixture 额外跑
// from_json → to_json → to_code 的端到端代码生成（utest 的文件循环只做 from_json+validate）。
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/app/validate_ui.h"
#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/serialization.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::itest_ai_compat {

using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;
using au::serialization::to_json;

namespace fs = std::filesystem;

// ---- 辅助：从多个候选路径中定位 fixture 目录（集成测试以仓库根为 CWD）----
static auto find_fixture_dir() -> fs::path {
    constexpr std::array candidates = {
        "tests/fixtures/ai_compat",
        "../tests/fixtures/ai_compat",
        "../../tests/fixtures/ai_compat",
    };
    for (const auto &c : candidates) {
        if (fs::exists(c) && fs::is_directory(c)) {
            return c;
        }
    }
    return {};
}

// ---- 辅助：加载 JSON fixture 文件 ----
static auto load_fixture(const fs::path &path) -> Json {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Json{};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return Json::parse(ss.str());
    } catch (...) {
        return Json{};
    }
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== itest_ai_compat ===\n");

    // 确保核心 widget 已注册
    register_core_widgets();

    // 定位 fixture 目录（仓库根 CWD）
    auto fixdir = find_fixture_dir();
    AURORA_TEST_REQUIRE_MSG(!fixdir.empty(), "fixture dir located at repo-root CWD");

    int valid_count = 0;
    int error_count = 0;

    // 目录遍历：按文件名前缀分类，无硬编码名单（新增 fixture 即纳入）。
    for (const auto &ent : fs::directory_iterator(fixdir)) {
        if (!ent.is_regular_file()) {
            continue;
        }
        const auto &p = ent.path();
        if (p.extension() != ".json") {
            continue;
        }
        const std::string name = p.filename().string();

        Json j = load_fixture(p);
        AURORA_TEST_CHECK_MSG(!j.is_null(), "fixture " + name + ": loaded");
        if (j.is_null()) {
            continue;
        }

        auto result = from_json(j);
        auto errors = validate_ui_tree(j);

        if (name.starts_with("valid_")) {
            AURORA_TEST_CHECK_MSG(result.ok(), "valid fixture " + name + ": from_json ok");
            AURORA_TEST_CHECK_MSG(errors.empty(), "valid fixture " + name + ": validate ok");
            if (result.ok()) {
                // 全链路：from_json → to_json → to_code（端到端代码生成）。
                Json j2 = to_json(*result.value());
                std::string code = to_code(j2);
                AURORA_TEST_CHECK_MSG(!code.empty(), "valid fixture " + name + ": to_code non-empty");
                // 生成代码应提及根控件类型（to_code 输出 au::<Type>(...) 形式）。
                const std::string root_type = j.value("type", std::string{});
                if (!root_type.empty()) {
                    AURORA_TEST_CHECK_MSG(code.find(root_type) != std::string::npos,
                                          "valid fixture " + name + ": code mentions type " + root_type);
                }
            }
            ++valid_count;
        } else if (name.starts_with("error_")) {
            // 错误 fixture：必须在管线某处被拒绝（from_json 失败 或 validate 报 error）。
            const bool rejected = (!result.ok()) || !errors.empty();
            AURORA_TEST_CHECK_MSG(rejected, "error fixture " + name + ": rejected by pipeline");
            ++error_count;
        }
        // 其它命名（manifest/README）忽略
    }

    AURORA_TEST_CHECK_MSG(valid_count > 0, "at least one valid_*.json fixture traversed");
    AURORA_TEST_CHECK_MSG(error_count > 0, "at least one error_*.json fixture traversed");
}

}  // namespace aurora::test_cases::itest_ai_compat
