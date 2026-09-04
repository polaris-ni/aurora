// test_ui_factories.cpp — 覆盖 aurora::ui 声明式工厂层：自动加父、强类型返回、文本透传、Node.id。
// ── API 覆盖映射 ─────────────────────────────
// ui/factories.h(声明式工厂层：自动加父/强类型返回)。

#include <string>

#include "aurora/aurora.h"
#include "aurora/state/reactive.h"
#include "aurora/ui/factories.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "test_harness.h"

using aurora::Button;
using aurora::Checkbox;
using aurora::Column;
using aurora::Grid;
using aurora::Node;
using aurora::Reactive;
using aurora::Row;
using aurora::Scroll;
using aurora::Slider;
using aurora::Stack;
using aurora::Text;
using aurora::TextInput;
using aurora::ui::button;
using aurora::ui::checkbox;
using aurora::ui::grid;
using aurora::ui::hbox;
using aurora::ui::input;
using aurora::ui::label;
using aurora::ui::lazy_list;
using aurora::ui::scroll;
using aurora::ui::slider;
using aurora::ui::stack;
using aurora::ui::vbox;

static void test_label_auto_add_and_text() {
    Column root;
    const size_t before = root.child_nodes().size();
    Text const *t = label(root, "Hello");
    AURORA_TEST_CHECK(t != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(t->type_name()) == "Text", "label returns Text*");
    AURORA_TEST_CHECK_MSG(root.child_nodes().size() == before + 1, "label auto-added to parent");
    AURORA_TEST_CHECK_MSG(t->content.get().text == "Hello", "label text applied");
    AURORA_TEST_CHECK_MSG(t->content.get().localize == false, "label is a literal (non-localized)");
}

static void test_button_factory() {
    Column root;
    Button const *b = button(root, "Go", {}, []() -> void {});
    AURORA_TEST_CHECK(b != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(b->type_name()) == "Button", "button returns Button*");
    AURORA_TEST_CHECK_MSG(b->label.get().text == "Go", "button label applied");
}

static void test_input_factory() {
    Column root;
    TextInput const *in = input(root, "abc");
    AURORA_TEST_CHECK(in != nullptr);
    AURORA_TEST_CHECK_MSG(in->value() == "abc", "input initial value applied");
}

static void test_checkbox_slider_factories() {
    Column root;
    Checkbox *const c = checkbox(root, Reactive{false});
    AURORA_TEST_CHECK(c != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(c->type_name()) == "Checkbox", "checkbox returns Checkbox*");
    // bool 初值重载
    Checkbox const *c2 = checkbox(root, true);
    AURORA_TEST_CHECK(c2 != nullptr);

    Slider *const s = slider(root, Reactive{0.5});
    AURORA_TEST_CHECK(s != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(s->type_name()) == "Slider", "slider returns Slider*");
    Slider const *s2 = slider(root, 0.25);
    AURORA_TEST_CHECK(s2 != nullptr);
}

static void test_lazy_list_factory() {
    Column root;
    auto const *ll = lazy_list(root, 1000, [](int i) -> Node { return Node{Text(std::to_string(i))}; }, 24.0F);
    AURORA_TEST_CHECK(ll != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(ll->type_name()) == "LazyList", "lazy_list returns LazyList*");
}

static void test_container_factories() {
    Column root;
    Column const *v = vbox(root);
    AURORA_TEST_CHECK(v != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(v->type_name()) == "Column", "vbox returns Column*");

    Row const *h = hbox(root);
    AURORA_TEST_CHECK(h != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(h->type_name()) == "Row", "hbox returns Row*");

    Stack const *st = stack(root);
    AURORA_TEST_CHECK(st != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(st->type_name()) == "Stack", "stack returns Stack*");

    Grid const *g = grid(root);
    AURORA_TEST_CHECK(g != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(g->type_name()) == "Grid", "grid returns Grid*");

    Scroll const *sc = scroll(root);
    AURORA_TEST_CHECK(sc != nullptr);
    AURORA_TEST_CHECK_MSG(std::string(sc->type_name()) == "Scroll", "scroll returns Scroll*");
}

static void test_node_id() {
    Node n{Text{"X"}};
    AURORA_TEST_CHECK_MSG(n.id().empty(), "default id empty");
    n.set_id("lbl");
    AURORA_TEST_CHECK_MSG(n.id() == "lbl", "set_id / id round-trip");
    n.set_id("");
    AURORA_TEST_CHECK_MSG(n.id().empty(), "clearing id works");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== ui_factories_test ===\n");
    test_label_auto_add_and_text();
    test_button_factory();
    test_input_factory();
    test_checkbox_slider_factories();
    test_lazy_list_factory();
    test_container_factories();
    test_node_id();
}
