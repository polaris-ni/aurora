// generate_ui 验证：NL 关键词 → Widget JSON 树，且生成结果可经 from_json 往返。
// 不依赖 GUI 后端；使用 tests/test_harness.h 的 AURORA_TEST_CHECK（NDEBUG 安全）。
#include <string>

#include "aurora/app/generate_ui.h"
#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::generate_ui;
using aurora::Json;
using aurora::validate_generate_ui;

AURORA_TEST() {
    // 空描述 → Error
    {
        auto r = generate_ui("");
        AURORA_TEST_CHECK(!r.ok());
    }

    // 关键词映射：button → Button（带默认 text）
    {
        auto r = generate_ui("please add a button");
        AURORA_TEST_CHECK(r.ok());
        const Json &tree = r.value();
        AURORA_TEST_CHECK(tree.contains("node"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tree["node"]["type"] == "Stack");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Json &children = tree["node"]["children"];
        AURORA_TEST_CHECK(children.is_array());
        bool has_button = false;
        for (const auto &c : children) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (c["type"] == "Button") {
                has_button = true;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                AURORA_TEST_CHECK(c["props"]["text"] == "Button");
            }
        }
        AURORA_TEST_CHECK(has_button);
    }

    // text / label → Text
    {
        auto r = generate_ui("show a text label");
        AURORA_TEST_CHECK(r.ok());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Json &children = r.value()["node"]["children"];
        bool has_text = false;
        for (const auto &c : children) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (c["type"] == "Text") {
                has_text = true;
            }
        }
        AURORA_TEST_CHECK(has_text);
    }

    // column / row → Column + Row 各一个
    {
        auto r = generate_ui("a column containing a row");
        AURORA_TEST_CHECK(r.ok());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Json &children = r.value()["node"]["children"];
        int cols = 0;
        int rows = 0;
        for (const auto &c : children) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (c["type"] == "Column") {
                ++cols;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (c["type"] == "Row") {
                ++rows;
            }
        }
        AURORA_TEST_CHECK(cols == 1 && rows == 1);
    }

    // checkbox / switch / slider → 各自对应类型
    {
        auto r = generate_ui("checkbox switch slider");
        AURORA_TEST_CHECK(r.ok());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Json &children = r.value()["node"]["children"];
        int matched = 0;
        for (const auto &c : children) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::string t = c["type"];
            if (t == "Checkbox" || t == "Switch" || t == "Slider") {
                ++matched;
            }
        }
        AURORA_TEST_CHECK(matched == 3);
    }

    // 无匹配关键词 → 兜底 Text（前缀 "?"）
    {
        auto r = generate_ui("do something weird");
        AURORA_TEST_CHECK(r.ok());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Json &children = r.value()["node"]["children"];
        AURORA_TEST_CHECK(children.size() == 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(children[0]["type"] == "Text");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(children[0]["props"]["text"].get<std::string>().substr(0, 1) == "?");
    }

    // 往返校验：生成 → from_json 成功（schema 匹配）
    {
        AURORA_TEST_CHECK(validate_generate_ui("button"));
        AURORA_TEST_CHECK(validate_generate_ui("column row checkbox slider switch"));
        AURORA_TEST_CHECK(!validate_generate_ui(""));  // 空描述无法生成
    }
}