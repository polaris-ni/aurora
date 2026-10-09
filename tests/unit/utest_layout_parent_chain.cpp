/// 测试类型: unit
/// 目标单元: src/aurora/widget/widget.cpp（`Node::~Node`）+ include/aurora/widget/widget.h
///           （`Widget::detach_child_layout_parent` / `detach_all_children_layout_parent`、
///           `Container` 与 `SingleChild` 的析构与换子路径）
/// 测试说明: 布局父链的生命周期契约。父链是脏标记上溯（`mark_needs_layout_impl` →
///           `request_frame` → 根侧 `on_subtree_dirty`）与布局 / Display List 缓存向上失效的
///           共同前提；链条一旦被静默抹断，脏标记无处可去，症状是「改了状态屏幕不动」。
///
/// 覆盖三形态：
///   ① 临时句柄 / 副本 / `std::move` 后的旧对象析构，**不得**清活控件的父链；
///   ② 真正摘除那一次（`Container::remove_child`）才清父指针，且此后对旧控件标脏不崩、不悬垂；
///   ③ 父容器先亡、子控件仍被外部 `shared_ptr` 持有时，子控件的 `layout_parent()` 不得留悬垂值。
///
/// 判据说明：`on_dirty` 是挂在控件自身上的观察回调（`request_frame` 末尾直接调），与父链无关；
/// `on_subtree_dirty` 才是沿 `layout_parent_` 上溯到根才命中的汇聚点。形态①②用
/// 「`on_dirty` 命中而根的 `on_subtree_dirty` 不命中」精确区分「控件自己知道脏了」与「脏到达了
/// 根」—— 只断言前者会漏掉断链，故必须断后者。形态①②的根侧汇聚点由 `DirtyRoot` 提供。

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_layout_parent_chain {

using aurora::Constraints;
using aurora::Size;

namespace {

auto bounded() -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 200.0F}};
}

/// 叶子探针：记录「脏抵达本控件」（作为链顶汇聚点）。
///
/// `request_frame` 先沿 `layout_parent_` 上溯（每层 `on_descendant_dirty`），再对**链顶**调
/// `on_subtree_dirty`，最后无条件调自身的 `on_dirty`。故「`on_dirty` 命中而本控件的
/// `on_subtree_dirty` 不命中」= 控件知道自己脏了、但脏没上溯到它 ⇒ 父链断了。
class DirtyProbe final : public Widget {
  public:
    /// 脏抵达本控件（链顶汇聚点）的次数。
    int reached_probe = 0;
    /// 本控件自身被标脏的次数（与父链无关，任何情形都应命中）。
    int self_dirty = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "DirtyProbe"; }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.max; }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    /// 装两个回调（公开而非 protected：探针自身即观测装置，不涉继承访问控制）。
    void install_counters() {
        on_subtree_dirty = [this](Widget & /*w*/, bool /*layout*/) -> void { ++reached_probe; };
        on_dirty = [this](bool /*layout*/) -> void { ++self_dirty; };
    }
};

/// 根探针：记录脏是否抵达**根**（真正需要抵达的地方）。
class DirtyRoot final : public Widget {
  public:
    int reached_root = 0;

    [[nodiscard]] auto type_name() const -> const char * override { return "DirtyRoot"; }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (child_) {
            child_->set_layout_parent(this);
            child_->layout(c, ctx);
        }
        return c.max;
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    void set_child(Node child) { child_ = std::move(child); }

    void install_counter() {
        on_subtree_dirty = [this](Widget & /*w*/, bool /*layout*/) -> void { ++reached_root; };
    }

  private:
    Node child_;
};

/// 走一次布局以按构造登记布局父链（父链由 `Widget::layout` 的嵌套栈保证完整）。
void drive_layout(Widget &root) { LayoutEngine::layout(root, bounded()); }

}  // namespace

// 形态①：临时句柄 / 副本 / move 后旧对象析构，不得清活控件的父链。
//
// 病灶：`Node::~Node` 曾无条件 `set_layout_parent(nullptr)`，而 `Node` 是可共享句柄——
// 拷贝 / 临时（如 `SingleChild` 的视图缓存、`set_children` 的初始化列表）析构时控件仍在世、
// 仍挂在那一只父下面，父链被静默抹断，脏标记无处上溯。
AURORA_TEST_CASE(a_dropped_temporary_node_keeps_the_layout_parent_chain_intact) {
    auto probe = std::make_shared<DirtyProbe>();
    probe->install_counters();
    auto root = std::make_shared<DirtyRoot>();
    root->set_child(Node{probe});
    root->install_counter();
    drive_layout(*root);

    AURORA_TEST_REQUIRE(probe->layout_parent() == root.get());

    // 三种「句柄没了但控件仍在世」的形态。
    {
        const Node copy = Node{probe};  // 拷贝
    }  // 拷贝析构
    {
        std::vector<Node> scratch;
        scratch.emplace_back(probe);  // 进 vector
        scratch.clear();  // 元素析构
    }  // vector 析构
    {
        Node moved = Node{probe};
        const Node sink = std::move(moved);  // move 后 moved 为空壳，其析构不得动活控件
    }  // moved / sink 析构

    // 父链必须仍在：这是形态①的核心断言。
    AURORA_TEST_CHECK(probe->layout_parent() == root.get());

    // 行为后果：标脏必须仍能抵达**根**的汇聚回调（父链完好 ⇒ 上溯到根，由根的
    // `on_subtree_dirty` 接住）。这是「父链没被临时句柄抹断」的可观测后果。
    //
    // 判据用**相对增量**而非绝对值：`mark_needs_layout` 在布局缓存开启时会沿父链递归
    // 调 `mark_needs_layout_impl`，每层各触发一次 `request_frame`，故根侧命中次数与
    // 树深相关、不是 1。只断「标脏后根被命中了」这个事实，不钉绝对次数（后者会把
    // 判据绑死在递归层数上，改动无关代码也会红）。
    const int root_hits_before = root->reached_root;
    probe->mark_needs_layout();
    AURORA_TEST_CHECK_EQ(probe->self_dirty, 1);
    AURORA_TEST_CHECK(root->reached_root > root_hits_before);
}

// 形态②：真正摘除那一次才清父指针，且此后对旧控件标脏不崩、不悬垂。
AURORA_TEST_CASE(removing_a_child_clears_its_layout_parent_and_the_old_widget_stays_usable) {
    auto probe = std::make_shared<DirtyProbe>();
    probe->install_counters();
    auto col = std::make_shared<Column>(ColumnProps{.children = {Node{probe}}});
    drive_layout(*col);

    AURORA_TEST_REQUIRE(probe->layout_parent() == col.get());
    AURORA_TEST_REQUIRE(col->remove_child(probe.get()));

    // 摘除后父指针必须被清（否则脏会传播到已不在树里的子树）。
    AURORA_TEST_CHECK(probe->layout_parent() == nullptr);

    // 此后对旧控件标脏不得崩溃、不得悬垂读：自身回调照常命中。
    //
    // 注意**不能**断言 `reached_probe == 0`：`request_frame` 里 `top` 初值就是 `this`，
    // 无父时循环不推进，故 `on_subtree_dirty` 仍会命中自己（这是「无父可上溯」的正常
    // 兜底，不是断链）。断链的可判定读数是 `layout_parent()`（已断为 nullptr）。
    probe->mark_needs_layout();
    AURORA_TEST_CHECK_EQ(probe->self_dirty, 1);
}

// 形态③：父容器先亡、子控件仍被外部 shared_ptr 持有时，子的 layout_parent() 不得留悬垂值。
AURORA_TEST_CASE(parent_destruction_clears_the_layout_parent_of_a_still_alive_child) {
    auto probe = std::make_shared<DirtyProbe>();
    probe->install_counters();
    {
        auto col = std::make_shared<Column>(ColumnProps{.children = {Node{probe}}});
        drive_layout(*col);
        AURORA_TEST_REQUIRE(probe->layout_parent() == col.get());
    }  // 父容器析构；probe 被局部 shared_ptr 持有，继续存活

    // 悬垂防护：父已亡，父指针必须已清空（否则后续上溯解引用已析构对象）。
    // 这是本形态的核心断言——留悬垂值意味着后续任何 mark_needs_layout 都会踩已析构内存。
    AURORA_TEST_CHECK(probe->layout_parent() == nullptr);

    // 且对这样的控件标脏不崩、不悬垂读（自身回调照常命中）。
    probe->mark_needs_layout();
    AURORA_TEST_CHECK_EQ(probe->self_dirty, 1);
}

/// 摘除告警的判别子串（与 widget.cpp 的文案一致；三处判定共用，改文案只需改这里）。
constexpr std::string_view AURORA_DETACH_MARK = "layout parent detached";

// 判据支撑：接管 Logger 捕获诊断行，析构时按「先读原值」恢复（不改全局默认行为）。
// 参照 utest_log.cpp 的 CapturedLogger 同款 RAII。
class LogCapture final {
  public:
    LogCapture() : level_{aurora::Logger::instance().level()}, enabled_{aurora::Logger::instance().is_enabled()} {
        auto &logger = aurora::Logger::instance();
        logger.set_level(aurora::LogLevel::Trace);  // 确保 WARN 不被默认阈值(Info)之外拦掉
        logger.set_sink([this](std::string_view line) -> void { lines_.emplace_back(line); });
    }
    ~LogCapture() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_enabled(enabled_);
        logger.set_sink(nullptr);  // 恢复默认 stderr
    }
    LogCapture(const LogCapture &) = delete;
    auto operator=(const LogCapture &) -> LogCapture & = delete;
    LogCapture(LogCapture &&) = delete;
    auto operator=(LogCapture &&) -> LogCapture & = delete;

    [[nodiscard]] auto detach_count() const -> std::size_t {
        return static_cast<std::size_t>(std::ranges::count_if(
            lines_, [](const std::string &line) { return line.find(AURORA_DETACH_MARK) != std::string::npos; }));
    }
    /// 某条 detach 告警正文是否含给定子串（判据③：类型名出现在消息正文里）。
    [[nodiscard]] auto detach_line_contains(std::string_view needle) const -> bool {
        return std::ranges::any_of(lines_, [needle](const std::string &line) {
            return line.find(AURORA_DETACH_MARK) != std::string::npos && line.find(needle) != std::string::npos;
        });
    }
    /// 是否存在含 printf 占位符 `%s` 的 detach 告警（判据③的反面：占位符必须消失）。
    [[nodiscard]] auto detach_line_has_placeholder() const -> bool {
        return std::ranges::any_of(lines_, [](const std::string &line) {
            return line.find(AURORA_DETACH_MARK) != std::string::npos && line.find("%s") != std::string::npos;
        });
    }

  private:
    std::vector<std::string> lines_;
    aurora::LogLevel level_;
    bool enabled_;
};

/// SingleChild 探针：暴露 protected 的 child_nodes_mut() 以便测试构造/驱动，并让
/// `type_name()` 产出可断言的稳定串（判据③要断类型名进正文）。
class ProbeSingleChild final : public SingleChild {
  public:
    explicit ProbeSingleChild(Node child) : SingleChild(std::move(child)) {}
    using SingleChild::child_nodes_mut;
    [[nodiscard]] auto type_name() const -> const char * override { return "ProbeSingleChild"; }
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return child_ ? child_.widget().layout(c, ctx) : c.max;
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, b, ctx);
        }
    }
};

// 判据①上半：容器连同其子控件一起销毁（Column 路径）时，**不得**产生 detach 告警。
AURORA_TEST_CASE(normal_container_teardown_emits_no_detach_warning) {
    LogCapture capture;
    {
        auto col = std::make_shared<Column>();
        col->set_children({Node{DirtyProbe{}}});
        drive_layout(*col);
    }  // 容器与其子控件一起正常销毁（子节点无外部持有）
    AURORA_TEST_CHECK_EQ(capture.detach_count(), 0U);
}

// 判据①上半（SingleChild 路径）：其遍历源 child_view_mut_ 是 child_ 的拷贝副本，
// use_count 基线为 2/3 而非 1——本例钉住「自身持有份数」判别在 SingleChild 上同样静默，
// 防止实现退化成硬编码 `use_count()==1`（那会让本例转红）。
AURORA_TEST_CASE(normal_single_child_teardown_emits_no_detach_warning) {
    LogCapture capture;
    {
        auto sc = std::make_shared<ProbeSingleChild>(Node{DirtyProbe{}});
        drive_layout(*sc);
    }  // SingleChild 与其子控件一起正常销毁
    AURORA_TEST_CHECK_EQ(capture.detach_count(), 0U);
}

// 判据①下半：子件确实活在容器之外被摘走（Column + 外部持有后 remove_child）时，**必须**告警。
AURORA_TEST_CASE(detaching_an_externally_held_child_emits_detach_warning) {
    LogCapture capture;
    auto probe = std::make_shared<DirtyProbe>();
    auto col = std::make_shared<Column>();
    col->set_children({Node{probe}});
    drive_layout(*col);
    AURORA_TEST_REQUIRE(probe->layout_parent() == col.get());
    AURORA_TEST_REQUIRE(col->remove_child(probe.get()));  // 外部仍持有 probe ⇒ 真摘除
    AURORA_TEST_CHECK_EQ(capture.detach_count(), 1U);
}

// 判据①下半（SingleChild 换子路径）：adopt_children 换子时若旧子仍被外部持有，须告警。
AURORA_TEST_CASE(replacing_an_externally_held_child_emits_detach_warning) {
    LogCapture capture;
    auto probe = std::make_shared<DirtyProbe>();
    auto col = std::make_shared<Column>();
    col->set_children({Node{probe}});
    drive_layout(*col);
    col->adopt_children({Node{DirtyProbe{}}});  // 换子：旧 probe 仍被外部持有 ⇒ 异常断链
    AURORA_TEST_CHECK_EQ(capture.detach_count(), 1U);
}

// 判据③：告警正文含被摘控件的类型名，且不再残留 printf 占位符 `%s`。
AURORA_TEST_CASE(detach_warning_message_embeds_the_child_type_name_without_placeholder) {
    LogCapture capture;
    auto probe = std::make_shared<DirtyProbe>();
    auto col = std::make_shared<Column>();
    col->set_children({Node{probe}});
    drive_layout(*col);
    AURORA_TEST_REQUIRE(col->remove_child(probe.get()));

    // 类型名出现在消息正文里（log_concat 折叠拼接的结果）。
    AURORA_TEST_CHECK(capture.detach_line_contains("DirtyProbe"));
    // printf 占位符已消失（AURORA_LOG_* 不是 printf，%s 会原样输出——必须清掉）。
    AURORA_TEST_CHECK(!capture.detach_line_has_placeholder());
}

}  // namespace aurora::test_cases::utest_layout_parent_chain
