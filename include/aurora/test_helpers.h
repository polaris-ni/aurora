#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

// 本头 expect_* 辅助使用 tests/test_harness.h 的 AURORA_TEST_CHECK* 断言宏。
// test_harness.h 自带 #pragma once，且其所在目录 tests/ 已加入测试目标包含路径，故用裸名包含。
#include "test_harness.h"

namespace aurora::test {

/// @brief 无头测试环境：持有根容器、`FocusManager` 与逻辑尺寸。
struct TestEnv {
    std::shared_ptr<Column> root_widget;  ///< 根容器（Column），供工厂 `add` 用
    Node root;  ///< 根节点（同 root_widget）
    FocusManager focus;  ///< 焦点管理（tap/type_text 用）
    int width = 0;  ///< 逻辑宽（dp）
    int height = 0;  ///< 逻辑高（dp）
};

/// @brief 创建无头测试环境（确定性、无需 GUI 后端）。
inline auto init_headless(int width, int height) -> TestEnv {
    TestEnv env;
    env.root_widget = std::make_shared<Column>();
    env.root = Node{env.root_widget};
    env.width = width;
    env.height = height;
    return env;
}

/// @brief 推进一帧：挂载 + 布局（等价于一次确定性 mount/layout，不依赖 GUI 后端）。
inline auto pump(TestEnv &env) -> void {
    env.focus.set_root(env.root_widget.get());
    (void)render_to_logical_snapshot(env.root, env.width, env.height);
}

/// @brief 计算 `target` 在 `root` 坐标系下的绝对包围盒（BFS 累加各层 origin）。
[[nodiscard]] inline auto absolute_bounds(const Node &root, const Widget &target) -> std::optional<Rect> {
    std::optional<Rect> found;
    std::function<void(const Node &, Point)> rec = [&](const Node &n, Point acc) -> void {
        if (found) {
            return;
        }
        const Rect abs{.origin = acc + n.bounds().origin, .size = n.bounds().size};
        if (&n.widget() == &target) {
            found = abs;
            return;
        }
        for (const Node &c : n.widget().child_nodes()) {
            rec(c, acc + n.bounds().origin);
        }
    };
    rec(root, Point{.x = 0.0F, .y = 0.0F});
    return found;
}

/// @brief 在 `target` 中心合成「按下+抬起」，触发其点击逻辑（如 `Button::on_click`）。
inline auto tap(TestEnv &env, const Widget &target) -> void {
    const auto box = absolute_bounds(env.root, target);
    if (!box) {
        return;
    }
    const Point center{.x = box->origin.x + (box->size.width / 2.0F), .y = box->origin.y + (box->size.height / 2.0F)};
    MouseEvent down;
    down.position = center;
    down.action = MouseAction::Press;
    down.button = MouseButton::Left;
    EventDispatcher::dispatch(*env.root_widget, down, &env.focus);
    MouseEvent up;
    up.position = center;
    up.action = MouseAction::Release;
    up.button = MouseButton::Left;
    EventDispatcher::dispatch(*env.root_widget, up, &env.focus);
}

/// @brief 将一段文本逐字符喂给已聚焦的 `target`（典型用于 `TextInput`）。
inline auto type_text(TestEnv &env, Widget &target, std::string_view text) -> void {
    env.focus.set_focus(&target);
    for (const char c : text) {
        TextInputEvent e;
        e.text = std::string(1, c);
        EventDispatcher::dispatch(*env.root_widget, e, env.focus);
    }
}

/// @brief 断言树中存在文本含 `needle` 的 `Text` / `TextInput`。
inline auto expect_text(const Node &root, std::string_view needle) -> void {
    bool found = false;
    std::function<void(const Node &)> rec = [&](const Node &n) -> void {
        if (found) {
            return;
        }
        const Widget &w = n.widget();
        std::string text;
        const std::string tn = w.type_name();
        if (tn == "Text") {
            text = dynamic_cast<const Text &>(w).content.get().text;
        } else if (tn == "TextInput") {
            text = dynamic_cast<const TextInput &>(w).value();
        }
        if (!text.empty() && text.find(std::string(needle)) != std::string::npos) {
            found = true;
        }
        for (const Node &c : w.child_nodes()) {
            rec(c);
        }
    };
    rec(root);
    AURORA_TEST_CHECK_MSG(found, "expected a Text/TextInput containing \"" + std::string(needle) + "\"");
}

/// @brief 断言树中存在指定 `type_name()` 的控件。
inline auto expect_tree_contains(const Node &root, std::string_view type) -> void {
    const std::string want(type);
    bool found = false;
    std::function<void(const Node &)> rec = [&](const Node &n) -> void {
        if (found) {
            return;
        }
        if (std::string(n.widget().type_name()) == want) {
            found = true;
            return;
        }
        for (const Node &c : n.widget().child_nodes()) {
            rec(c);
        }
    };
    rec(root);
    AURORA_TEST_CHECK_MSG(found, "expected tree to contain a \"" + want + "\"");
}

/// @brief 断言树中指定类型控件的数量。
inline auto expect_count(const Node &root, std::string_view type, int expected) -> void {
    const std::string want(type);
    int count = 0;
    std::function<void(const Node &)> rec = [&](const Node &n) -> void {
        if (std::string(n.widget().type_name()) == want) {
            ++count;
        }
        for (const Node &c : n.widget().child_nodes()) {
            rec(c);
        }
    };
    rec(root);
    AURORA_TEST_CHECK_EQ(count, expected);
}

/// @brief 断言某节点绝对包围盒与期望一致（容差 `tol` dp）。
inline auto expect_bounds(const Node &node, const Rect &expected, float tol = 1.0F) -> void {
    const Rect b = node.bounds();
    const bool ok =
        std::abs(b.origin.x - expected.origin.x) <= tol && std::abs(b.origin.y - expected.origin.y) <= tol &&
        std::abs(b.size.width - expected.size.width) <= tol && std::abs(b.size.height - expected.size.height) <= tol;
    AURORA_TEST_CHECK_MSG(ok, "expected node bounds to match (within tolerance)");
}

/// @brief 断言某节点可见性。
inline auto expect_visible(const Node &node, bool expected = true) -> void {
    AURORA_TEST_CHECK_MSG(node.widget().show.get() == expected, "expected node visibility to match");
}

}  // namespace aurora::test
