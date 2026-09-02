// 验证 UI Schema 验证器：validate_ui_tree / validate_ui_tree_json。
// 检查：合法树通过、未知类型、缺 type、children 策略违规、类型不匹配、递归子节点错误路径。

#include <cstdio>

#include "aurora/app/validate_ui.h"

#include "test_harness.h"

using aurora::Json;
using aurora::validate_ui_tree;
using aurora::validate_ui_tree_json;

AURORA_TEST() {
    // ---- 1. 合法树验证通过 ----
    {
        Json tree;
        tree["type"] = "Text";
        tree["props"] = Json::object();
        tree["props"]["content"] = "Hello";

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(errors.empty());
    }

    // ---- 2. 未知 widget 类型 ----
    {
        Json tree;
        tree["type"] = "NotAWidget";

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        AURORA_TEST_CHECK(errors[0].path == "$.type");
        AURORA_TEST_CHECK(errors[0].message.find("unknown widget type") != std::string::npos);
        AURORA_TEST_CHECK(!errors[0].suggestion.empty()); // 建议里包含已知类型列表
    }

    // ---- 3. 缺失 type 字段 ----
    {
        Json tree = Json::object();
        tree["props"] = Json::object();

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        AURORA_TEST_CHECK(errors[0].message.find("type") != std::string::npos);
    }

    // ---- 4. 非对象节点 ----
    {
        Json tree = "just a string";
        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        AURORA_TEST_CHECK(errors[0].message.find("object") != std::string::npos);
    }

    // ---- 5. 合法容器树（Column + 子节点）----
    {
        Json child1;
        child1["type"] = "Text";
        child1["props"]["content"] = "A";

        Json child2;
        child2["type"] = "Button";
        child2["props"]["label"] = "Click";

        Json tree;
        tree["type"] = "Column";
        tree["children"] = Json::array({ child1, child2 });

        const auto errors = validate_ui_tree(tree);
        for (const auto &e : errors) {
            AURORA_TEST_PRINTF("  unexpected error: %s: %s\n", e.path.c_str(), e.message.c_str());
        }
        AURORA_TEST_CHECK(errors.empty());
    }

    // ---- 6. 递归子节点错误定位 ----
    {
        Json bad_child;
        bad_child["type"] = "BogusWidget";

        Json tree;
        tree["type"] = "Column";
        tree["children"] = Json::array({ bad_child });

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        AURORA_TEST_CHECK(errors[0].path == "$.children[0].type");
    }

    // ---- 7. children 必须是数组 ----
    {
        Json tree;
        tree["type"] = "Column";
        tree["children"] = "not an array";

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        bool found = false;
        for (const auto &e : errors) {
            if (e.message.find("array") != std::string::npos) {
                found = true;
            }
        }
        AURORA_TEST_CHECK(found);
    }

    // ---- 8. props 必须是对象 ----
    {
        Json tree;
        tree["type"] = "Text";
        tree["props"] = Json::array();

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        bool found = false;
        for (const auto &e : errors) {
            if (e.message.find("object") != std::string::npos) {
                found = true;
            }
        }
        AURORA_TEST_CHECK(found);
    }

    // ---- 9. JSON 报告格式 ----
    {
        Json tree;
        tree["type"] = "Text";
        tree["props"]["content"] = "OK";

        const Json report = validate_ui_tree_json(tree);
        AURORA_TEST_CHECK(report["valid"].get<bool>());
        AURORA_TEST_CHECK(report["errors"].is_array());
        AURORA_TEST_CHECK(report["errors"].empty());

        Json bad;
        bad["type"] = "Nothing";
        const Json report2 = validate_ui_tree_json(bad);
        AURORA_TEST_CHECK(!report2["valid"].get<bool>());
        AURORA_TEST_CHECK(!report2["errors"].empty());
        AURORA_TEST_CHECK(report2["errors"][0].contains("path"));
        AURORA_TEST_CHECK(report2["errors"][0].contains("message"));
    }

    // ---- 10. 嵌套多层树 ----
    {
        Json text;
        text["type"] = "Text";
        text["props"]["content"] = "deep";

        Json row;
        row["type"] = "Row";
        row["children"] = Json::array({ text });

        Json col;
        col["type"] = "Column";
        col["children"] = Json::array({ row });

        const auto errors = validate_ui_tree(col);
        AURORA_TEST_CHECK(errors.empty());

        // 三层深处的错误
        Json bad;
        bad["type"] = "Zzz";
        Json row2;
        row2["type"] = "Row";
        row2["children"] = Json::array({ bad });
        Json col2;
        col2["type"] = "Column";
        col2["children"] = Json::array({ row2 });

        const auto errors2 = validate_ui_tree(col2);
        AURORA_TEST_CHECK(!errors2.empty());
        AURORA_TEST_CHECK(errors2[0].path == "$.children[0].children[0].type");
    }

    // ---- 11. 缺失必填属性（Text 需要 content）----
    {
        Json tree;
        tree["type"] = "Text";
        tree["props"] = Json::object(); // 没有 content

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        AURORA_TEST_CHECK(errors[0].path == "$.props.content");
        AURORA_TEST_CHECK(errors[0].message.find("required") != std::string::npos);
        AURORA_TEST_CHECK(errors[0].suggestion.find("content") != std::string::npos);
    }

    // ---- 12. 属性类型不匹配（content 应为 string）----
    {
        Json tree;
        tree["type"] = "Text";
        tree["props"]["content"] = 12345; // 应为字符串

        const auto errors = validate_ui_tree(tree);
        AURORA_TEST_CHECK(!errors.empty());
        bool found = false;
        for (const auto &e : errors) {
            if (e.message.find("type mismatch") != std::string::npos) {
                found = true;
            }
        }
        AURORA_TEST_CHECK(found);
    }
}
