// test_dialog.cpp — 对话框控件测试（v0.10.0-beta）。

#include <string>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/dialog.h"

#include "test_harness.h"

using aurora::alert;
using aurora::confirm;
using aurora::Dialog;
using aurora::Json;
using aurora::Node;

// ---------- Dialog 基础 ----------

static void test_dialog_basic() {
    Dialog dlg;
    AURORA_TEST_CHECK(!dlg.is_open());
    AURORA_TEST_CHECK(std::string(dlg.type_name()) == "Dialog");

    dlg.show();
    AURORA_TEST_CHECK(dlg.is_open());

    dlg.close();
    AURORA_TEST_CHECK(!dlg.is_open());
}

static void test_dialog_close_callback() {
    bool closed = false;
    Dialog dlg;
    dlg.set_on_close([&]() -> void { closed = true; });

    dlg.show();
    dlg.close();
    AURORA_TEST_CHECK(closed);
}

static void test_dialog_content() {
    auto content = alert("Title", "Message");
    Dialog dlg(std::move(content));
    dlg.show();

    AURORA_TEST_CHECK(dlg.is_open());
    AURORA_TEST_CHECK(!dlg.child_nodes().empty());
}

// ---------- alert 工厂 ----------

static void test_alert_factory() {
    Node n = alert("Error", "Something went wrong", []() -> void {});
    AURORA_TEST_CHECK(static_cast<bool>(n)); // 非空节点
    AURORA_TEST_CHECK(std::string(n.widget().type_name()) == "Column");
}

// ---------- confirm 工厂 ----------

static void test_confirm_factory() {
    Node n = confirm("Delete?", "Are you sure?", [](bool) -> void {});
    AURORA_TEST_CHECK(static_cast<bool>(n));
    AURORA_TEST_CHECK(std::string(n.widget().type_name()) == "Column");
}

// ---------- Dialog 布局 ----------

static void test_dialog_layout() {
    auto content = alert("Hi", "World");
    Dialog dlg(std::move(content));
    dlg.show();

    Node root(std::move(dlg));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK(snap["type"] == "Dialog");
    // 对话框应占满视口
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 800.0f);
    AURORA_TEST_CHECK(snap["box"]["h"].get<float>() == 600.0f);
}

// ---------- Dialog 关闭时不渲染 ----------

static void test_dialog_closed_invisible() {
    Dialog dlg;
    // 不 show，直接布局
    Node root(std::move(dlg));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    // 关闭时尺寸为 0
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() == 0.0f);
    AURORA_TEST_CHECK(snap["box"]["h"].get<float>() == 0.0f);
}

AURORA_TEST() {
    test_dialog_basic();
    test_dialog_close_callback();
    test_dialog_content();
    test_alert_factory();
    test_confirm_factory();
    test_dialog_layout();
    test_dialog_closed_invisible();
}
