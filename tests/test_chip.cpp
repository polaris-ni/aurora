#include "aurora/widget/chip.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

#include "test_harness.h"

using aurora::Badge;
using aurora::Chip;
using aurora::Color;
using aurora::Json;
using aurora::Node;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_chip ===\n");

    // --- Chip 构造 / 空状态 ---
    {
        Chip chip;
        AURORA_TEST_CHECK(chip.type_name() == std::string("Chip"));
        AURORA_TEST_CHECK(chip.label().empty());
    }

    // --- Chip 带 label ---
    {
        Chip chip;
        chip.set_label("Tag");
        AURORA_TEST_CHECK(chip.label() == "Tag");
    }

    // --- Chip describe_static ---
    {
        auto desc = Chip::describe_static();
        AURORA_TEST_CHECK(desc.name == "Chip");
        AURORA_TEST_CHECK(!desc.properties.empty());
        bool has_label = false;
        for (const auto &p : desc.properties) {
            if (p.name == "label") {
                has_label = true;
            }
        }
        AURORA_TEST_CHECK(has_label);
    }

    // --- Chip 序列化往返 ---
    {
        Chip chip;
        chip.set_label("Hello");
        Json props = Json::object();
        chip.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("label"));
        AURORA_TEST_CHECK(props["label"].get<std::string>() == "Hello");

        Chip chip2;
        chip2.deserialize_props(props);
        AURORA_TEST_CHECK(chip2.label() == "Hello");
    }

    // --- Chip on_delete 回调 ---
    {
        Chip chip;
        chip.set_label("Removable");
        bool deleted = false;
        chip.set_on_delete([&]() -> void { deleted = true; });
        // 验证 set_on_delete 返回引用
        chip.set_on_delete([&]() -> void { deleted = true; });
        AURORA_TEST_CHECK(!deleted); // 未触发（需要点击）
    }

    // --- Badge 构造 ---
    {
        Badge badge;
        AURORA_TEST_CHECK(badge.type_name() == std::string("Badge"));
        AURORA_TEST_CHECK(badge.count() == 0);
    }

    // --- Badge count ---
    {
        Badge badge;
        badge.set_count(42);
        AURORA_TEST_CHECK(badge.count() == 42);
    }

    // --- Badge with child ---
    {
        Badge badge(5, Node(std::make_shared<Text>("Hi")));
        AURORA_TEST_CHECK(badge.count() == 5);
    }

    // --- Badge describe_static ---
    {
        auto desc = Badge::describe_static();
        AURORA_TEST_CHECK(desc.name == "Badge");
        bool has_count = false;
        for (const auto &p : desc.properties) {
            if (p.name == "count") {
                has_count = true;
            }
        }
        AURORA_TEST_CHECK(has_count);
    }

    // --- Badge 序列化往返 ---
    {
        Badge badge;
        badge.set_count(7);
        Json props = Json::object();
        badge.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("count"));
        AURORA_TEST_CHECK(props["count"].get<int>() == 7);

        Badge badge2;
        badge2.deserialize_props(props);
        AURORA_TEST_CHECK(badge2.count() == 7);
    }

    // --- Chip 现代化属性：胶囊圆角/文本色/字号往返 ---
    {
        Chip chip;
        chip.set_label("Style")
            .set_background(Color{ 10, 20, 30, 255 })
            .set_text_color(Color{ 1, 2, 3, 255 })
            .set_delete_color(Color{ 4, 5, 6, 255 })
            .set_font_size(15.0f)
            .set_corner_radius(9.0f);
        Json props = Json::object();
        chip.serialize_props(props);
        AURORA_TEST_CHECK(props["background"][0].get<int>() == 10);
        AURORA_TEST_CHECK(props["text_color"][2].get<int>() == 3);
        AURORA_TEST_CHECK(props["delete_color"][0].get<int>() == 4);
        AURORA_TEST_CHECK(props["font_size"].get<float>() == 15.0f);
        AURORA_TEST_CHECK(props["corner_radius"].get<float>() == 9.0f);

        Chip chip2;
        chip2.deserialize_props(props);
        AURORA_TEST_CHECK(chip2.background() == Color(10, 20, 30, 255));
        Json k = Json::object();
        chip2.serialize_props(k);
        AURORA_TEST_CHECK(k["corner_radius"].get<float>() == 9.0f);
    }

    // --- Badge 徽章色/文字色往返 ---
    {
        Badge badge;
        badge.set_count(3).set_badge_color(Color{ 11, 22, 33, 255 }).set_text_color(Color{ 44, 55, 66, 255 });
        Json props = Json::object();
        badge.serialize_props(props);
        AURORA_TEST_CHECK(props["badge_color"][0].get<int>() == 11);
        AURORA_TEST_CHECK(props["text_color"][1].get<int>() == 55);

        Badge badge2;
        badge2.deserialize_props(props);
        Json k = Json::object();
        badge2.serialize_props(k);
        AURORA_TEST_CHECK(k["badge_color"][2].get<int>() == 33);
    }
}
