// 需求 #12：生成 aurora_api.json（Aurora 公共 API 描述，供工具链 / LSP / 文档使用）。
//
// 通过运行时反射（WidgetRegistry + 各 widget 默认实例的 serializeProps）提取
// 已注册 widget 类型及其属性键，并补充核心枚举（Color 调色板、LengthKind、Alignment、
// KeyCode），输出为 schema 友好的 JSON。
//
// 用法：
//   gen_api_tools                -> 写 JSON 到 stdout（可 `> aurora_api.json` 重定向）
//   gen_api_tools aurora_api.json -> 跨平台直接写文件（CMake 目标 aurora_api_json 使用此模式）
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "api_schema.h"
#include "known_enums.h"

namespace {
// 已知枚举（编译期常量，无法纯运行时反射）。
// 单一来源见 tools/include/known_enums.h —— gen_api / aurora_mcp / aurora_cli / aurora_lsp 共享，
// 取值必须与 include/aurora/** 真实枚举成员逐字一致（tests/test_known_enums.cpp 守护）。
auto known_enums() -> std::map<std::string, std::vector<std::string>> { return aurora::tools::known_enums(); }

} // namespace

auto main(int argc, char **argv) -> int {
    aurora::serialization::register_core_widgets();

    aurora::storage::Json api = aurora::tools::build_api_skeleton();

    // ---- layout_rules：简化版布局协议摘要（供 AI 在约束内生成 JSON） ----
    aurora::storage::Json layout_rules = aurora::storage::Json::object();
    {
        aurora::storage::Json flex = aurora::storage::Json::object();
        flex["description"] = "Flex 布局（Column/Row）：子项沿主轴排列，交叉轴对齐";
        flex["main_axis_alignment"] = known_enums()["MainAxisAlignment"];
        flex["cross_axis_alignment"] = known_enums()["CrossAxisAlignment"];
        flex["main_axis_size"] = known_enums()["MainAxisSize"];
        flex["gap_constraint"] = "gap >= 0";
        layout_rules["flex"] = flex;

        aurora::storage::Json stack = aurora::storage::Json::object();
        stack["description"] = "Stack 布局：子项层叠，后绘制的在上层";
        stack["fit"] = known_enums()["StackFit"];
        layout_rules["stack"] = stack;

        aurora::storage::Json grid = aurora::storage::Json::object();
        grid["description"] = "Grid 布局：固定列数的二维网格";
        grid["required_props"] = aurora::storage::Json::array({ "columns" });
        grid["column_constraint"] = "columns >= 1";
        layout_rules["grid"] = grid;

        aurora::storage::Json length = aurora::storage::Json::object();
        length["description"] = "Length 类型：auto | fill | [px, v] | [percent, v]";
        length["auto"] = "WrapContent，由内容决定尺寸";
        length["fill"] = "Expand，吸收剩余空间";
        length["px_example"] = aurora::storage::Json::array({ "px", 100 });
        length["percent_example"] = aurora::storage::Json::array({ "percent", 50 });
        layout_rules["length"] = length;

        aurora::storage::Json edge_insets = aurora::storage::Json::object();
        edge_insets["description"] = "EdgeInsets 对象：{left, top, right, bottom}，单位 dp";
        layout_rules["edge_insets"] = edge_insets;

        aurora::storage::Json color = aurora::storage::Json::object();
        color["description"] = "Color 类型：[r, g, b, a]，各分量 0-255";
        color["example"] = aurora::storage::Json::array({ 255, 128, 0, 255 });
        layout_rules["color"] = color;
    }
    api["layout_rules"] = layout_rules;

    // ---- state_patterns：三种状态模式的使用场景和 JSON 示例 ----
    aurora::storage::Json state_patterns = aurora::storage::Json::array();
    {
        aurora::storage::Json p1 = aurora::storage::Json::object();
        p1["name"] = "simple_value";
        p1["description"] = "简单值状态：控件持有单一可变值，通过 on_changed 回调通知";
        p1["applicable_widgets"] = aurora::storage::Json::array(
            { "TextInput", "Slider", "Checkbox", "Switch", "Dropdown", "RadioGroup", "SegmentedControl", "TabBar" });
        p1["json_example"] =
            R"({"type": "TextInput", "props": {"value": "hello"}, "events": {"on_changed": "handler_name"}})";
        state_patterns.push_back(p1);

        aurora::storage::Json p2 = aurora::storage::Json::object();
        p2["name"] = "selection_index";
        p2["description"] = "选择索引状态：从选项列表中选择一项，通过 selected_index 和 on_change 管理";
        p2["applicable_widgets"] =
            aurora::storage::Json::array({ "Dropdown", "RadioGroup", "SegmentedControl", "TabBar" });
        p2["json_example"] =
            R"({"type": "Dropdown", "props": {"options": ["A","B","C"], "selected_index": 1}, "events": {"on_change": "handler"}})";
        state_patterns.push_back(p2);

        aurora::storage::Json p3 = aurora::storage::Json::object();
        p3["name"] = "toggle_state";
        p3["description"] = "开关状态：布尔值切换，通过 checked/value 和 on_changed/on_toggled 管理";
        p3["applicable_widgets"] = aurora::storage::Json::array({ "Checkbox", "Switch", "ExpansionPanel" });
        p3["json_example"] =
            R"({"type": "Checkbox", "props": {"checked": false}, "events": {"on_changed": "handler"}})";
        state_patterns.push_back(p3);
    }
    api["state_patterns"] = state_patterns;

    // 保留 gen_error_codes 生成的 error_codes 段，避免整体重写时被覆盖。
    if (argc > 1 && std::string(argv[1]) != "-") {    // NOLINT(*-pro-bounds-pointer-arithmetic)
        std::ifstream ein(argv[1], std::ios::binary); // NOLINT(*-pro-bounds-pointer-arithmetic)
        if (ein) {
            try {
                nlohmann::json existing = nlohmann::json::parse(ein);
                if (existing.contains("error_codes")) {
                    api["error_codes"] = existing["error_codes"];
                }
                // 保留 gen_debug_api 生成的 debug 段（同构 merge-only），避免整体重写被覆盖。
                if (existing.contains("debug")) {
                    api["debug"] = existing["debug"];
                }
                // NOLINTNEXTLINE(*-empty-catch)
            } catch (...) { /* 忽略损坏的既有文件 */
            }
        }
    }

    const std::string text = api.dump(2);
    // 传入文件路径参数时直接写文件（跨平台，供 CMake 目标 aurora_api_json 调用）；
    // 否则写 stdout，保持 `gen_api_tools > aurora_api.json` 的手动重定向用法。
    char *argv1 = argv[1]; // NOLINT(*-pro-bounds-pointer-arithmetic)
    if (argc > 1 && std::string(argv1) != "-") {
        std::ofstream out(argv1, std::ios::binary);
        if (!out) {
            AURORA_LOG_RAW("genapi", "error: cannot open output file: ", argv1, "\n");
            return 1;
        }
        out << text << "\n";
        return 0;
    }
    AURORA_LOG_RAW("genapi", text, "\n");
    return 0;
}
