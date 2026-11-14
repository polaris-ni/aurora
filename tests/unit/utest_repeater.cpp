/// 测试类型: unit
/// 目标单元: include/aurora/widget/repeater.h
/// 测试说明: 动态列表的四条契约——构造期首轮展开、只在条目数变化时重建、布局按约束夹取、
///           子树深度超限时截断并留降级诊断；另覆盖自描述、序列化与信号采集三个旁路出口
/// 覆盖说明: 不覆盖依赖真实窗口的部分（本控件无）；`State` 提交后的自动标脏依赖挂载接线，
///           此处只钉 rebuild 判据本身，订阅传播由 state 套件覆盖

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/environment/build_context.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/state.h"
#include "aurora/widget/repeater.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_repeater {

namespace {

/// 造一个固定尺寸的替身子件（`Text` 配死尺寸，避免依赖字体度量）。
auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(Length::fixed(w));
    t->height(Length::fixed(h));
    return Node{t};
}

/// 造一组带上下界的约束（上下界相同的场景用 `bounded` 更省事）。
auto limits(float min_w, float min_h, float max_w, float max_h) -> Constraints {
    return Constraints{.min = Size{.width = min_w, .height = min_h}, .max = Size{.width = max_w, .height = max_h}};
}

/// 无下限的宽松约束（布局夹取的上界测法）。
auto bounded(float w, float h) -> Constraints { return limits(0.0F, 0.0F, w, h); }

}  // namespace

AURORA_TEST_CASE(repeater_expands_every_item_on_construction) {
    // 首轮展开发生在构造体内（built_ 初值 false），无需挂载或布局即可得到完整子树。
    State<std::vector<std::string>> items{std::vector<std::string>{"a", "b", "c"}};
    std::vector<int> indices;
    Repeater<std::string> rep{items, [&indices](const std::string &, int i) {
                                  indices.push_back(i);
                                  return box(10.0F, 5.0F);
                              }};

    AURORA_TEST_CHECK_EQ(indices.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(rep.child_nodes().size(), std::size_t{3});
    // 下标按数据源顺序给出 0..n-1（回调契约里它是 int，不是 size_t）。
    for (std::size_t i = 0; i < indices.size(); ++i) {
        AURORA_TEST_CHECK_EQ(indices[i], static_cast<int>(i));
    }
    AURORA_TEST_CHECK(std::string(rep.type_name()) == "Repeater");
}

AURORA_TEST_CASE(repeater_rebuilds_only_when_item_count_changes) {
    // 重建判据是「条目数变化」，不是「数据源提交」——同长度换内容不重建子树。
    // 这条口径写在 rebuild_if_needed 里（避免每帧重建），用例把它钉住：改判据会让
    // 下面第二段断言的 builder 调用次数先变。
    State<std::vector<int>> items{std::vector<int>{1, 2}};
    int calls = 0;
    Repeater<int> rep{items, [&calls](int v, int) {
                          ++calls;
                          return box(10.0F, static_cast<float>(v));
                      }};
    AURORA_TEST_CHECK_EQ(calls, 2);

    BuildContext ctx;
    rep.mount(ctx);

    // 同长度换值：不重建，子件保持首轮结果。
    items.set(std::vector<int>{7, 8});
    rep.layout(bounded(100.0F, 100.0F), ctx);
    AURORA_TEST_CHECK_MSG(calls == 2, "same-size update must not rebuild the subtree");
    AURORA_TEST_CHECK_EQ(rep.child_nodes().size(), std::size_t{2});

    // 条目数变化：下一次布局重建，回调补两次。
    items.set(std::vector<int>{1, 2, 3});
    rep.layout(bounded(100.0F, 100.0F), ctx);
    AURORA_TEST_CHECK_EQ(calls, 5);
    AURORA_TEST_CHECK_EQ(rep.child_nodes().size(), std::size_t{3});

    // 清空数据源：子树随之清空（重建路径的边界）。
    items.set(std::vector<int>{});
    rep.layout(bounded(100.0F, 100.0F), ctx);
    AURORA_TEST_CHECK_EQ(rep.child_nodes().size(), std::size_t{0});
}

AURORA_TEST_CASE(repeater_layout_stacks_children_and_clamps_to_constraints) {
    // 布局口径：宽取子件最大宽、高取子件高之和，两端都按约束夹取。
    State<std::vector<int>> items{std::vector<int>{1, 2, 3}};
    Repeater<int> rep{items, [](int, int) { return box(20.0F, 30.0F); }};
    BuildContext ctx;
    rep.mount(ctx);

    // 上界宽于内容：原样给出内容尺寸。
    const Size roomy = rep.layout(bounded(100.0F, 200.0F), ctx);
    AURORA_TEST_CHECK_NEAR(roomy.width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(roomy.height, 90.0F, 1e-4F);

    // 上界两轴都小于内容：两轴各自被夹到上界（高 90 → 50、宽 20 → 5）。
    const Size tight = rep.layout(bounded(5.0F, 50.0F), ctx);
    AURORA_TEST_CHECK_NEAR(tight.width, 5.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tight.height, 50.0F, 1e-4F);

    // 下界大于内容：两轴各自被抬到下界。
    const Size padded = rep.layout(limits(50.0F, 100.0F, 200.0F, 300.0F), ctx);
    AURORA_TEST_CHECK_NEAR(padded.width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(padded.height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(repeater_truncates_and_reports_when_depth_exceeds_max_depth) {
    // 有界层深度守卫：自身 + 一层子件 = 2，所以 max_depth = 1 必触发。
    // 判据是「子树被清空 + 恰好一条降级诊断」，两者都得成立——只清空不报是静默丢内容。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    State<std::vector<int>> items{std::vector<int>{1}};
    Repeater<int> rep{items, [](int, int) { return box(10.0F, 5.0F); }};
    AURORA_TEST_CHECK_EQ(rep.child_nodes().size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(rep.max_depth(), AURORA_DEFAULT_MAX_WIDGET_DEPTH);

    // 守卫在重建时求值：先调阈值，再改条目数触发一次重建。
    rep.set_max_depth(1);
    AURORA_TEST_CHECK_EQ(rep.max_depth(), std::size_t{1});
    items.set(std::vector<int>{1, 2});
    BuildContext ctx;
    rep.mount(ctx);
    rep.layout(bounded(100.0F, 100.0F), ctx);

    AURORA_TEST_CHECK_MSG(rep.child_nodes().empty(), "over-deep expansion must be truncated");
    const std::vector<Diagnostic> diags = Diagnostics::take();
    AURORA_TEST_CHECK_EQ(diags.size(), std::size_t{1});
    if (!diags.empty()) {
        // 句子必须落在 message（`Diagnostics::degraded` 的形参序是 message/where/code）——
        // 落进 where 的那些调用点让 message 只剩一个子系统词，读日志的人拿不到任何信息。
        AURORA_TEST_CHECK_MSG(diags.front().message.find("exceeds max_depth") != std::string::npos,
                              "the truncation sentence must land in Diagnostic::message");
        AURORA_TEST_CHECK_MSG(diags.front().message.find("Repeater subtree depth") != std::string::npos,
                              "the truncation notice names the subsystem and the measured depth");
        AURORA_TEST_CHECK_MSG(std::string(diags.front().where) == "Repeater",
                              "Diagnostic::where carries the widget type, not the sentence");
    }
}

AURORA_TEST_CASE(repeater_serializes_runtime_note_instead_of_items) {
    // 数据源是运行时态、不可序列化：产物里只留一条说明，且不出现条目内容。
    State<std::vector<std::string>> items{std::vector<std::string>{"alpha"}};
    Repeater<std::string> rep{items, [](const std::string &, int) { return box(10.0F, 5.0F); }};

    const Json snap = serialization::to_json(rep);
    AURORA_TEST_CHECK(std::string(snap.at("type") != nullptr ? snap.at("type")->as_or<std::string>("") : "") ==
                      "Repeater");
    const Json *props = snap.at("props");
    AURORA_TEST_REQUIRE(props != nullptr);
    const Json *note = props->at("note");
    AURORA_TEST_REQUIRE(note != nullptr);
    AURORA_TEST_CHECK_MSG(note->as_or<std::string>("").find("runtime-state driven") != std::string::npos,
                          "the note explains why items are absent");
    AURORA_TEST_CHECK_MSG(note->as_or<std::string>("").find("alpha") == std::string::npos,
                          "item payloads must never leak into the snapshot");
}

AURORA_TEST_CASE(repeater_describes_itself_as_a_multi_child_container) {
    // 自描述是 API 描述与 Inspector 的登记源：名字、子项策略与静态/实例两入口须一致。
    const WidgetDescriptor described = Repeater<int>::describe_static();
    AURORA_TEST_CHECK(std::string(described.name) == "Repeater");
    AURORA_TEST_CHECK_MSG(std::string(described.children_policy) == "multiple",
                          "Repeater expands one child per item, so its policy is multiple");
    AURORA_TEST_CHECK_MSG(!described.examples.empty(), "the descriptor ships a usage example");

    State<std::vector<int>> items{std::vector<int>{1}};
    Repeater<int> rep{items, [](int, int) { return box(10.0F, 5.0F); }};
    AURORA_TEST_CHECK(std::string(rep.describe().name) == described.name);
    AURORA_TEST_CHECK_EQ(rep.describe().properties.size(), described.properties.size());
}

AURORA_TEST_CASE(repeater_collects_its_items_source_exactly_once) {
    // 采集出口只登记数据源本身：同一实例重复采集按来源身份去重，不同实例各记一条。
    State<std::vector<int>> items{std::vector<int>{1}};
    Repeater<int> rep{items, [](int, int) { return box(10.0F, 5.0F); }};

    StateSink sink;
    rep.collect_signals(sink);
    AURORA_TEST_CHECK_EQ(sink.sources().size(), std::size_t{1});
    rep.collect_signals(sink);
    AURORA_TEST_CHECK_MSG(sink.sources().size() == 1, "the same source registers once");

    State<std::vector<int>> other_items{std::vector<int>{2}};
    Repeater<int> other{other_items, [](int, int) { return box(10.0F, 5.0F); }};
    other.collect_signals(sink);
    AURORA_TEST_CHECK_EQ(sink.sources().size(), std::size_t{2});

    // 采集建立的是「来源身份」，不是值快照：写数据源后订阅照常收到一轮。
    int notified = 0;
    const auto subscriptions = sink.watch([&notified]() { ++notified; });
    const int after_first_run = notified;
    items.set(std::vector<int>{1, 2});
    AURORA_TEST_CHECK_MSG(notified > after_first_run, "a commit on the collected source reaches the subscriber");
}

}  // namespace aurora::test_cases::utest_repeater
