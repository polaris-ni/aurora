/// 测试类型: unit
/// 目标单元: src/aurora/event/detail/system_drag_ingress.h
/// 测试说明: 系统拖入入口的三条契约——① 槽写定的 `allowed` 原样回传（宿主据此回写 OS 光标）；
/// ② 无槽 / inert 接管期间返回 `None` 且**不派发**（不谎报已派发）；③ inert 三件套安装 → 抑制
/// 真实派发、卸载 → 恢复。另含一条串联用例：入口 → 真 `EventDispatcher` → 控件 accept 查询与
/// `allowed` 求交，证明「入口不是绕过派发链的第二条路」。
///
/// 变异自证：把「inert 期间抑制」改成不抑制 → `..._suppresses_the_real_sink` 与
/// `..._blocks_the_real_dispatcher_chain` 转红；把无槽时的返回从 `None` 改成 `Copy`
/// → `..._reports_none_without_a_sink` 转红。

#include <optional>
#include <string>
#include <vector>

#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_system_drag_ingress {

using aurora::system_drag::ingress::dispatch;
using aurora::system_drag::ingress::DragInSink;
using aurora::system_drag::ingress::install_test_drag_in;
using aurora::system_drag::ingress::remove_test_drag_in;
using aurora::system_drag::ingress::reset_test_drag_in;

namespace {

/// @brief 固定尺寸拖入目标：记录落放与 accept 查询，`accept` 可配。
class DropBox final : public LeafWidget {
  public:
    DragAction accept = DragAction::Copy;  ///< `query_system_drag_accept` 的返回值
    int drops = 0;  ///< 收到的 `on_system_drop` 次数
    int accept_queries = 0;  ///< accept 查询次数（Enter / Over 各态一次）
    SystemDropPhase last_phase = SystemDropPhase::Drop;  ///< 最近一次事件阶段

    auto type_name() const -> const char * override { return "DropBox"; }

    auto on_layout(const Constraints &c, [[maybe_unused]] const BuildContext &ctx) -> Size override {
        size_ = c.constrain(Size{.width = 100.0F, .height = 100.0F});
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter &p, [[maybe_unused]] const Rect &bounds,
                  [[maybe_unused]] const BuildContext &ctx) -> void override {}

    auto on_system_drop(SystemDropEvent &e) -> void override {
        ++drops;
        last_phase = e.phase;
        e.is_handled = true;
    }

    [[nodiscard]] auto query_system_drag_accept([[maybe_unused]] const std::vector<std::string> &formats)
        -> DragAction override {
        ++accept_queries;
        return accept;
    }
};

/// @brief 构造并完成一次布局的拖入目标（命中测试需要有效尺寸）。
[[nodiscard]] auto make_target() -> std::shared_ptr<DropBox> {
    auto box = std::make_shared<DropBox>();
    box->layout(Constraints{}, BuildContext{});
    return box;
}

/// @brief 一条 Enter 态事件（坐标落在目标内、proposed 为 Move、格式为 uri-list）。
[[nodiscard]] auto enter_event() -> SystemDropEvent {
    SystemDropEvent event;
    event.phase = SystemDropPhase::Enter;
    event.position = Point{.x = 10.0F, .y = 10.0F};
    event.formats.emplace_back("text/uri-list");
    event.proposed = DragAction::Move;
    return event;
}

}  // namespace

AURORA_TEST_CASE(ingress_dispatch_returns_allowed_written_by_the_sink) {
    SystemDropEvent event = enter_event();
    const DragAction allowed = dispatch(event, [](SystemDropEvent &e) -> DragAction {
        e.allowed = DragAction::Copy;
        return e.allowed;
    });
    AURORA_TEST_CHECK_TRUE(allowed == DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(event.allowed == DragAction::Copy);
}

AURORA_TEST_CASE(ingress_dispatch_reports_none_without_a_sink) {
    // 无派发链可投时不谎报：返回 None 且事件不被改写。
    SystemDropEvent event = enter_event();
    AURORA_TEST_CHECK_TRUE(dispatch(event, DragInSink{}) == DragAction::None);
    AURORA_TEST_CHECK_TRUE(event.allowed == DragAction::None);
}

AURORA_TEST_CASE(ingress_inert_backend_suppresses_the_real_sink) {
    if (!install_test_drag_in()) {
        AURORA_TEST_SKIP(
            "drag-in inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }
    bool sink_called = false;
    SystemDropEvent event = enter_event();
    const DragAction allowed = dispatch(event, [&sink_called](SystemDropEvent &e) -> DragAction {
        sink_called = true;
        e.allowed = DragAction::Copy;
        return e.allowed;
    });
    // 接管期间真实后端不插手：槽不被调用、allowed 保持 None（不是「派发了但假装没派发」）。
    AURORA_TEST_CHECK_FALSE(sink_called);
    AURORA_TEST_CHECK_TRUE(allowed == DragAction::None);
    AURORA_TEST_CHECK_TRUE(event.allowed == DragAction::None);
    AURORA_TEST_CHECK_TRUE(remove_test_drag_in());
}

AURORA_TEST_CASE(ingress_inert_backend_removal_restores_the_real_sink) {
    if (!install_test_drag_in()) {
        AURORA_TEST_SKIP(
            "drag-in inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }
    reset_test_drag_in();  // 复位即恢复真实派发（不卸载后端对象）
    SystemDropEvent event = enter_event();
    const DragAction allowed = dispatch(event, [](SystemDropEvent &e) -> DragAction {
        e.allowed = DragAction::Move;
        return e.allowed;
    });
    AURORA_TEST_CHECK_TRUE(allowed == DragAction::Move);
    // 复位已把接管态清掉，故此时卸载返回 false（「没有后端被卸载」），与三件套的语义一致。
    AURORA_TEST_CHECK_FALSE(remove_test_drag_in());
    AURORA_TEST_CHECK_TRUE(install_test_drag_in());
    AURORA_TEST_CHECK_TRUE(remove_test_drag_in());  // 真装过一次 ⇒ 卸载返回 true
    // 卸载后仍走真实派发（无后端 ⇒ 不抑制）。
    SystemDropEvent after = enter_event();
    AURORA_TEST_CHECK_TRUE(dispatch(after, [](SystemDropEvent &e) -> DragAction {
                               e.allowed = DragAction::Link;
                               return e.allowed;
                           }) == DragAction::Link);
}

AURORA_TEST_CASE(ingress_feeds_the_real_dispatcher_chain) {
    const auto box = make_target();
    box->accept = DragAction::Move;
    SystemDropEvent event = enter_event();
    // 串联真派发器：accepted(Move) 与 proposed(Move) 求交 ⇒ allowed = Move。
    const DragAction allowed = dispatch(event, [&box](SystemDropEvent &e) -> DragAction {
        EventDispatcher::dispatch(*box, e);
        return e.allowed;
    });
    AURORA_TEST_CHECK_TRUE(allowed == DragAction::Move);
    AURORA_TEST_CHECK_EQ(box->accept_queries, 1);  // Enter 态查询一次
    AURORA_TEST_CHECK_TRUE(event.is_handled);

    // 控件声明只接受 Copy 而源建议 Move ⇒ 求交后 None（光标显示「禁止放置」）。
    const auto strict = make_target();
    strict->accept = DragAction::Copy;
    SystemDropEvent second = enter_event();
    AURORA_TEST_CHECK_TRUE(dispatch(second, [&strict](SystemDropEvent &e) -> DragAction {
                               EventDispatcher::dispatch(*strict, e);
                               return e.allowed;
                           }) == DragAction::None);
}

AURORA_TEST_CASE(ingress_inert_takeover_blocks_the_real_dispatcher_chain) {
    if (!install_test_drag_in()) {
        AURORA_TEST_SKIP(
            "drag-in inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }
    const auto box = make_target();
    SystemDropEvent event = enter_event();
    const DragAction allowed = dispatch(event, [&box](SystemDropEvent &e) -> DragAction {
        EventDispatcher::dispatch(*box, e);
        return e.allowed;
    });
    // 接管期间真机后端不会把事件塞进派发链：控件零查询、零落放。
    AURORA_TEST_CHECK_TRUE(allowed == DragAction::None);
    AURORA_TEST_CHECK_EQ(box->accept_queries, 0);
    AURORA_TEST_CHECK_EQ(box->drops, 0);
    AURORA_TEST_CHECK_TRUE(remove_test_drag_in());
}

}  // namespace aurora::test_cases::utest_system_drag_ingress
