// 目标源单元：media/video_player.h + src/aurora/media/video_player.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_video_player.cpp
//   - test_video_player_subclass.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <cstdio>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/media/image_sequence_source.h"
#include "aurora/media/video_player.h"
#include "aurora/render/offscreen.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Constraints;
using aurora::Image;
using aurora::ImageSequenceSource;
using aurora::Json;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::px;
using aurora::Row;
using aurora::Size;
using aurora::VideoPlayer;

namespace aurora::tests::sec_video_player {

namespace {

auto solid_frame(int w, const int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> Image {
    std::vector<std::uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U, 0U);
    for (size_t i = 0; i < px.size(); i += 4U) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i] = r;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 1U] = g;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 2U] = b;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 3U] = 255U;
    }
    return Image{.width = w, .height = h, .pixels = std::move(px)};
}

auto make_source() -> std::shared_ptr<ImageSequenceSource> {
    auto src = std::make_shared<ImageSequenceSource>();
    src->set_fps(2.0);
    src->set_frames({solid_frame(16, 9, 255, 0, 0), solid_frame(16, 9, 0, 255, 0), solid_frame(16, 9, 0, 0, 255)});
    return src;
}

}  // namespace

namespace {
// 测试辅助：暴露受保护的 on_pointer_event，便于模拟点击手势。
class TestPlayer : public VideoPlayer {
  public:
    auto tap(Point p) -> void {
        MouseEvent pr;
        pr.action = MouseAction::Press;
        pr.local_position = p;
        on_pointer_event(pr);
        MouseEvent re;
        re.action = MouseAction::Release;
        re.local_position = p;
        on_pointer_event(re);
    }
};
}  // namespace

static void run() {
    BuildContext ctx;
    Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 320.0F, .height = 180.0F}};

    // ---- 自然尺寸 / 布局 ----
    {
        VideoPlayer player;
        player.set_source(make_source());
        player.width(px(320));
        player.height(px(180));
        const Size s = player.layout(c, ctx);
        AURORA_TEST_CHECK(s.width == 320.0F);
        AURORA_TEST_CHECK(s.height == 180.0F);
    }

    // ---- 离屏渲染：逻辑快照验证树结构（含控件叠层），PNG 验证 paint 不崩溃 ----
    {
        auto player = std::make_unique<VideoPlayer>();
        player->set_source(make_source());
        player->width(px(320));
        player->height(px(180));
        Node node{std::move(player)};

        const Json snap = render_to_logical_snapshot(node, 320, 180);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(snap["type"] == "VideoPlayer");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(!snap["children"].empty());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(snap["children"][0]["type"] == "VideoControls");

        const auto png = render_to_png(node, 320, 180, "test_video_player_out.png");
        AURORA_TEST_CHECK(static_cast<bool>(png) && png.value());
        std::remove("test_video_player_out.png");
    }

    // ---- 公开播放控制 API ----
    {
        VideoPlayer player;
        player.set_source(make_source());
        player.toggle_play();
        AURORA_TEST_CHECK(player.is_playing());
        player.seek_fraction(0.5);
        AURORA_TEST_CHECK(std::abs(player.position_fraction() - 0.5) < 1e-6);
        player.set_volume(0.3);
        AURORA_TEST_CHECK(std::abs(player.volume() - 0.3) < 1e-9);
        player.set_muted(true);
        AURORA_TEST_CHECK(player.muted());
        player.pause();
        AURORA_TEST_CHECK(!player.is_playing());
    }

    // ---- 扩展点④：点击手势（默认 toggle_play） ----
    {
        TestPlayer player;
        player.set_source(make_source());
        player.width(px(320));
        player.height(px(180));
        player.layout(c, ctx);
        AURORA_TEST_CHECK(!player.is_playing());
        player.tap(Point{.x = 20.0F, .y = 20.0F});  // 视频画面区域（非底部控件）
        AURORA_TEST_CHECK(player.is_playing());  // 默认 on_tap → toggle_play
    }

    // ---- 扩展点④：自定义 on_tap 回调（覆盖默认行为） ----
    {
        TestPlayer player;
        player.set_source(make_source());
        player.width(px(320));
        player.height(px(180));
        player.layout(c, ctx);
        bool tapped = false;
        player.set_on_tap([&tapped]() -> void { tapped = true; });
        player.tap(Point{.x = 20.0F, .y = 20.0F});
        AURORA_TEST_CHECK(tapped);
        AURORA_TEST_CHECK(!player.is_playing());  // 自定义回调未调用 toggle_play
    }

    // ---- 扩展点②：整体替换控件叠层 ----
    {
        VideoPlayer player;
        player.set_source(make_source());
        auto custom = std::make_unique<Row>();
        custom->adopt_children({Node(std::make_unique<Button>("Custom"))});
        player.set_controls(std::move(custom));
        player.toggle_play();
        AURORA_TEST_CHECK(player.is_playing());  // 控制器仍可用
        player.toggle_play();
    }
}
}  // namespace aurora::tests::sec_video_player

namespace aurora::tests::sec_video_player_subclass {
namespace au = aurora;

namespace {
// #1: create_default_controls() 在挂载期生效（子类覆写真正可达，set_controls 仍优先）
class MinimalPlayer : public VideoPlayer {
  public:
    Button *marker_ = nullptr;

    [[nodiscard]] auto create_default_controls() -> std::unique_ptr<Widget> override {
        auto b = std::make_unique<Button>("X");
        marker_ = b.get();  // NOLINT
        return b;
    }
};
}  // namespace

namespace {
// #2/#5: current_frame() 受保护访问器可访问；on_pointer_event/wants_click 为 public
class WatermarkPlayer : public VideoPlayer {
  public:
    auto frame_w() const -> int { return current_frame().width; }
    auto frame_h() const -> int { return current_frame().height; }

  protected:
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        VideoPlayer::on_paint(p, b, ctx);
    }
};
}  // namespace

static void run() {
    constexpr BuildContext ctx;

    MinimalPlayer mp;
    mp.mount(ctx);
    AURORA_TEST_CHECK(mp.marker_ != nullptr);  // 默认控件被 adopt
    AURORA_TEST_CHECK(!mp.child_nodes().empty());  // 挂在子树

    WatermarkPlayer wp;
    wp.mount(ctx);
    AURORA_TEST_CHECK(wp.frame_w() == 0 && wp.frame_h() == 0);  // 默认无帧，current_frame() 可访问
    AURORA_TEST_CHECK(wp.wants_click() == true);  // #5: on_pointer_event/wants_click 为 public
}
}  // namespace aurora::tests::sec_video_player_subclass

AURORA_TEST() {
    aurora::tests::sec_video_player::run();
    aurora::tests::sec_video_player_subclass::run();
}