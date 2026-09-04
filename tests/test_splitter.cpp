// 验证 Splitter：布局分配、比例钳制、拖拽调整、序列化。

#include <memory>

#include "aurora/widget/splitter.h"
#include "aurora/widget/text.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::HSplitter;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Size;
using aurora::Splitter;
using aurora::SplitterOrientation;
using aurora::Text;
using aurora::VSplitter;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{s};
    return Node{std::move(t)};
}

auto layout_splitter(std::shared_ptr<Splitter> const &sp, float w, float h) -> void {
    constexpr BuildContext ctx;
    sp->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = w, .height = h};
    sp->layout(c, ctx);
}

}  // namespace

AURORA_TEST() {
    // ---- 1. HSplitter 水平布局分配 ----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("left"), make_text("right"), 0.3F));
        layout_splitter(sp, 606.0F, 400.0F);  // 可用 = 606 - 6(handle) = 600

        AURORA_TEST_CHECK(sp->ratio_value() == 0.3F);
        const auto kids = sp->child_nodes();
        AURORA_TEST_CHECK(kids.size() == 2);
        // 左区域宽 = 600 * 0.3 = 180
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(kids[0].bounds().size.width - 180.0F) < 0.5F);
        // 右区域从 180 + 6 = 186 开始
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(kids[1].bounds().origin.x - 186.0F) < 0.5F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(kids[1].bounds().size.width - 420.0F) < 0.5F);
    }

    // ---- 2. VSplitter 垂直布局分配 ----
    {
        auto sp = std::make_shared<Splitter>(VSplitter(make_text("top"), make_text("bottom"), 0.5F));
        layout_splitter(sp, 400.0F, 306.0F);  // 可用 = 300

        const auto kids = sp->child_nodes();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(kids[0].bounds().size.height - 150.0F) < 0.5F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(kids[1].bounds().origin.y - 156.0F) < 0.5F);
    }

    // ---- 3. 比例钳制（min_first/min_second）----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("a"), make_text("b"), 0.5F));
        sp->set_min_sizes(100.0F, 100.0F);
        layout_splitter(sp, 406.0F, 300.0F);  // 可用 = 400

        // 0.1 * 400 = 40 < min_first(100) → 钳制到 0.25
        sp->set_ratio(0.1F);
        AURORA_TEST_CHECK(std::abs(sp->ratio_value() - 0.25F) < 0.01F);

        // 0.9 * 400 = 360 → second = 40 < 100 → 钳制到 0.75
        sp->set_ratio(0.95F);
        AURORA_TEST_CHECK(std::abs(sp->ratio_value() - 0.75F) < 0.01F);
    }

    // ---- 4. 拖拽分隔条 ----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("a"), make_text("b"), 0.5F));
        layout_splitter(sp, 406.0F, 300.0F);  // 可用 400，handle 在 200~206

        AURORA_TEST_CHECK(!sp->is_dragging());

        // 按下分隔条
        MouseEvent press;
        press.action = MouseAction::Press;
        press.local_position = Point{.x = 203.0F, .y = 150.0F};
        sp->on_pointer_event(press);
        AURORA_TEST_CHECK(press.is_handled_);
        AURORA_TEST_CHECK(sp->is_dragging());

        // 拖动到 x=100（比例应变为约 (100-3)/400 ≈ 0.2425）
        MouseEvent move;
        move.action = MouseAction::Move;
        move.local_position = Point{.x = 100.0F, .y = 150.0F};
        sp->on_pointer_event(move);
        AURORA_TEST_CHECK(move.is_handled_);
        AURORA_TEST_CHECK(std::abs(sp->ratio_value() - 0.2425F) < 0.01F);

        // 释放
        MouseEvent release;
        release.action = MouseAction::Release;
        release.local_position = Point{.x = 100.0F, .y = 150.0F};
        sp->on_pointer_event(release);
        AURORA_TEST_CHECK(!sp->is_dragging());
    }

    // ---- 5. 非分隔条区域按下不进入拖拽 ----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("a"), make_text("b"), 0.5F));
        layout_splitter(sp, 406.0F, 300.0F);

        MouseEvent press;
        press.action = MouseAction::Press;
        press.local_position = Point{.x = 50.0F, .y = 150.0F};  // 左区域
        sp->on_pointer_event(press);
        AURORA_TEST_CHECK(!sp->is_dragging());
    }

    // ---- 6. 比例变化回调 ----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("a"), make_text("b"), 0.5F));
        layout_splitter(sp, 406.0F, 300.0F);

        float last_ratio = -1.0F;
        sp->set_on_ratio_change([&last_ratio](float r) -> void { last_ratio = r; });

        sp->set_ratio(0.4F);
        AURORA_TEST_CHECK(std::abs(last_ratio - 0.4F) < 0.01F);

        // 相同值不重复回调
        last_ratio = -1.0F;
        sp->set_ratio(0.4F);
        AURORA_TEST_CHECK(last_ratio == -1.0F);
    }

    // ---- 7. 序列化往返 ----
    {
        auto sp = std::make_shared<Splitter>(VSplitter(make_text("a"), make_text("b"), 0.6F));
        sp->set_min_sizes(30.0F, 40.0F);
        sp->set_handle_size(8.0F);
        layout_splitter(sp, 400.0F, 308.0F);

        aurora::Json props;
        sp->serialize_props(props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["orientation"].get<std::string>() == "vertical");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(std::abs(props["ratio"].get<float>() - 0.6F) < 0.01F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["min_first"].get<float>() == 30.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["handle_size"].get<float>() == 8.0F);

        auto sp2 = std::make_shared<Splitter>();
        sp2->deserialize_props(props);
        AURORA_TEST_CHECK(std::abs(sp2->ratio_value() - 0.6F) < 0.01F);
    }

    // ---- 8. 无头渲染不崩溃 ----
    {
        auto sp = std::make_shared<Splitter>(HSplitter(make_text("L"), make_text("R"), 0.5F));
        layout_splitter(sp, 320.0F, 240.0F);

        aurora::Painter p;
        p.begin(320, 240);
        BuildContext ctx;
        sp->paint(p,
                  aurora::Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 240.0F}},
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }
}