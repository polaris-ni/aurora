/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h
/// 测试说明: 集成——locale 的帧级根注入与换档落地：`Window::set_locale` 只存值、自下一帧起进入
///           根 BuildContext，未设定时不注入（子树读空指针，控件据此回落缺省档，口径不变）；
///           子树 LocaleProvider 按「最近祖先优先」覆盖窗口级默认；换档那一帧整子树缓存失效，
///           故按环境解析的文本（布局期 resolve）在下一帧即按新档重解析——Provider 换值与窗口
///           级换档两条路径同口径，且不依赖后代自身标脏

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/message_catalog.h"
#include "aurora/i18n/message_id.h"
#include "aurora/state/state.h"
#include "aurora/widget/node.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_locale_injection {

namespace {

// 静态初始化期构造短字符串，唯一抛出路径是 bad_alloc；测试进程对内存耗尽没有恢复语义
// （既有同类件按同口径豁免，见 src/aurora/render/font_discovery.cpp）。
// NOLINTBEGIN(bugprone-throwing-static-initialization)
const Locale AURORA_WINDOW_LOCALE{.language = "zh", .script = "Hans", .region = "CN"};
const Locale AURORA_SUBTREE_LOCALE{.language = "en"};
// NOLINTEND(bugprone-throwing-static-initialization)

/// Headless 窗口：固定尺寸 + 关闭脏追踪（否则无脏的下一帧被整帧跳过，根注入无从复评）。
auto make_window(const std::string &name) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    opts.size = Size{.width = 400.0F, .height = 200.0F};
    opts.title = name;
    opts.png_path = (std::filesystem::path(testing::isolation::temp_dir()) / (name + ".png")).string();
    auto created = create_window(opts);
    AURORA_TEST_REQUIRE_MSG(created.ok(), "create_window(HeadlessOptions) failed");
    std::unique_ptr<Window> win = std::move(created.value());
    win->enable_dirty_tracking(false);
    return win;
}

/// 逐帧记录本控件读到的 locale 注入值（未注入记 nullopt）的叶子探针。
///
/// 用自建控件而非 `LayoutBuilder`：后者只在约束变化 / 闭包替换时重跑 builder，同一棵树连投
/// 两帧观察不到「第二帧才生效」。探针不禁布局缓存——「换档那帧整子树重排」正是本文件要钉的
/// 行为之一（见 `Window::set_locale` 的子树缓存失效），缓存若短路则第二帧读不到新注入。
class LocaleObserver final : public Widget {
  public:
    std::vector<std::optional<Locale>> seen;  ///< 每帧一条：布局阶段读到的环境 locale。

    [[nodiscard]] auto type_name() const -> const char * override { return "LocaleObserver"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const auto *loc = ctx.environment<Locale>();
        seen.push_back(loc != nullptr ? std::optional<Locale>{*loc} : std::nullopt);
        return c.max;  // 撑满父约束（无子节点，尺寸不参与断言）
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

/// 按树形取本控件的直接子控件（const 遍历，取首个；无子则 nullptr）。
[[nodiscard]] auto first_child(const Widget &w) -> const Widget * {
    const Widget *found = nullptr;
    w.for_each_child([&found](const Widget &child) -> void {
        if (found == nullptr) {
            found = &child;
        }
    });
    return found;
}

}  // namespace

AURORA_TEST_CASE(window_locale_reaches_the_tree_from_the_next_frame) {
    auto win = make_window("locale_inject");
    auto probe = std::make_shared<LocaleObserver>();
    Node node{probe};

    // 未设定：不注入，子树读到空指针（控件据此回落缺省档）。
    (void)win->present_root(node);
    AURORA_TEST_REQUIRE_EQ(probe->seen.size(), 1U);
    AURORA_TEST_CHECK_FALSE(probe->seen[0].has_value());

    // setter 只存值：窗口侧已读到新档，注入却要等下一次 prepare_context（即下一帧）才进树。
    win->set_locale(AURORA_WINDOW_LOCALE);
    AURORA_TEST_CHECK_EQ(testing::require_value(win->locale()).tag(), std::string("zh-Hans-CN"));

    // 换档自带动作：`set_locale` 在注入那帧清整子树缓存，故探针无需手工标脏即可重读环境。
    (void)win->present_root(node);
    AURORA_TEST_REQUIRE_EQ(probe->seen.size(), 2U);
    AURORA_TEST_REQUIRE_MSG(probe->seen[1].has_value(), "window locale is root-injected from the next frame");
    const Locale window_locale = testing::require_value(probe->seen[1]);
    AURORA_TEST_CHECK_EQ(window_locale.language, std::string("zh"));
    AURORA_TEST_CHECK_EQ(window_locale.script, std::string("Hans"));
    AURORA_TEST_CHECK_EQ(window_locale.region, std::string("CN"));
}

AURORA_TEST_CASE(locale_provider_subtree_still_overrides_window_locale) {
    auto win = make_window("locale_override");
    win->set_locale(AURORA_WINDOW_LOCALE);

    auto probe = std::make_shared<LocaleObserver>();
    Node node{LocaleProvider{AURORA_SUBTREE_LOCALE, Node{probe}}};
    (void)win->present_root(node);

    AURORA_TEST_REQUIRE_EQ(probe->seen.size(), 1U);
    AURORA_TEST_REQUIRE_MSG(probe->seen[0].has_value(), "subtree reads an injected locale");
    const Locale subtree_locale = testing::require_value(probe->seen[0]);
    AURORA_TEST_CHECK_EQ(subtree_locale.tag(), std::string("en"));
}

AURORA_TEST_CASE(provider_state_switch_reresolves_message_text) {
    auto win = make_window("locale_switch_text");
    MessageCatalog &catalog = default_catalog();
    // 词条由本用例自建（两档 ASCII 模板），既避开把译文写进源码，也让断言可读回字面值。
    AURORA_TEST_REQUIRE(catalog.register_vocabulary(1024U, std::vector<std::string>{"itest.items"}).ok());
    AURORA_TEST_REQUIRE(catalog.add(Locale{.language = "en"}, "itest.items", "count {n}").ok());
    AURORA_TEST_REQUIRE(catalog.add(Locale{.language = "de"}, "itest.items", "anzahl {n}").ok());
    catalog.set_default_locale(Locale{.language = "en"});
    const MessageId id = catalog.find_by_name("itest.items");
    AURORA_TEST_REQUIRE_MSG(id != AURORA_INVALID_MESSAGE_ID, "registered name resolves to a vocabulary Id");

    auto locale_state = std::make_shared<State<Locale>>(Locale{.language = "en"});
    Node text_node{Text{TextProps{.content = LocalizedString::tr(id, {MessageArg{"n", std::int64_t{3}}})}}};
    Node node{LocaleProvider{locale_state, text_node}};
    (void)win->present_root(node);

    const Widget *rendered = first_child(node.widget());  // Provider 的直接子控件即那条 Text
    AURORA_TEST_REQUIRE_MSG(rendered != nullptr, "provider holds its child");
    AURORA_TEST_CHECK_EQ(rendered->accessibility_label(), std::string("count 3"));

    // 换档 = 只改外部 State：子树无任何自身脏标记，靠 Provider 的向下缓存失效整片重排重录。
    locale_state->set(Locale{.language = "de"});
    (void)win->present_root(node);
    AURORA_TEST_CHECK_EQ(rendered->accessibility_label(), std::string("anzahl 3"));

    // 再换回来：失效不是一次性的（否则第二次切换会停在上一档）。
    locale_state->set(Locale{.language = "en"});
    (void)win->present_root(node);
    AURORA_TEST_CHECK_EQ(rendered->accessibility_label(), std::string("count 3"));
}

}  // namespace aurora::test_cases::itest_locale_injection
