// test_scene.cpp — 场景（无头渲染 + 结构快照）1:1 测试。
#include <cstdio>
#include <fstream>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::Column;
using aurora::Node;
using aurora::Scene;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_scene ===\n");

    // 1) 单节点场景构造。
    Scene scene{Node{Text{"hi"}}};
    AURORA_TEST_CHECK(true);

    // 2) root() 返回非空的 Widget 引用（类型正确）。
    AURORA_TEST_CHECK(std::string(scene.root().type_name()) == "Text");

    // 3) root_node() 返回根 Node 引用（有效）。
    AURORA_TEST_CHECK(static_cast<bool>(scene.root_node()));

    // 4) 渲染布局后 root() 有确定的非负尺寸。
    auto rp = scene.render_to_png("scene_single.tmp.png", 80, 40);
    AURORA_TEST_CHECK(rp.ok());
    AURORA_TEST_CHECK(scene.root().size().width >= 0.0F && scene.root().size().height >= 0.0F);

    // 5) 单节点 serialize 含 "type" 与 "size" 键。
    std::string s = scene.serialize();
    AURORA_TEST_CHECK(s.find("\"type\"") != std::string::npos);
    AURORA_TEST_CHECK(s.find("\"size\"") != std::string::npos);

    // 6) 单节点 serialize 不含 "children"（叶子无子节点）。
    AURORA_TEST_CHECK(s.find("children") == std::string::npos);

    // 7) 嵌套 Column→Text 场景 serialize 含 "children"。
    Scene nested{Node{Column{Node{Text{"a"}}, Node{Text{"b"}}}}};
    auto nrp = nested.render_to_png("scene_nested.tmp.png", 80, 40);
    AURORA_TEST_CHECK(nrp.ok());
    std::string ns = nested.serialize();
    AURORA_TEST_CHECK(ns.find("children") != std::string::npos);

    // 8) 嵌套 serialize 含两个 Text 子节点类型。
    int text_count = 0;
    size_t pos = ns.find(R"("type":"Text")");
    while (pos != std::string::npos) {
        ++text_count;
        pos = ns.find(R"("type":"Text")", pos + 1);
    }
    AURORA_TEST_CHECK(text_count == 2);

    // 9) 正常路径生成 PNG 文件不崩溃。
    AURORA_TEST_CHECK(std::ifstream("scene_single.tmp.png").good());

    // 清理渲染产物（避免污染工作树）。
    std::remove("scene_single.tmp.png");
    std::remove("scene_nested.tmp.png");
}
