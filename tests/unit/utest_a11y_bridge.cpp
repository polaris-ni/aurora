/// 测试类型: unit
/// 目标单元: include/aurora/core/a11y_provider.h, include/aurora/core/a11y_types.h
/// 测试说明: 平台桥抽象的注册表语义（注册/去重/注销/计数）、事件广播与宿主处理器并存（G12）、
///           播报直投（G4）、惰性激活与 screen_reader_active 回填（D14/R9）、
///           runtime_id 身份分配与状态位默认取值

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/a11y_provider.h"
#include "aurora/core/a11y_types.h"
#include "aurora/core/accessibility.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_a11y_bridge {

namespace {

/// @brief 记录型假桥：只观测「桥侧收到了什么」，不触任何平台 API（可无头运行）。
class RecordingProvider final : public a11y::Provider {
  public:
    auto activate() -> void override {
        active = true;
        ++activate_calls;
        // D14/R9：激活即回填「读屏在线」（heuristic，见设计 §5.1）。
        current_accessibility_settings().screen_reader_active = true;
        a11y::register_provider(*this);
    }
    auto deactivate() -> void override {
        a11y::unregister_provider(*this);
        active = false;
        current_accessibility_settings().screen_reader_active = false;
    }
    auto sync_if_dirty() -> void override {
        ++sync_calls;
        dirty = false;
    }
    auto mark_dirty() -> void override { dirty = true; }
    [[nodiscard]] auto is_active() const -> bool override { return active; }
    [[nodiscard]] auto name() const -> std::string override { return "recording"; }
    auto on_event(const AccessibilityEvent &e) -> void override { events.push_back(e); }
    auto on_announcement(const std::string &t, const Widget *target) -> void override {
        announcements.emplace_back(t, target);
    }
    auto on_widget_destroying(const Widget *w) -> void override { destroying.push_back(w); }

    int activate_calls = 0;
    int sync_calls = 0;
    bool dirty = false;
    bool active = false;
    std::vector<AccessibilityEvent> events;
    std::vector<std::pair<std::string, const Widget *>> announcements;
    /// @brief 收到的「控件正在销毁」通知序列（桥据此切断悬垂语义根）。
    std::vector<const Widget *> destroying;
};

class ProbeLeaf final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "A11yProbeLeaf"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override { return {}; }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

[[nodiscard]] auto save_settings() -> AccessibilitySettings { return current_accessibility_settings(); }

}  // namespace

AURORA_TEST_CASE(registry_registers_and_deduplicates) {
    const std::size_t before = a11y::registered_provider_count();
    RecordingProvider p;
    p.activate();
    AURORA_TEST_CHECK_EQ(a11y::registered_provider_count(), before + 1);

    p.activate();  // 幂等：重复激活不应重复入表
    AURORA_TEST_CHECK_EQ(a11y::registered_provider_count(), before + 1);

    p.deactivate();
    AURORA_TEST_CHECK_EQ(a11y::registered_provider_count(), before);
}

AURORA_TEST_CASE(registry_unregisters_only_when_present) {
    const std::size_t before = a11y::registered_provider_count();
    RecordingProvider p;
    p.deactivate();  // 未激活即去激活：不应下溢、不应抛
    AURORA_TEST_CHECK_EQ(a11y::registered_provider_count(), before);
}

AURORA_TEST_CASE(activation_backfills_screen_reader_active) {
    const AccessibilitySettings saved = save_settings();
    current_accessibility_settings().screen_reader_active = false;

    {
        RecordingProvider p;
        p.activate();
        AURORA_TEST_CHECK_TRUE(current_accessibility_settings().screen_reader_active);
        p.deactivate();
        AURORA_TEST_CHECK_FALSE(current_accessibility_settings().screen_reader_active);
    }

    set_accessibility_settings(saved);
}

AURORA_TEST_CASE(broadcast_marks_dirty_and_forwards_event) {
    RecordingProvider p;
    p.activate();
    p.dirty = false;

    const auto widget = std::make_shared<ProbeLeaf>();
    notify_accessibility_event(
        AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = widget.get()});

    AURORA_TEST_REQUIRE_EQ(p.events.size(), std::size_t{1});
    AURORA_TEST_CHECK(p.events.front().kind == AccessibilityEventKind::ValueChanged);
    AURORA_TEST_CHECK_EQ(p.events.front().target, widget.get());
    AURORA_TEST_CHECK_TRUE(p.dirty);  // 拉取式：事件只置脏，不即时重建

    p.deactivate();
}

AURORA_TEST_CASE(broadcast_coexists_with_host_handler) {
    // G12：桥广播走独立钩子，宿主处理器不被覆盖、也不与桥链式耦合（安装顺序无关）。
    const AccessibilitySettings saved = save_settings();
    int host_calls = 0;
    const auto saved_handler = current_accessibility_event_handler();
    set_accessibility_event_handler([&host_calls](const AccessibilityEvent & /*e*/) -> void { ++host_calls; });

    RecordingProvider p;
    p.activate();  // 宿主处理器先装、桥后装

    notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::FocusChanged});
    AURORA_TEST_CHECK_EQ(host_calls, 1);
    AURORA_TEST_CHECK_EQ(p.events.size(), std::size_t{1});

    p.deactivate();
    notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::FocusChanged});
    AURORA_TEST_CHECK_EQ(host_calls, 2);  // 桥卸载后宿主处理器仍生效（未被链式包裹）

    set_accessibility_event_handler(saved_handler);
    set_accessibility_settings(saved);
}

AURORA_TEST_CASE(announcement_bypasses_diff_and_reaches_provider) {
    RecordingProvider p;
    p.activate();

    const auto widget = std::make_shared<ProbeLeaf>();
    announce_accessibility("保存成功", widget.get());

    AURORA_TEST_REQUIRE_EQ(p.announcements.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(p.announcements.front().first.c_str(), "保存成功");
    AURORA_TEST_CHECK_EQ(p.announcements.front().second, widget.get());
    AURORA_TEST_CHECK_TRUE(p.events.empty());  // 播报不经 on_event

    p.deactivate();
}

AURORA_TEST_CASE(empty_announcement_is_dropped) {
    RecordingProvider p;
    p.activate();
    announce_accessibility("");
    AURORA_TEST_CHECK_TRUE(p.announcements.empty());
    AURORA_TEST_CHECK_TRUE(p.events.empty());
    p.deactivate();
}

AURORA_TEST_CASE(runtime_id_is_unique_and_stable) {
    const auto a = std::make_shared<ProbeLeaf>();
    const auto b = std::make_shared<ProbeLeaf>();
    AURORA_TEST_CHECK_NE(a->runtime_id(), std::uint64_t{0});
    AURORA_TEST_CHECK_NE(a->runtime_id(), b->runtime_id());
    AURORA_TEST_CHECK_EQ(a->runtime_id(), a->runtime_id());  // 同一控件反复读取恒定
}

AURORA_TEST_CASE(state_defaults_are_all_false) {
    const AccessibilityState s;
    AURORA_TEST_CHECK_FALSE(s.focused);
    AURORA_TEST_CHECK_FALSE(s.checkable);
    AURORA_TEST_CHECK_FALSE(s.checked);
    AURORA_TEST_CHECK_FALSE(s.selected);
    AURORA_TEST_CHECK_FALSE(s.read_only);
    AURORA_TEST_CHECK_FALSE(s.disabled);
    AURORA_TEST_CHECK_FALSE(s.visible);
    AURORA_TEST_CHECK_FALSE(s.focusable);
    AURORA_TEST_CHECK_FALSE(s.offscreen);
    AURORA_TEST_CHECK_FALSE(s.expandable);
    AURORA_TEST_CHECK_FALSE(s.expanded);
    AURORA_TEST_CHECK_FALSE(s.multiline);
    AURORA_TEST_CHECK_FALSE(s.password);
}

AURORA_TEST_CASE(range_defaults_are_unit_interval) {
    const AccessibilityRange r;
    AURORA_TEST_CHECK_NEAR(r.min, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.max, 1.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.step, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.value, 0.0, 1e-9);
}

/// @brief 语义根销毁必须被桥感知：桥缓存的根是裸指针，宿主「先拆 UI 树、后拆窗口」是常规顺序，
///        若根没了而桥不知情，窗口存活期间的平台查询会拿悬垂根重建语义树（实机 SIGSEGV）。
///        通道出自 `Node::~Node()` 的单源上报，与结构事件同源但**独立**（结构事件只能给出宿主
///        容器，无法承载「是不是我的根没了」这一判定）。本用例锁死该通知的到达与载荷正确性。
AURORA_TEST_CASE(root_widget_destruction_is_broadcast_to_providers) {
    const AccessibilitySettings saved = save_settings();
    RecordingProvider provider;
    provider.activate();

    const Widget *destroyed = nullptr;
    {
        Node root{std::make_shared<ProbeLeaf>()};
        destroyed = &root.widget();
        provider.destroying.clear();
    }  // 根销毁：通知在此刻发出，且指针此刻仍有效
    AURORA_TEST_CHECK_EQ(provider.destroying.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(provider.destroying.front(), destroyed);

    // 非根控件的销毁同样上报（桥自行比对是否为自己的根，语义树层不做归属判定）。
    provider.destroying.clear();
    {
        Node child{std::make_shared<ProbeLeaf>()};
    }
    AURORA_TEST_CHECK_EQ(provider.destroying.size(), std::size_t{1});

    // 未激活（未注册）的桥收不到任何广播。
    provider.deactivate();
    provider.destroying.clear();
    {
        Node plain{std::make_shared<ProbeLeaf>()};
    }
    AURORA_TEST_CHECK_EQ(provider.destroying.size(), std::size_t{0});

    set_accessibility_settings(saved);
}

}  // namespace aurora::test_cases::utest_a11y_bridge
