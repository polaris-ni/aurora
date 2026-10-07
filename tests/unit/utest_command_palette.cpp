/// 测试类型: unit
/// 目标单元: include/aurora/widget/command_palette.h
/// 测试说明: 覆盖命令面板的关闭惰性（不占位/不命中）、打开后的焦点作用域与搜索框聚焦、
/// 查询过滤与选中移动、Esc/上下键经打开期临时快捷键接管、Enter 经搜索框提交执行、
/// 空格只落字不执行、无快捷键表与无注册表时的降级、结果上限截断，以及点击结果行执行、点击遮罩关闭。
/// 另覆盖两处上屏文案的 i18n：占位符与空态提示按词条 key 查 `default_string_table()`
/// （未登记回退英文字面量、随 `ctx` 注入的 locale 变化）、文本覆盖优先于查表、自定义 key 仍走 i18n、
/// 空态提示确实进入绘制、以及属性序列化往返保留 i18n 语义。

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/shortcuts.h"
#include "aurora/commands.h"
#include "aurora/environment/environment.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/string_table.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"
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

/// @brief 字符串表守卫：`default_string_table()` 是进程级单例且无删除接口，登记完必须整体还原
///        （否则会污染同 TU 里依赖「未登记即回退英文字面量」的用例）。
class ScopedStringTable {
  public:
    ScopedStringTable() : saved_(default_string_table()) {}
    // 还原只是一张字符串表赋值，唯一抛出面是 bad_alloc；守卫析构期抛出会盖掉真正的用例失败，故不捕获。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~ScopedStringTable() { default_string_table() = saved_; }
    ScopedStringTable(const ScopedStringTable &) = delete;
    auto operator=(const ScopedStringTable &) -> ScopedStringTable & = delete;
    ScopedStringTable(ScopedStringTable &&) = delete;
    auto operator=(ScopedStringTable &&) -> ScopedStringTable & = delete;

  private:
    StringTable saved_;
};

#ifdef AURORA_ENABLE_DISPLAY_LIST
/// 录制-回放探针：把 Painter 录下的 DisplayList 回放到本后端，取出每条 DrawText 的文本。
/// 走的是生产侧同一条 `DisplayList::replay` 通路（唯一实现），不新增任何观测专用 API。
class TextCapture final : public rhi::RhiBackend {
  public:
    struct Entry {
        std::string text;  ///< DrawText 落笔的显示串
        Rect bounds;  ///< 该次 draw_text 的包围盒
    };

    [[nodiscard]] auto name() const -> std::string_view override { return "utest_command_palette_text_capture"; }

    auto submit(const DrawCmd &cmd, const rhi::CmdData &data) -> void override {
        if (cmd.kind == CmdKind::DrawText && data.text != nullptr) {
            entries.push_back(Entry{.text = *data.text, .bounds = cmd.bounds});
        }
    }

    // 探针后端：命令序列供用例直接读取，故有意保持公开。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::vector<Entry> entries;  ///< 按提交顺序记录的文本绘制
};
#endif

/// @brief 以给定 locale 构造 `BuildContext`（`ctx.env` 指向栈上 `Environment`，生命周期归调用方）。
/// @param env 已注入值的环境对象（须比返回的 ctx 长寿）。
/// @return 绑定了该环境的构建上下文。
[[nodiscard]] auto ctx_with(const Environment &env) -> BuildContext {
    BuildContext ctx;
    ctx.env = &env;
    return ctx;
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
    for (int yi = 1; yi < 479; ++yi) {
        const auto y = static_cast<float>(yi);  // 用整型计数器避免浮点累加误差与 FloatLoopCounter 告警
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

// ---- 上屏文案 i18n：占位符与空态提示走词条表 ----

AURORA_TEST_CASE(placeholder_defaults_to_lookup_result_or_fallback) {
    CommandPalette palette;
    // 未登记译文：恒非空（判据要求），且等于库内兜底串——即改动前的那条字面量。
    AURORA_TEST_CHECK_FALSE(palette.placeholder().empty());
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{CommandPalette::kDefaultPlaceholderText});
    AURORA_TEST_CHECK_FALSE(palette.empty_message().empty());
    AURORA_TEST_CHECK_EQ(palette.empty_message(), std::string{CommandPalette::kDefaultEmptyMessageText});

    // 两条 key 以**字面量**钉死（不用常量对照）：宿主登记译文要靠这条串，它是外部契约的一部分。
    // 若写成 `kDefaultPlaceholderKey` 自比自，key 拼错时注册端与查表端一起错、两边抵消 → 恒绿。
    AURORA_TEST_CHECK_EQ(palette.placeholder_key(), std::string{"command_palette.placeholder"});
    AURORA_TEST_CHECK_EQ(palette.empty_message_key(), std::string{"command_palette.no_results"});
}

AURORA_TEST_CASE(placeholder_and_empty_message_follow_context_locale) {
    ScopedStringTable guard;
    auto &table = default_string_table();
    // 只登记 fr 档：缺省档（en）仍应回退兜底串，故两条译文必须是与兜底串不同的值。
    // key 写字面量而非常量：库侧 key 被改错一位时，登记端不变、查表端落空 → 本用例转红。
    const std::string ph_fr = "Rechercher une commande";
    const std::string em_fr = "Aucune commande correspondante";
    table.add(Locale{.language = "fr"}, "command_palette.placeholder", ph_fr);
    table.add(Locale{.language = "fr"}, "command_palette.no_results", em_fr);

    CommandPalette palette;  // 无注册表 → 结果为空，走空态分支
    const Environment env = Environment{}.with<Locale>(Locale{.language = "fr"});
    const BuildContext ctx = ctx_with(env);
    palette.open();
    (void)palette.layout(bounded(640.0F, 480.0F), ctx);

    AURORA_TEST_CHECK_EQ(palette.placeholder(), ph_fr);
    AURORA_TEST_CHECK_EQ(palette.empty_message(), em_fr);

    // 换回缺省档（无 Locale 注入）→ 查表落空，回退英文字面量。
    constexpr BuildContext plain;
    palette.mark_needs_layout();
    (void)palette.layout(bounded(640.0F, 480.0F), plain);
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{CommandPalette::kDefaultPlaceholderText});
    AURORA_TEST_CHECK_EQ(palette.empty_message(), std::string{CommandPalette::kDefaultEmptyMessageText});
    palette.close();
}

AURORA_TEST_CASE(custom_text_overrides_lookup) {
    ScopedStringTable guard;
    auto &table = default_string_table();
    // 连缺省档都登记上（key 写字面量）：覆盖值若被查表结果盖掉，本用例立刻转红（不是恒真）。
    table.add(Locale{}, "command_palette.placeholder", "Registered placeholder");
    table.add(Locale{}, "command_palette.no_results", "Registered empty state");

    CommandPalette palette;
    palette.set_placeholder("Find anything");
    palette.set_empty_message("Nothing here");

    constexpr BuildContext ctx;
    palette.open();
    (void)palette.layout(bounded(640.0F, 480.0F), ctx);

    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{"Find anything"});
    AURORA_TEST_CHECK_EQ(palette.empty_message(), std::string{"Nothing here"});
    palette.close();
}

AURORA_TEST_CASE(custom_key_still_localizes) {
    ScopedStringTable guard;
    auto &table = default_string_table();
    const std::string ph_key = "utest_command_palette.custom_placeholder";
    const std::string em_key = "utest_command_palette.custom_empty";
    table.add(Locale{.language = "fr"}, ph_key, "Commande personnalisee");
    table.add(Locale{.language = "fr"}, em_key, "Resultat personnalise");

    CommandPalette palette;
    palette.set_placeholder_key(ph_key);
    palette.set_empty_message_key(em_key);
    AURORA_TEST_CHECK_EQ(palette.placeholder_key(), ph_key);
    AURORA_TEST_CHECK_EQ(palette.empty_message_key(), em_key);

    // 换 key 后**仍走 i18n**：缺省档未登记 → 兜底；fr 档 → 译文。
    constexpr BuildContext plain;
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{CommandPalette::kDefaultPlaceholderText});

    const Environment env = Environment{}.with<Locale>(Locale{.language = "fr"});
    const BuildContext ctx = ctx_with(env);
    palette.open();
    (void)palette.layout(bounded(640.0F, 480.0F), ctx);
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{"Commande personnalisee"});
    AURORA_TEST_CHECK_EQ(palette.empty_message(), std::string{"Resultat personnalise"});

    // 覆写档：文本覆盖优先于查表（此时 fr 档已登记，若覆盖没生效就会被译文盖掉）。
    palette.set_placeholder("Literal override");
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{"Literal override"});

    // 设 key 会清掉文本覆盖，回到查表档——按 fr 上下文重排后应重新读出译文而非兜底串。
    palette.set_placeholder_key(ph_key);
    palette.mark_needs_layout();
    (void)palette.layout(bounded(640.0F, 480.0F), ctx);
    AURORA_TEST_CHECK_EQ(palette.placeholder(), std::string{"Commande personnalisee"});
    palette.close();
}

AURORA_TEST_CASE(empty_state_text_is_drawn_from_lookup) {
    ScopedStringTable guard;
    auto &table = default_string_table();
    const std::string drawn = "Nothing matched at all";
    table.add(Locale{}, "command_palette.no_results", drawn);

    CommandPalette palette;  // 无注册表 → 结果为空，绘制走空态分支
    constexpr BuildContext ctx;
    palette.open();
    (void)palette.layout(bounded(640.0F, 480.0F), ctx);

#ifdef AURORA_ENABLE_DISPLAY_LIST
    // 证明查表结果真的进了绘制，而不是只躺在 getter 里。
    Painter p;
    p.begin(648, 488);
    DisplayList dl;
    p.record(dl);
    palette.paint(p, full_rect(), ctx);
    p.stop();
    TextCapture cap;
    dl.replay(cap);

    bool found = false;
    for (const auto &e : cap.entries) {
        if (e.text == drawn) {
            found = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(found);
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_DISPLAY_LIST is off; draw-record probe unavailable");
#endif
    palette.close();
}

AURORA_TEST_CASE(props_round_trip_preserves_i18n_semantics) {
    CommandPalette src;
    src.set_placeholder_key("host.command_palette.placeholder");
    src.set_empty_message_key("host.command_palette.empty");

    Json props = Json::object();
    src.serialize_props(props);
    // 未设文本覆盖时「未设不写键」：placeholder / empty_message 两键不应出现，
    // 否则读回会被当成撤除 i18n 的指令。
    AURORA_TEST_CHECK_FALSE(props.contains("placeholder"));
    AURORA_TEST_CHECK_FALSE(props.contains("empty_message"));
    AURORA_TEST_CHECK_EQ(props.at("placeholder_key")->as_or<std::string>(""),
                         std::string{"host.command_palette.placeholder"});

    CommandPalette dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.placeholder_key(), std::string{"host.command_palette.placeholder"});
    AURORA_TEST_CHECK_EQ(dst.empty_message_key(), std::string{"host.command_palette.empty"});

    // 覆盖值参与往返，且读回后仍优先于查表。
    CommandPalette with_text;
    with_text.set_placeholder_key("host.command_palette.placeholder");
    with_text.set_placeholder("Literal placeholder");
    Json text_props = Json::object();
    with_text.serialize_props(text_props);
    AURORA_TEST_REQUIRE_TRUE(text_props.contains("placeholder"));

    CommandPalette dst2;
    dst2.deserialize_props(text_props);
    AURORA_TEST_CHECK_EQ(dst2.placeholder(), std::string{"Literal placeholder"});
    AURORA_TEST_CHECK_EQ(dst2.placeholder_key(), std::string{"host.command_palette.placeholder"});
}

}  // namespace aurora::test_cases::utest_command_palette
