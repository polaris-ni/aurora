/// 测试类型: unit
/// 目标单元: include/aurora/widget/command_palette.h
/// 测试说明: 覆盖命令面板的关闭惰性（不占位/不命中）、打开后的焦点作用域与搜索框聚焦、
/// 查询过滤与选中移动、Esc/上下键经打开期临时快捷键接管、Enter 经搜索框提交执行、
/// 空格只落字不执行、无快捷键表与无注册表时的降级、结果上限截断，以及点击结果行执行、点击遮罩关闭

#include <cstddef>
#include <string>
#include <utility>

#include "aurora/app/shortcuts.h"
#include "aurora/commands.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/widget/command_palette.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_command_palette {

namespace {

/// @brief 提升 `on_pointer_event` 可见性：指针事件的命中与坐标本地化由派发器负责，单测直接注入。
class ProbePalette : public CommandPalette {
  public:
    using CommandPalette::CommandPalette;
    using CommandPalette::on_pointer_event;
};

[[nodiscard]] auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

[[nodiscard]] auto full_rect() -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 640.0F, .height = 480.0F}};
}

[[nodiscard]] auto key_event(KeyCode code, ModifierKey mods = ModifierKey::None) -> KeyEvent {
    KeyEvent e;
    e.action = KeyAction::Down;
    e.key = static_cast<int>(code);
    e.modifiers = mods;
    return e;
}

[[nodiscard]] auto typed(const std::string &text) -> TextInputEvent {
    TextInputEvent e;
    e.text = text;
    return e;
}

/// @brief 三条命令：两条可用（一条带默认快捷键 Ctrl+O），一条被启用条件禁用。
void fill_registry(CommandRegistry &reg, int &calls) {
    Command open;
    open.id = "file.open";
    open.title = "Open File";
    open.category = "File";
    open.default_binding = KeyCombo{ModifierKey::Control, KeyCode::O};
    open.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(open));

    Command save;
    save.id = "file.save";
    save.title = "Save File";
    save.category = "File";
    save.action = [&calls]() -> void { calls += 10; };
    reg.add(std::move(save));

    Command closed;
    closed.id = "file.close";
    closed.title = "Close File";
    closed.enabled = []() -> bool { return false; };
    reg.add(std::move(closed));
}

}  // namespace

AURORA_TEST_CASE(closed_palette_does_not_occupy_or_hit) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);

    CommandPalette palette{&reg};
    AURORA_TEST_CHECK_FALSE(palette.is_open());
    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{});
    AURORA_TEST_CHECK_FALSE(palette.execute_selected());  // 无结果：不执行

    constexpr BuildContext ctx;
    AURORA_TEST_CHECK_NULL(palette.hit_test(Point{.x = 100.0F, .y = 100.0F}, full_rect(), ctx));

    const Size laid = palette.layout(bounded(640.0F, 480.0F), ctx);
    AURORA_TEST_CHECK_NEAR(laid.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(laid.height, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(open_traps_focus_and_lists_only_enabled_commands) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);
    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);

    CommandPalette palette{&reg};
    FocusManager fm;
    fm.set_root(&palette);

    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);

    AURORA_TEST_CHECK_TRUE(palette.is_open());
    AURORA_TEST_CHECK_EQ(fm.scope_depth(), std::size_t{1});
    AURORA_TEST_REQUIRE_TRUE(fm.focused() != nullptr);
    AURORA_TEST_CHECK_TRUE(fm.focused() != &palette);  // 焦点落在搜索框，而非面板自身
    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{2});  // 禁用项被过滤
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.open"});

    // ↑ / ↓ 经打开期临时绑定接管（快捷键层先于键盘派发器），越界环绕。
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::ArrowDown)));
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.save"});
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::ArrowDown)));
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.open"});
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::ArrowUp)));
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.save"});

    // Esc 关闭并弹出焦点作用域。
    set_current_focus_manager(&fm);
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::Escape)));
    set_current_focus_manager(nullptr);
    AURORA_TEST_CHECK_FALSE(palette.is_open());
    AURORA_TEST_CHECK_EQ(fm.scope_depth(), std::size_t{0});

    // 关闭即卸载临时绑定：相关按键不再被面板消费；命令自身的绑定不受影响。
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::Escape)));
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::ArrowDown)));
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::O, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(query_filters_and_enter_executes_selected) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);
    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);

    CommandPalette palette{&reg};
    FocusManager fm;
    fm.set_root(&palette);
    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);
    AURORA_TEST_REQUIRE_TRUE(palette.is_open());

    // 文本输入经派发器路由到焦点控件（搜索框）→ 触发过滤。
    TextInputEvent text = typed("save");
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(palette, text, fm));
    AURORA_TEST_CHECK_EQ(palette.query(), std::string{"save"});
    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.save"});

    // Enter 经搜索框的提交回调执行选中项并关闭（不依赖快捷键表）。
    KeyEvent enter = key_event(KeyCode::Enter);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(palette, enter, fm));
    AURORA_TEST_CHECK_EQ(calls, 10);
    AURORA_TEST_CHECK_FALSE(palette.is_open());
    AURORA_TEST_CHECK_EQ(fm.scope_depth(), std::size_t{0});
}

AURORA_TEST_CASE(space_types_a_character_without_executing) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);

    CommandPalette palette{&reg};
    FocusManager fm;
    fm.set_root(&palette);
    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);
    AURORA_TEST_REQUIRE_TRUE(palette.is_open());

    // 空格字符经文本输入落字（走搜索框），不触发执行。
    TextInputEvent space = typed(" ");
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(palette, space, fm));
    AURORA_TEST_CHECK_EQ(palette.query(), std::string{" "});
    AURORA_TEST_CHECK_EQ(calls, 0);
    AURORA_TEST_CHECK_TRUE(palette.is_open());

    // Space 按键本身被激活语义消费，但搜索框的 activate() 是默认 no-op：不执行、不崩溃。
    KeyEvent space_key = key_event(KeyCode::Space);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(palette, space_key, fm));
    AURORA_TEST_CHECK_EQ(calls, 0);
    AURORA_TEST_CHECK_TRUE(palette.is_open());
}

AURORA_TEST_CASE(open_without_registry_degrades_to_empty_panel) {
    ProbePalette palette{nullptr};
    FocusManager fm;
    fm.set_root(&palette);

    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);

    AURORA_TEST_CHECK_TRUE(palette.is_open());
    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{});
    AURORA_TEST_CHECK_FALSE(palette.execute_selected());
    AURORA_TEST_CHECK_TRUE(palette.is_open());  // 无选中项时不关闭

    // 无注册表 → 无从取得快捷键表；面板仍可正常关闭。
    set_current_focus_manager(&fm);
    palette.close();
    set_current_focus_manager(nullptr);
    AURORA_TEST_CHECK_FALSE(palette.is_open());
    AURORA_TEST_CHECK_EQ(fm.scope_depth(), std::size_t{0});
}

AURORA_TEST_CASE(enter_path_works_without_bound_shortcuts) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);
    // 故意不调用 bind_shortcuts：面板取不到快捷键表（↑/↓/Esc 不可用），但搜索框提交路径仍有效。

    CommandPalette palette{&reg};
    FocusManager fm;
    fm.set_root(&palette);
    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);
    AURORA_TEST_REQUIRE_TRUE(palette.is_open());
    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{2});

    KeyEvent enter = key_event(KeyCode::Enter);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(palette, enter, fm));
    AURORA_TEST_CHECK_EQ(calls, 1);  // 首项 "file.open"
    AURORA_TEST_CHECK_FALSE(palette.is_open());
}

AURORA_TEST_CASE(max_results_truncates_the_result_list) {
    CommandRegistry reg;
    for (int i = 0; i < 5; ++i) {
        Command cmd;
        cmd.id = "cmd" + std::to_string(i);
        cmd.title = "Command Item " + std::to_string(i);
        cmd.action = []() -> void {};
        reg.add(std::move(cmd));
    }

    CommandPalette palette{&reg};
    palette.set_max_results(2);
    FocusManager fm;
    fm.set_root(&palette);
    set_current_focus_manager(&fm);
    palette.open();
    set_current_focus_manager(nullptr);

    AURORA_TEST_CHECK_EQ(palette.results().size(), std::size_t{2});
}

AURORA_TEST_CASE(pointer_click_executes_row_and_mask_click_closes) {
    CommandRegistry reg;
    int calls = 0;
    fill_registry(reg, calls);

    ProbePalette palette{&reg};
    FocusManager fm;
    fm.set_root(&palette);
    constexpr BuildContext ctx;
    set_current_focus_manager(&fm);  // 全程保持派发上下文：open/close 的焦点作用域须能配对

    auto open_and_layout = [&]() -> void {
        palette.open();
        (void)palette.layout(bounded(640.0F, 480.0F), ctx);
    };

    open_and_layout();
    AURORA_TEST_REQUIRE_TRUE(palette.is_open());
    // 打开时占满命中（模态吞掉下层点击）。
    AURORA_TEST_CHECK_TRUE(palette.hit_test(Point{.x = 320.0F, .y = 240.0F}, full_rect(), ctx) == &palette);

    // 面板几何不对外暴露：按行探测出「按下后选中第 2 行」的位置，避免在测试里复制布局常量。
    float row_y = -1.0F;
    for (float y = 1.0F; y < 479.0F; y += 1.0F) {
        if (!palette.is_open()) {
            open_and_layout();
        }
        MouseEvent probe;
        probe.action = MouseAction::Press;
        probe.local_position = Point{.x = 320.0F, .y = y};
        palette.on_pointer_event(probe);
        if (palette.is_open() && palette.selected_index() == static_cast<std::size_t>(1)) {
            row_y = y;
            break;
        }
        if (palette.is_open()) {
            palette.close();
        }
    }
    AURORA_TEST_REQUIRE_TRUE(row_y > 0.0F);
    AURORA_TEST_CHECK_EQ(palette.selected_id(), std::string{"file.save"});

    // 在同一行释放 → 执行该行命令。
    MouseEvent release;
    release.action = MouseAction::Release;
    release.local_position = Point{.x = 320.0F, .y = row_y};
    palette.on_pointer_event(release);
    AURORA_TEST_CHECK_EQ(calls, 10);
    AURORA_TEST_CHECK_FALSE(palette.is_open());

    // 点击遮罩（卡片外）→ 关闭且不执行任何命令。
    open_and_layout();
    AURORA_TEST_REQUIRE_TRUE(palette.is_open());
    MouseEvent mask;
    mask.action = MouseAction::Press;
    mask.local_position = Point{.x = 2.0F, .y = 2.0F};
    palette.on_pointer_event(mask);
    AURORA_TEST_CHECK_FALSE(palette.is_open());
    AURORA_TEST_CHECK_EQ(calls, 10);

    AURORA_TEST_CHECK_EQ(fm.scope_depth(), std::size_t{0});
    set_current_focus_manager(nullptr);  // 复原槽位，避免泄漏到后续用例
}

}  // namespace aurora::test_cases::utest_command_palette
