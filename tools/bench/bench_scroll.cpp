// Scroll performance benchmark: use `ScrollBenchHarness` under Headless to run deterministic scroll
// sequences against a "real workload tree", producing p99 / jitter / full_redraw_frames and
// RenderCounters baselines for "before-optimization / after-optimization" comparison tables.
//
// Notes:
// - This program is a "benchmark/diagnostic" tool, not a unit test; it is not wired into CTest
//   (timings are affected by environment jitter). But the counter readings are deterministic under
//   Headless; the regression assertions are owned by tests/unit/utest_scroll_bench.cpp.
// - Counters only have values in a build with `AURORA_ENABLE_PROFILING=ON`; timings should be
//   collected in Release + PROFILING=OFF (see codespec/BUILD_OPTIONS.md).
// - Output goes to stdout via AURORA_LOG_RAW (project hard rule #8: program product output uses the
//   raw channel).
//
// Scenes:
// - `app`       : primary workload -- a self-contained app-shell-shaped tree (top bar + search +
//                 auto-advancing banner carousel + chip row + static grid of layer-cached cells +
//                 static reco row + review cards + bottom nav). It reproduces the workload
//                 characteristics of a real store-app home page: heterogeneous sections,
//                 text-dense cells, canvas icons, layer-cached cells and a self-driving carousel
//                 (a tree that never goes idle -- the settle phase must take the wall-clock exit
//                 rather than the idle exit). The page Scroll is the only scrollable in the tree
//                 on purpose: the harness dispatches each wheel event to the deepest scrollable
//                 under the viewport center, so any nested scrollable would steal the page wheel
//                 while the probe watches the page Scroll -- a silently corrupted reading (the
//                 retired google_play scene nested a GridView + LazyRow and measured ~0.9 ms,
//                 which was the inner grid's scroll, not the page's). LazyList / GridView
//                 self-driven scroll paths are covered by the lazy / grid isolated scenes.
// - `synthetic` : control group -- a stable 200-row x 6-Chip long list decoupled from any app
//                 shape, for trend comparison in demo-less environments / CI.
// - `lazy` / `grid` : L5-C isolated scenes -- LazyList / GridView driven directly as roots, to
//                 measure their self-driven scroll paths (not via the outer Scroll offscreen buffer).
//
// Usage: run `bench_scroll --help`; the whole option set lives in the declaration table below.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/canvas.h"
#include "aurora/widget/chip.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"

using aurora::Chip;
using aurora::Column;
using aurora::ColumnProps;
using aurora::GridView;
using aurora::LazyList;
using aurora::Node;
using aurora::profiling_enabled;
using aurora::Row;
using aurora::RowProps;
using aurora::Scroll;
using aurora::ScrollBenchHarness;
using aurora::ScrollProps;
using aurora::Size;

namespace {

// ---- app 场景常量（自包含场景不依赖 examples/ 下任何 fixture 头）----
constexpr int AURORA_APP_GRID_ITEMS = 72;            ///< 静态网格条目数（grid_cols 列 x 18 行 @1100dp）
constexpr float AURORA_APP_GRID_CELL_H = 132.0F;     ///< 网格单元格高（dp）
constexpr int AURORA_APP_RECO_VISIBLE = 4;           ///< 推荐行可见条目数（静态行，非可滚动）
constexpr int AURORA_APP_REVIEWS = 8;                ///< 评论卡片数（文本密集段）
constexpr float AURORA_APP_CAROUSEL_H = 196.0F;      ///< 轮播叶高（dp）
constexpr float AURORA_APP_CAROUSEL_FIRST_S = 1.2F;  ///< 首次自动切换的墙钟秒数（落在落定窗口内）
constexpr float AURORA_APP_CAROUSEL_PERIOD_S = 2.4F; ///< 自动切换周期（秒）

/// @brief app 场景固定调色板（基准不需要主题切换；真实换肤路径由 other 用例覆盖）。
struct BenchPalette {
    au::Color window{244, 246, 250};
    au::Color surface{255, 255, 255};
    au::Color surface_variant{228, 232, 240};
    au::Color primary{23, 92, 222};
    au::Color on_primary{255, 255, 255};
    au::Color text{24, 28, 35};
    au::Color text_secondary{108, 116, 128};
    au::Color accent{0, 150, 136};
    au::Color accent_warm{216, 101, 41};
};

constexpr BenchPalette AURORA_BENCH_APP_PALETTE;

/// @brief 轮播横幅数据（本地生成，确定性）。
struct BenchBanner {
    std::string title;
    std::string subtitle;
    au::Color tile_a;
    au::Color tile_b;
};

/// @brief 网格单元数据（本地生成，确定性）。
struct BenchCell {
    std::string name;
    std::string rating;
    au::Color tile_a;
    au::Color tile_b;
};

/// @brief 横向推荐行条目数据（本地生成，确定性）。
struct BenchReco {
    std::string title;
    std::string subtitle;
    au::Color tile_a;
    au::Color tile_b;
};

[[nodiscard]] auto app_banners() -> std::vector<BenchBanner> {
    return {
        {"Aurora Waves", "Music - Editor's choice", au::Color{23, 92, 222}, au::Color{94, 53, 211}},
        {"Nightfall RPG", "Role playing - New release", au::Color{216, 101, 41}, au::Color{180, 60, 120}},
        {"Focus Timer", "Productivity - Featured", au::Color{0, 150, 136}, au::Color{20, 110, 90}},
    };
}

[[nodiscard]] auto app_cells() -> std::vector<BenchCell> {
    static const std::string kNames[] = {
        "Atlas Map",     "Beacon Mail",   "Cursor Note",   "Driftwood FM",  "Ember Camera",
        "Fern Reader",   "Glyph Editor",  "Harbor Chat",   "Inkwell Draw",  "Juno Weather",
        "Kite Tasks",    "Lantern News",  "Mosaic Fit",    "Nimbus Drive",  "Orbit Calendar",
        "Parcel Track",  "Quill Diary",   "Ripple Timer",  "Slate Budget",  "Tidal Podcast",
        "Umbra Theme",   "Vault Notes",   "Willow Sleep",  "Zephyr Mail",
    };
    static const au::Color kTiles[][2] = {
        {au::Color{23, 92, 222}, au::Color{94, 53, 211}},  {au::Color{0, 150, 136}, au::Color{20, 110, 90}},
        {au::Color{216, 101, 41}, au::Color{180, 60, 120}}, {au::Color{109, 76, 175}, au::Color{63, 81, 181}},
        {au::Color{194, 24, 91}, au::Color{136, 14, 79}},   {au::Color{56, 142, 60}, au::Color{27, 94, 32}},
        {au::Color{2, 119, 189}, au::Color{1, 87, 155}},    {au::Color{245, 127, 23}, au::Color{230, 81, 0}},
    };
    std::vector<BenchCell> cells;
    cells.reserve(AURORA_APP_GRID_ITEMS);
    for (int i = 0; i < AURORA_APP_GRID_ITEMS; ++i) {
        const auto &name = kNames[static_cast<std::size_t>(i) % std::size(kNames)];
        const auto &tile = kTiles[static_cast<std::size_t>(i) % std::size(kTiles)];
        const int major = i / std::size(kNames);
        cells.push_back(BenchCell{
            major == 0 ? name : name + " " + std::to_string(major + 1),
            "4." + std::to_string(1 + (i % 9)) + " (" + std::to_string(120 + (i * 37) % 4800) + ")",
            tile[0],
            tile[1],
        });
    }
    return cells;
}

[[nodiscard]] auto app_recos() -> std::vector<BenchReco> {
    static const std::string kNames[] = {
        "Aurora Waves", "Nightfall RPG", "Focus Timer", "Atlas Map",
    };
    static const au::Color kTiles[][2] = {
        {au::Color{23, 92, 222}, au::Color{94, 53, 211}},  {au::Color{0, 150, 136}, au::Color{20, 110, 90}},
        {au::Color{216, 101, 41}, au::Color{180, 60, 120}}, {au::Color{109, 76, 175}, au::Color{63, 81, 181}},
    };
    std::vector<BenchReco> recos;
    recos.reserve(AURORA_APP_RECO_VISIBLE);
    for (int i = 0; i < AURORA_APP_RECO_VISIBLE; ++i) {
        const auto &tile = kTiles[static_cast<std::size_t>(i) % std::size(kTiles)];
        recos.push_back(BenchReco{
            kNames[static_cast<std::size_t>(i) % std::size(kNames)],
            "4." + std::to_string(1 + (i % 9)) + " - " + std::to_string(40 + (i * 13) % 300) + " MB",
            tile[0],
            tile[1],
        });
    }
    return recos;
}

/// @brief 应用商店横幅轮播叶：自驱动（墙钟推进切换 + 平滑滑动），卡片层一次性渲染进离屏层、
/// 每帧仅平移合成。树因此在轮播期间永不动画停歇——基准落定阶段必须走墙钟出口而非空闲出口。
class AppBannerCarousel : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据字段由工厂直接赋值（数据载体式），刻意公开。
    std::vector<BenchBanner> items;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "AppBannerCarousel"; }
    // 横幅每帧整体平移 + 入场淡入：Display List 缓存会冻结平移与 alpha 首帧，故禁 DL 缓存，
    // 改为卡片一次性渲染进离屏层、每帧仅 composite。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        const float w = c.max.width;
        banner_w_ = w * 0.86F;
        step_ = banner_w_ + 16.0F;
        left_ = (w - banner_w_) * 0.5F;
        return c.constrain(au::Size{.width = w, .height = AURORA_APP_CAROUSEL_H});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const BenchPalette &th = AURORA_BENCH_APP_PALETTE;
        const int n = static_cast<int>(items.size());

        // 自驱动（不依赖 Scheduler：无头基准下无 App 运行时同样永动）：墙钟 elapsed 决定目标卡片，
        // 滑动由 on_paint 每帧按 dt 推进，过渡结束自然停止。
        const auto now = std::chrono::steady_clock::now();
        if (!clock_started_) {
            clock_started_ = true;
            start_ = now;
            last_ = now;
        }
        const float dt = std::clamp(std::chrono::duration<float>(now - last_).count(), 0.0F, 0.1F);
        last_ = now;
        const float elapsed = std::chrono::duration<float>(now - start_).count();
        if (n > 1) {
            const float since_first = elapsed - AURORA_APP_CAROUSEL_FIRST_S;
            const int next_target =
                since_first >= 0.0F ? static_cast<int>(since_first / AURORA_APP_CAROUSEL_PERIOD_S) % n : 0;
            if (next_target != target_) {
                target_ = next_target;
            }
            const float target_off = -static_cast<float>(target_) * step_;
            const float diff = target_off - offset_;
            if (std::fabs(diff) > 0.5F) {
                const float speed = step_ / 0.45F;  // 约 0.45s 滑到位
                const float step = std::clamp(speed * dt, 0.0F, std::fabs(diff));
                offset_ += (diff > 0.0F ? 1.0F : -1.0F) * step;
                mark_needs_paint();  // 滑动中每帧驱动，过渡结束自然停止
            } else {
                offset_ = target_off;
            }
        }

        // 入场淡入（0.35s）
        float ent = 1.0F;
        if (!appear_started_) {
            appear_started_ = true;
            appear_start_ = now;
        }
        const float t = std::chrono::duration<float>(now - appear_start_).count() / 0.35F;
        ent = 1.0F - (1.0F - std::clamp(t, 0.0F, 1.0F)) * (1.0F - std::clamp(t, 0.0F, 1.0F));
        if (ent < 1.0F) {
            mark_needs_paint();
        }
        p.set_alpha(static_cast<double>(ent));

        // 离屏层缓存：卡片内容（渐变 + 图标 + 文本）在滑动期间每帧不变，仅整体平移。
        card_h_ = b.size.height - 24.0F;
        if (n > 0 && banner_w_ > 0.0F) {
            const float lw = static_cast<float>(n) * step_;
            const auto raster_gen = au::render::FontEngine::raster_generation();
            if (!layer_valid_ || std::fabs(layer_w_ - lw) > 0.5F || std::fabs(layer_h_ - card_h_) > 0.5F ||
                layer_gen_ != raster_gen) {
                layer_ = std::make_unique<au::Painter>();
                layer_->set_scale(p.scale());
                layer_->begin(static_cast<int>(std::ceil(lw)), static_cast<int>(std::ceil(card_h_)));
                render_banner_layer(*layer_, th, n);
                layer_w_ = lw;
                layer_h_ = card_h_;
                layer_gen_ = raster_gen;
                layer_valid_ = true;
            }
        }

        p.push_clip(b);
        p.set_alpha(static_cast<double>(ent));
        if (layer_ != nullptr) {
            // 平移按设备像素网格吸附，消除滑动中的亚像素重采样模糊。
            const float dev = p.scale();
            const float x_log = b.origin.x + left_ + offset_;
            const float x_snap = std::round(x_log * dev) / dev;
            p.composite(*layer_, au::Matrix2D::from_translate(x_snap, b.origin.y));
        } else {
            draw_banner_direct(p, th, n, b);  // 兜底：首帧 layout 前尺寸未就绪时直绘
        }
        if (n > 1) {
            const float total = static_cast<float>(n) * 14.0F;
            const float sx = b.origin.x + ((b.size.width - total) * 0.5F);
            const float cy = b.origin.y + b.size.height - 12.0F;
            for (int i = 0; i < n; ++i) {
                const bool active = (i == target_);
                const float dot_w = active ? 18.0F : 6.0F;
                const float dx = sx + (static_cast<float>(i) * 14.0F) + ((14.0F - dot_w) * 0.5F);
                p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = dx, .y = cy - 3.0F},
                                             .size = au::Size{.width = dot_w, .height = 6.0F}},
                                    3.0F, active ? th.primary : th.text_secondary.with_alpha(120));
            }
        }
        p.pop_clip();
        p.set_alpha(1.0);
    }

    // 在给定 Painter 上绘制第 i 张卡片（卡片左上角 (x,y)）；卡片内容静态，离屏层与兜底直绘共用。
    auto draw_card(au::Painter &p, const BenchPalette &th, int i, float x, float y) const -> void {
        const au::Rect card{.origin = au::Point{.x = x, .y = y},
                            .size = au::Size{.width = banner_w_, .height = card_h_}};
        p.push_clip_rounded(card, 20.0F);
        const BenchBanner &bn = items[static_cast<std::size_t>(i)];  // NOLINT(*-redundant-parentheses)
        p.fill_rect(card, bn.tile_a);
        p.draw_linear_gradient(card, card.origin, au::Point{.x = card.origin.x, .y = card.origin.y + card.size.height},
                               {bn.tile_a, bn.tile_b}, {0.0F, 1.0F});
        p.pop_clip();
        constexpr float icon = 64.0F;
        const au::Rect tile{.origin = au::Point{.x = card.origin.x + 18.0F, .y = card.origin.y + 18.0F},
                            .size = au::Size{.width = icon, .height = icon}};
        p.fill_rounded_rect(tile, 16.0F, au::Color{255, 255, 255, 60});
        p.draw_text(au::Rect{.origin = tile.origin, .size = tile.size},
                    bn.title.substr(0, 1), au::Font{.size_pt = 26.0F, .weight = 700}, th.on_primary);
        float ty = card.origin.y + 18.0F + icon + 12.0F;
        p.draw_text(au::Rect{.origin = au::Point{.x = card.origin.x + 18.0F, .y = ty},
                             .size = au::Size{.width = card.size.width - 36.0F, .height = 24}},
                    bn.title, au::Font{.size_pt = 18.0F, .weight = 700}, th.on_primary);
        ty += 26.0F;
        p.draw_text(au::Rect{.origin = au::Point{.x = card.origin.x + 18.0F, .y = ty},
                             .size = au::Size{.width = card.size.width - 36.0F, .height = 18}},
                    bn.subtitle, au::Font{.size_pt = 13.0F}, th.on_primary.with_alpha(230));
    }

    // 一次性把全部卡片渲染进离屏层（层局部坐标：卡片 i 位于 x=i*step_，y=0）。
    auto render_banner_layer(au::Painter &lp, const BenchPalette &th, int n) const -> void {
        for (int i = 0; i < n; ++i) {
            draw_card(lp, th, i, static_cast<float>(i) * step_, 0.0F);
        }
    }

    // 兜底：首帧 layout 前尺寸未就绪时直绘（按当前 offset_ 摆放）。
    auto draw_banner_direct(au::Painter &p, const BenchPalette &th, int n, const au::Rect &b) const -> void {
        for (int i = 0; i < n; ++i) {
            const float x = b.origin.x + left_ + (static_cast<float>(i) * step_) + offset_;
            if (x > b.origin.x + b.size.width || x + banner_w_ < b.origin.x) {
                continue;
            }
            draw_card(p, th, i, x, b.origin.y);
        }
    }

  private:
    float card_h_ = 0.0F;  ///< 卡片高（离屏层与直绘共用）
    std::unique_ptr<au::Painter> layer_;  ///< 卡片带离屏层（一次性渲染，每帧平移合成）
    bool layer_valid_ = false;
    float layer_w_ = 0.0F;
    float layer_h_ = 0.0F;
    std::uint64_t layer_gen_ = 0;  ///< 离屏层生成时的光栅状态世代（AA 模式变更需重建）
    float offset_ = 0.0F;
    float step_ = 0.0F;
    float banner_w_ = 0.0F;
    float left_ = 0.0F;
    int target_ = 0;
    bool clock_started_ = false;
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point last_{};
    bool appear_started_ = false;
    std::chrono::steady_clock::time_point appear_start_{};
};

/// @brief 虚拟化网格单元叶：色块 + 名称 + 评分，整格一次性渲染进离屏层后每帧仅合成
/// （复刻真实商店网格「图标 + 两行文本」的格缓存形态）。
class AppCellLeaf : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据字段由 GridView 工厂直接赋值（数据载体式），刻意公开。
    std::string name;
    std::string rating;
    au::Color tile_a{23, 92, 222};
    au::Color tile_b{94, 53, 211};
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "AppCellLeaf"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = AURORA_APP_GRID_CELL_H});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const BenchPalette &th = AURORA_BENCH_APP_PALETTE;
        const auto raster_gen = au::render::FontEngine::raster_generation();
        if (!layer_valid_ || std::fabs(layer_w_ - b.size.width) > 0.5F ||
            std::fabs(layer_h_ - b.size.height) > 0.5F || layer_gen_ != raster_gen) {
            layer_ = std::make_unique<au::Painter>();
            layer_->set_scale(p.scale());
            layer_->begin(static_cast<int>(std::ceil(b.size.width)), static_cast<int>(std::ceil(b.size.height)));
            layer_w_ = b.size.width;
            layer_h_ = b.size.height;
            layer_gen_ = raster_gen;
            layer_valid_ = true;
            const au::Rect local{.origin = au::Point{.x = 0.0F, .y = 0.0F}, .size = b.size};
            layer_->fill_rounded_rect(local, 14.0F, th.surface);
            constexpr float tile = 72.0F;
            const au::Rect icon{.origin = au::Point{.x = 12.0F, .y = 12.0F},
                                .size = au::Size{.width = tile, .height = tile}};
            layer_->fill_rounded_rect(icon, 16.0F, tile_a);
            layer_->draw_linear_gradient(icon, icon.origin,
                                         au::Point{.x = icon.origin.x, .y = icon.origin.y + icon.size.height},
                                         {tile_a, tile_b}, {0.0F, 1.0F});
            layer_->draw_text(au::Rect{.origin = icon.origin, .size = icon.size}, name.substr(0, 1),
                              au::Font{.size_pt = 28.0F, .weight = 700}, th.on_primary);
            const float tx = icon.origin.x + tile + 12.0F;
            const float tw = b.size.width - tx - 12.0F;
            layer_->draw_text(
                au::Rect{.origin = au::Point{.x = tx, .y = 16.0F}, .size = au::Size{.width = tw, .height = 24}},
                name, au::Font{.size_pt = 15.0F, .weight = 600}, th.text);
            layer_->draw_text(
                au::Rect{.origin = au::Point{.x = tx, .y = 44.0F}, .size = au::Size{.width = tw, .height = 20}},
                rating, au::Font{.size_pt = 12.0F}, th.text_secondary);
            layer_->draw_text(
                au::Rect{.origin = au::Point{.x = 12.0F, .y = 96.0F}, .size = au::Size{.width = tw + tile, .height = 18}},
                "Free - In-app purchases", au::Font{.size_pt = 11.0F}, th.accent);
        }
        p.composite(*layer_, au::Matrix2D::from_translate(b.origin.x, b.origin.y));
    }

  private:
    std::unique_ptr<au::Painter> layer_;
    bool layer_valid_ = false;
    float layer_w_ = 0.0F;
    float layer_h_ = 0.0F;
    std::uint64_t layer_gen_ = 0;
};

/// @brief Synthetic control scene: a stable long list decoupled from the demo, for trend comparison
/// in demo-less environments / CI.
/// 200 rows x 6 Chips; total content height far exceeds the viewport, so the whole scroll runs on
/// real content.
[[nodiscard]] auto build_synthetic_tree() -> Node {
    std::vector<Node> rows;
    rows.reserve(200);
    for (int r = 0; r < 200; ++r) {
        std::vector<Node> cells;
        cells.reserve(6);
        for (int c = 0; c < 6; ++c) {
            auto chip = std::make_shared<Chip>();
            chip->set_label("item " + std::to_string(r) + "-" + std::to_string(c));
            cells.emplace_back(chip);
        }
        rows.emplace_back(std::make_shared<Row>(RowProps{.children = std::move(cells)}));
    }
    auto col = std::make_shared<Column>(ColumnProps{.children = std::move(rows)});
    return Node{std::make_shared<Scroll>(ScrollProps{.child = Node{std::move(col)}})};
}

/// @brief app 场景的 Section 头（色条 + 标题）。
[[nodiscard]] auto section_header(const std::string &title) -> Node {
    const BenchPalette &th = AURORA_BENCH_APP_PALETTE;
    auto bar = std::make_shared<au::Canvas>(4.0F, 18.0F, [th](au::Painter &p, const au::Rect &b) -> void {
        p.fill_rounded_rect(b, 2.0F, th.accent);
    });
    auto text = std::make_shared<au::Text>(au::TextProps{
        .content = title, .font = au::Font{.size_pt = 15.0F, .weight = 700}, .text_color = th.text});
    text->modifier = au::Modifier{}.padding(au::EdgeInsets{.left = 10.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    auto row = std::make_shared<Row>();
    row->add(Node{bar});
    row->add(Node{text});
    return Node{row};
}

/// @brief app 场景主体：Store 首页形态（自包含，无外部数据/fixture 依赖）。
///
/// 结构复刻真实商店首页的负载特征：顶栏 + 搜索 → 纵向 Scroll 内混排
/// （自驱动轮播 / chip 行 / 静态层缓存网格 / 静态推荐行 / 评论卡片）→ 底部导航。
/// 页面 Scroll 是树中唯一可滚动控件（harness 的滚轮归最深的 wants_scroll 控件，
/// 嵌套可滚动控件会抢走页面滚轮、使探针读数失真）；虚拟化自驱动滚动路径由
/// lazy / grid 隔离场景覆盖。
/// @param width 窗口逻辑宽度（dp），决定网格列数（宽约束 4 列、窄约束 3 列）。
[[nodiscard]] auto build_app_tree(float width) -> Node {
    const BenchPalette &th = AURORA_BENCH_APP_PALETTE;
    const int grid_cols = width > 760.0F ? 4 : 3;

    // ---- 顶栏（矢量图标 + 标题 + 头像）----
    auto bar = std::make_shared<Row>();
    auto menu = std::make_shared<au::Canvas>(26.0F, 26.0F, [th](au::Painter &p, const au::Rect &b) -> void {
        const float w = b.size.width * 0.72F;
        const float x = b.origin.x + (b.size.width - w) * 0.5F;
        for (int i = 0; i < 3; ++i) {
            const float y = b.origin.y + 5.0F + (static_cast<float>(i) * 7.0F);
            p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = x, .y = y},
                                         .size = au::Size{.width = w, .height = 2.5F}},
                                1.25F, th.text);
        }
    });
    auto bar_title = std::make_shared<au::Text>(au::TextProps{
        .content = "Aurora Store", .font = au::Font{.size_pt = 20.0F, .weight = 700}, .text_color = th.primary});
    bar_title->modifier = au::Modifier{}.padding(au::EdgeInsets{.left = 12.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    auto avatar = std::make_shared<au::Canvas>(28.0F, 28.0F, [th](au::Painter &p, const au::Rect &b) -> void {
        p.fill_rounded_rect(b, 14.0F, th.primary);
        p.draw_text(b, "A", au::Font{.size_pt = 14.0F, .weight = 700}, th.on_primary);
    });
    bar->add(Node{menu});
    bar->add(Node{bar_title});
    bar->add(Node{avatar});
    bar->modifier = au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 10.0F, .right = 16.0F, .bottom = 10.0F});

    // ---- 搜索行（放大镜图标 + 输入框）----
    auto search_row = std::make_shared<Row>();
    auto search_icon = std::make_shared<au::Canvas>(20.0F, 20.0F, [th](au::Painter &p, const au::Rect &b) -> void {
        const au::Rect lens{.origin = b.origin, .size = au::Size{.width = 13.0F, .height = 13.0F}};
        p.fill_rounded_rect(lens, 7.0F, th.text_secondary);
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = b.origin.x + 9.0F, .y = b.origin.y + 12.0F},
                                     .size = au::Size{.width = 7.0F, .height = 3.0F}},
                            1.5F, th.text_secondary);
    });
    auto search_input = std::make_shared<au::TextInput>(au::TextInputProps{.placeholder = "Search apps and books"});
    search_row->add(Node{search_icon});
    search_row->add(Node{search_input});
    search_row->modifier =
        au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 0.0F, .right = 16.0F, .bottom = 8.0F});

    // ---- 纵向滚动主体 ----
    std::vector<Node> body;

    body.push_back(section_header("Featured"));
    auto carousel = std::make_shared<AppBannerCarousel>();
    carousel->items = app_banners();
    body.push_back(Node{carousel});

    body.push_back(section_header("Categories"));
    auto chips = std::make_shared<Row>();
    chips->modifier =
        au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 0.0F, .right = 16.0F, .bottom = 8.0F});
    static const std::string kCategories[] = {"Games", "Apps", "Movies", "Books", "Kids"};
    for (const auto &cat : kCategories) {
        auto chip = std::make_shared<Chip>();
        chip->set_label(cat);
        chips->add(Node{chip});
    }
    body.push_back(Node{chips});

    body.push_back(section_header("Top charts"));
    // 静态网格：18 行 x grid_cols 列层缓存单元格。
    // 刻意不用内嵌 GridView / LazyList：harness 的滚轮归「最深的 wants_scroll 控件」，
    // 内嵌纵向可滚动控件会把页面滚轮抢成内部滚动（探针跟踪的是页面 Scroll，读数即失真）；
    // LazyList / GridView 的自驱动滚动路径由 lazy / grid 两个隔离场景覆盖。
    auto cells = std::make_shared<std::vector<BenchCell>>(app_cells());
    std::vector<Node> grid_rows;
    const int grid_row_count = AURORA_APP_GRID_ITEMS / grid_cols;
    grid_rows.reserve(static_cast<std::size_t>(grid_row_count));
    for (int r = 0; r < grid_row_count; ++r) {
        std::vector<Node> row_cells;
        row_cells.reserve(static_cast<std::size_t>(grid_cols));
        for (int c = 0; c < grid_cols; ++c) {
            const BenchCell &data = (*cells)[static_cast<std::size_t>((r * grid_cols) + c)];  // NOLINT(*-redundant-parentheses)
            auto cell = std::make_shared<AppCellLeaf>();
            cell->name = data.name;
            cell->rating = data.rating;
            cell->tile_a = data.tile_a;
            cell->tile_b = data.tile_b;
            // 行内等分宽度（flex 权重），单元格自身按 max.width 布局。
            cell->modifier = au::Modifier{}
                                 .expand()
                                 .padding(au::EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
            row_cells.push_back(Node{cell});
        }
        grid_rows.push_back(Node{std::make_shared<Row>(RowProps{.children = std::move(row_cells)})});
    }
    body.push_back(Node{std::make_shared<Column>(ColumnProps{.children = std::move(grid_rows)})});

    body.push_back(section_header("Recommended for you"));
    // 静态推荐行：缩略图 + 两行文本。同理不用 LazyRow（横向可滚动控件同样会抢滚轮）。
    auto recos = std::make_shared<std::vector<BenchReco>>(app_recos());
    auto reco_row = std::make_shared<Row>();
    reco_row->modifier =
        au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 0.0F, .right = 16.0F, .bottom = 0.0F});
    for (int i = 0; i < AURORA_APP_RECO_VISIBLE; ++i) {
        const BenchReco &data = (*recos)[static_cast<std::size_t>(i)];  // NOLINT(*-redundant-parentheses)
        auto thumb = std::make_shared<au::Canvas>(96.0F, 96.0F, [data, th](au::Painter &p, const au::Rect &b) -> void {
            p.fill_rounded_rect(b, 14.0F, data.tile_a);
            p.draw_linear_gradient(b, b.origin, au::Point{.x = b.origin.x, .y = b.origin.y + b.size.height},
                                   {data.tile_a, data.tile_b}, {0.0F, 1.0F});
            p.draw_text(b, data.title.substr(0, 1), au::Font{.size_pt = 34.0F, .weight = 700}, th.on_primary);
        });
        auto title = std::make_shared<au::Text>(au::TextProps{
            .content = data.title, .font = au::Font{.size_pt = 14.0F, .weight = 600}, .text_color = th.text});
        auto subtitle = std::make_shared<au::Text>(au::TextProps{
            .content = data.subtitle, .font = au::Font{.size_pt = 12.0F}, .text_color = th.text_secondary});
        auto col = std::make_shared<Column>(ColumnProps{.children = std::vector<Node>{Node{title}, Node{subtitle}},
                                                .gap = 4.0F});
        auto item = std::make_shared<Row>();
        item->add(Node{thumb});
        item->add(Node{col});
        item->modifier = au::Modifier{}.expand();
        reco_row->add(Node{item});
    }
    body.push_back(Node{reco_row});

    body.push_back(section_header("Reviews"));
    static const std::string kReviewers[] = {"Lynn", "Mateo", "Priya", "Sana", "Igor", "Wen", "Dana", "Ravi"};
    static const std::string kBodies[] = {
        "Smooth scrolling and the layout adapts nicely when I rotate the window.",
        "Text stays crisp at every scale; the dark mode switch is instant.",
        "Virtualized lists stay responsive even with thousands of rows loaded.",
        "The banner carousel transitions without stuttering on low-end hardware.",
    };
    for (int i = 0; i < AURORA_APP_REVIEWS; ++i) {
        auto avatar_small = std::make_shared<au::Canvas>(36.0F, 36.0F, [i, th](au::Painter &p, const au::Rect &b) -> void {
            p.fill_rounded_rect(b, 18.0F, th.accent_warm);
            p.draw_text(b, kReviewers[static_cast<std::size_t>(i) % std::size(kReviewers)].substr(0, 1),
                        au::Font{.size_pt = 16.0F, .weight = 700}, th.on_primary);
        });
        auto name = std::make_shared<au::Text>(au::TextProps{
            .content = kReviewers[static_cast<std::size_t>(i) % std::size(kReviewers)],
            .font = au::Font{.size_pt = 13.0F, .weight = 600}, .text_color = th.text});
        auto meta = std::make_shared<au::Text>(au::TextProps{
            .content = "2026-09-1" + std::to_string(1 + (i % 9)),
            .font = au::Font{.size_pt = 11.0F}, .text_color = th.text_secondary});
        auto info = std::make_shared<Column>(
            ColumnProps{.children = std::vector<Node>{Node{name}, Node{meta}}, .gap = 2.0F});
        auto head = std::make_shared<Row>();
        head->add(Node{avatar_small});
        head->add(Node{info});
        auto body_text = std::make_shared<au::Text>(au::TextProps{
            .content = kBodies[static_cast<std::size_t>(i) % std::size(kBodies)],
            .font = au::Font{.size_pt = 12.0F}, .text_color = th.text, .max_lines = 2, .soft_wrap = true});
        auto card = std::make_shared<Column>(
            ColumnProps{.children = std::vector<Node>{Node{head}, Node{body_text}}, .gap = 6.0F});
        card->modifier = au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 8.0F, .right = 16.0F, .bottom = 8.0F});
        body.push_back(Node{card});
    }

    auto body_col = std::make_shared<Column>(ColumnProps{.children = std::move(body)});
    auto scroll = std::make_shared<Scroll>(ScrollProps{.child = Node{std::move(body_col)}});
    scroll->modifier = au::Modifier{}.expand();

    // ---- 底部导航 ----
    static const std::string kNavLabels[] = {"Home", "Top", "New", "Subs"};
    auto nav = std::make_shared<Row>();
    nav->modifier = au::Modifier{}.padding(au::EdgeInsets{.left = 16.0F, .top = 8.0F, .right = 16.0F, .bottom = 8.0F});
    for (int i = 0; i < 4; ++i) {
        auto icon = std::make_shared<au::Canvas>(22.0F, 22.0F, [i, th](au::Painter &p, const au::Rect &b) -> void {
            const au::Color c = (i == 0) ? th.primary : th.text_secondary;
            if (i == 0) {
                p.fill_rounded_rect(b, 5.0F, c);  // 首页：圆角方块
            } else if (i == 1) {
                p.fill_rounded_rect(au::Rect{.origin = b.origin, .size = au::Size{.width = b.size.width, .height = 5.0F}},
                                    2.5F, c);  // 榜单：横条
                p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = b.origin.x, .y = b.origin.y + 9.0F},
                                             .size = au::Size{.width = b.size.width * 0.6F, .height = 5.0F}},
                                    2.5F, c);
                p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = b.origin.x, .y = b.origin.y + 18.0F},
                                             .size = au::Size{.width = b.size.width * 0.8F, .height = 5.0F}},
                                    2.5F, c);
            } else if (i == 2) {
                p.fill_rounded_rect(b, 11.0F, c);  // 新：圆
            } else {
                p.fill_rounded_rect(au::Rect{.origin = b.origin, .size = au::Size{.width = b.size.width, .height = 16.0F}},
                                    4.0F, c);  // 订阅：卡片
            }
        });
        auto label = std::make_shared<au::Text>(au::TextProps{
            .content = kNavLabels[static_cast<std::size_t>(i)], .font = au::Font{.size_pt = 10.0F},
            .text_color = (i == 0) ? th.primary : th.text_secondary});
        auto item = std::make_shared<Column>(ColumnProps{.children = std::vector<Node>{Node{icon}, Node{label}},
                                                        .gap = 3.0F});
        nav->add(Node{item});
    }

    std::vector<Node> root;
    root.push_back(Node{bar});
    root.push_back(Node{search_row});
    root.push_back(Node{scroll});
    root.push_back(Node{nav});
    return Node{std::make_shared<Column>(ColumnProps{.children = std::move(root)})};
}

/// @brief L5-C isolated scenario: LazyList self-driven scroll path (not via the outer Scroll buffer).
/// 2000 rows x single-Chip cards; virtualization only instantiates the visible window, so the whole
/// scroll runs on real content.
[[nodiscard]] auto build_lazy_tree() -> Node {
    constexpr int items = 2000;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("lazy item " + std::to_string(i));
        return Node{chip};
    };
    return Node{std::make_shared<LazyList>(items, builder, 48.0F)};
}

/// @brief L5-C scenario: GridView self-driven scroll path (root-level GridView, not via the outer
/// Scroll).
/// As a root-level scrollable control, GridView receives a window-limited constraint; virtualization
/// only instantiates the visible rows, so the whole scroll runs on real content.
/// Note: under a loose (infinite) constraint GridView collapses to a 320x480 fallback size, so it
/// must not be wrapped in a Scroll for measurement (that would strip it of scrollable content); the
/// root-level usage here matches the app scene's make_grid.
/// 2000 items x 4 columns x single-Chip cards.
[[nodiscard]] auto build_grid_tree() -> Node {
    constexpr int items = 2000;
    constexpr int cols = 4;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("grid " + std::to_string(i));
        return Node{chip};
    };
    return Node{std::make_shared<GridView>(items, cols, builder, 96.0F)};
}

/// @brief Emit a scene report (rendering form selected by format).
auto emit(const std::string &format, const ScrollBenchHarness::Result &r, bool &csv_header_written) -> void {
    if (format == "json") {
        AURORA_LOG_RAW("bench", r.to_json(), "\n");
    } else if (format == "csv") {
        if (!csv_header_written) {
            AURORA_LOG_RAW("bench", ScrollBenchHarness::Result::csv_header(), "\n");
            csv_header_written = true;
        }
        AURORA_LOG_RAW("bench", r.to_csv_row(), "\n");
    } else {
        AURORA_LOG_RAW("bench", r.to_markdown(), "\n");
    }
}

/// @brief Repeat sampling and take the best (the lowest p99).
///
/// **Why take min instead of average**: measured on 300 frames at the same config, `avg`/`p50`
/// stay within +/-2%, but p99 fluctuates +/-6~11% and `worst` can reach +/-65%. This noise is
/// **additive** -- OS preemption, page faults, and thermal throttling only make frames slower,
/// never faster. So the fastest sample across multiple runs is closest to "the true rendering cost
/// on this machine"; averaging writes the noise into the baseline and the threshold drifts with
/// machine load. The counters are deterministic, so any single run is equivalent.
///
/// **Known bias (must be aware)**: this function repeats **within the same process**. Measured in
/// the same process, the 1st p99 is ~11.8ms while the 2nd/3rd stabilize at ~18.7ms -- a 27MB-class
/// offscreen buffer being repeatedly allocated/freed causes heap fragmentation and page-fault
/// overhead unrelated to the rendering logic under test. The min statistic happens to select the 1st
/// run (the cleanest sample), so the conclusion direction is **conservative** (it only over-reports
/// cost, never under-reports). To get strictly comparable timings, call it multiple times in
/// **separate processes** and take the minimum:
/// `for i in 1 2 3; do bench_scroll --scene X --format csv; done` (separate-process sampling
/// protocol).
///
/// @param build    rebuild a brand-new tree on every rerun (avoid leftover cache/animation state
///                from the previous sample affecting the next)
/// @param viewport viewport size (logical dp), matching the target window resolution
/// @param cfg      scroll config: frames, warmup, delta, fling, etc.
/// @param repeat   repeat count; < 1 is treated as 1
[[nodiscard]] auto run_best_of(const std::function<Node()> &build, Size viewport, const ScrollBenchHarness::Config &cfg,
                               int repeat) -> std::pair<ScrollBenchHarness::Result, std::string> {
    const int n = repeat > 0 ? repeat : 1;
    ScrollBenchHarness::Result best;
    std::vector<double> p99s;
    p99s.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto r = ScrollBenchHarness::run(build(), viewport, cfg);
        p99s.push_back(r.p99_ms());
        if (i == 0 || r.p99_ms() < best.p99_ms()) {
            best = std::move(r);
        }
    }
    std::string note;
    if (n > 1) {
        auto sorted = p99s;
        std::ranges::sort(sorted);
        const double lo = sorted.front();
        const double hi = sorted.back();
        // 基准测量热路径：.at()
        // 的边界检查开销会影响计时
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const double mid = sorted[sorted.size() / 2];
        note = "> best-of-" + std::to_string(n) + ": p99 across runs " + std::to_string(lo) + " / " +
               std::to_string(mid) + " / " + std::to_string(hi) +
               " ms (min / median / max); the table below takes the lowest run.\n"
               "> Note: same-process repetition has a systematic bias (later runs are slower due to heap "
               "fragmentation); for strict sampling, call it multiple times in separate processes.\n\n";
    }
    return {std::move(best), std::move(note)};
}

/// @brief Command-line declaration table: parsing, derived `--help` and range checks all come
/// from this single schema (`aurora::cli`).
[[nodiscard]] auto build_spec() -> const aurora::cli::CommandSpec & {
    using aurora::cli::Arity;
    using aurora::cli::OptionSchema;
    using aurora::cli::ValueKind;

    static const aurora::cli::CommandSpec ROOT_SPEC = [] {
        aurora::cli::CommandSpec root;
        root.name = "bench_scroll";
        root.about = "Aurora scroll rendering benchmark (L5 harness, headless surface)";
        root.options = {
            OptionSchema{
                .long_name = "scene",
                .kind = ValueKind::Enum,
                .help = "Scene to sample; `all` runs every scene",
                .value_hint = "SCENE",
                .default_text = "all",
                .choices = {"app", "synthetic", "lazy", "grid", "all"},
            },
            OptionSchema{
                .long_name = "format",
                .kind = ValueKind::Enum,
                .help = "Report format",
                .value_hint = "FMT",
                .default_text = "md",
                .choices = {"md", "json", "csv"},
            },
            OptionSchema{
                .long_name = "frames",
                .kind = ValueKind::Int,
                .help = "Frames per sample",
                .default_text = "300",
                .minimum = 1,
            },
            OptionSchema{
                .long_name = "warmup",
                .kind = ValueKind::Int,
                .help = "Warmup frames discarded before sampling",
                .default_text = "30",
                .minimum = 0,
            },
            OptionSchema{
                .long_name = "delta",
                .kind = ValueKind::Double,
                .help = "Scroll distance per frame, in logical dp",
                .value_hint = "DP",
                .default_text = "12.0",
                .minimum = 0,
            },
            OptionSchema{
                .long_name = "scale",
                .kind = ValueKind::Double,
                .help = "Device pixel ratio",
                .default_text = "1.0",
                .minimum = 0.01,
            },
            OptionSchema{
                .long_name = "repeat",
                .kind = ValueKind::Int,
                .help = "In-process repetitions, take the lowest p99",
                .default_text = "1",
                .minimum = 1,
            },
            OptionSchema{
                .long_name = "fling",
                .kind = ValueKind::Bool,
                .arity = Arity::flag(),
                .help = "Drive the scroll by fling inertia instead of uniform stepping",
            },
        };
        root.epilog =
            R"(Scenes:
  app        app-shell-shaped workload: top bar + search + auto-advancing banner carousel +
             chip row + static grid of layer-cached cells + reco row + review cards + bottom
             nav (heterogeneous sections, text-dense cells, canvas icons, layer-cached cells,
             self-driving carousel; the page Scroll is the only scrollable in the tree)
  synthetic  control group: a stable 200-row x 6-Chip long list decoupled from any app shape
  lazy / grid are L5-C isolated scenes: they use LazyList / GridView directly as roots to
             measure their self-driven scroll paths (not via the outer Scroll offscreen buffer)
Sampling:
  --repeat N repeats inside one process and keeps the lowest p99, which is conservative (it can
             only over-report). Counter readings are deterministic, so one shot suffices; only
             timings need repetition. For strict sampling take separate processes instead:
               for i in 1 2 3; do bench_scroll --scene app --format csv; done
Defaults: --delta 12 dp per frame ~ 720 dp/s at 60fps, over 300 frames after 30 warmup frames.
Exit codes: 0 = report written, 2 = usage error or an untrustworthy reading (see the note above).)";
        return root;
    }();
    return ROOT_SPEC;
}

}  // namespace

auto main(int argc, char **argv) -> int {  // NOLINT(*-function-cognitive-complexity,bugprone-exception-escape)
    const auto parsed = aurora::cli::parse(build_spec(), argc, argv);
    if (!parsed) {
        std::string detail = parsed.error().message;
        if (!parsed.error().suggestion.empty()) {
            detail += " — " + parsed.error().suggestion;
        }
        AURORA_LOG_ERROR("bench", detail);
        return 2;
    }
    const aurora::cli::Invocation &invocation = parsed.value();
    if (invocation.shows_display()) {
        AURORA_LOG_RAW("bench", invocation.display_text);  // --help / --version come from the schema
        return 0;
    }

    // Every option declares a default_text, so each read below is guaranteed by the schema
    // (a value that fails its kind or range already returned as a usage error above).
    const aurora::cli::Arguments &cli_args = invocation.arguments;
    const std::string scene = cli_args.get<std::string>("scene").value();
    const std::string format = cli_args.get<std::string>("format").value();
    const int repeat = cli_args.get<int>("repeat").value();
    ScrollBenchHarness::Config cfg;
    cfg.frames = cli_args.get<int>("frames").value();
    cfg.warmup_frames = cli_args.get<int>("warmup").value();
    cfg.delta_per_frame = static_cast<float>(cli_args.get<double>("delta").value());
    cfg.scale = static_cast<float>(cli_args.get<double>("scale").value());
    cfg.fling = cli_args.flag("fling");

    constexpr Size viewport{.width = 1100.0F,
                            .height = 760.0F};  // fixed convention across all scenes
    bool csv_header_written = false;

    if (format == "md") {
        AURORA_LOG_RAW("bench", "## scroll bench\n\n- viewport: 1100x760 dp @scale ",
                       std::to_string(static_cast<double>(cfg.scale)), "\n- frames: ", std::to_string(cfg.frames),
                       " (warmup ", std::to_string(cfg.warmup_frames), ")\n- mode: ", cfg.fling ? "fling" : "uniform",
                       ", delta/frame: ", std::to_string(static_cast<double>(cfg.delta_per_frame)), " dp",
                       "\n- repeat: ", std::to_string(repeat), repeat > 1 ? " (lowest p99 run)" : "", "\n- profiling: ",
                       profiling_enabled() ? "ON (counters valid)" : "OFF (counters always 0, time only)", "\n\n");
    }

    bool any_untrustworthy = false;

    if (scene == "app" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "app-fling" : "app-uniform";
        auto [r, note] = run_best_of([width = viewport.width]() -> Node { return build_app_tree(width); },
                                     viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "synthetic" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "synthetic-fling" : "synthetic-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_synthetic_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "lazy" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "lazy-fling" : "lazy-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_lazy_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "grid" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "grid-fling" : "grid-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_grid_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (any_untrustworthy) {
        // Non-zero exit code: the baseline collection script uses this to refuse writing untrusted
        // readings into the comparison table.
        AURORA_LOG_ERROR("bench",
                         "At least one scenario reading is unreliable (scroll container not found / idle "
                         "frame drops); non-zero exit code set");
        return 2;
    }
    return 0;
}
