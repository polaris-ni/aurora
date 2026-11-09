/// 测试类型: unit
/// 目标单元: include/aurora/widget/image_widget.h
/// 测试说明: 覆盖 ImageView——默认不变量与自描述、无图占位自然尺寸、位图自然尺寸与约束钳制、
/// Fixed 宽高意图严格等值覆盖（显式盒语义，不受 max 约束钳制）、无图占位描边框/有图栅格化（内存位图，软件 Painter
/// 像素断言）、 source 序列化往返与非字符串宽容读、from_file 失败回退占位、
/// 异步 URL 源三态（占位→加载→成功/失败）与缓存命中、未注入 fetcher 优雅降级。属性单一声明迁移后
/// 另覆盖权威表的读消费面：撤表态该键退出序列化、常量档读件在明 / 暗两档与库内解析全同且不存在
/// 带上下文的第二件、字符串行的宽容反序列化口径与派生键惰性、挂载后写入只标该行声明的脏档
/// （source 零档、表外位图槽布局档）

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/main_queue.h"
#include "aurora/environment/environment.h"
#include "aurora/image/image_codec.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/state/async.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/image_widget.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_image_widget {

using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 构造 w×h 的内存图像（RGBA8，像素清零，不依赖文件系统）。
auto make_image(int w, int h) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U);
    return img;
}

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct ImageDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(ImageView &target) -> void {
        paints = 0;
        layouts = 0;
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

/// 「带上下文的源路径读件」形态：常量档行不应存在这第二件。
/// 用概念而非裸 `requires` 式判定：GCC 在非模板上下文把非法简单要求判成硬错误而非 false。
template <typename W>
concept ScopedSourceReader = requires(const W &probe, const BuildContext &scope) { probe.source(scope); };

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class ImageViewProbe final : public ImageView {
  public:
    using ImageView::ImageView;
    using ImageView::present;
    using ImageView::resolve;
};

}  // namespace

AURORA_TEST_CASE(default_image_view_invariants) {
    ImageViewProbe iv;
    AURORA_TEST_CHECK_EQ(std::string{iv.type_name()}, "Image");
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
    AURORA_TEST_CHECK_TRUE(iv.bitmap.pixels.empty());
    AURORA_TEST_CHECK_TRUE(iv.source().empty());
    AURORA_TEST_CHECK_FALSE(iv.present(ImageViewProps::source_row()));

    // `defaults()` 与默认构造同源：源未表态，生效值即该行回落点。
    const ImageViewProps fresh = ImageView::defaults();
    AURORA_TEST_CHECK_TRUE(fresh.source().empty());

    // 描述符 = 表行（source）＋两枚派生只读键（image_width / image_height），序不变。
    const auto d = ImageView::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Image");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 3U);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].name}, "source");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].default_value}, "nullopt");
    AURORA_TEST_CHECK_FALSE(d.properties[0].required);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].name}, "image_width");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].default_value}, "0");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].json_type}, "integer");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].min_value}, "0");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[2].name}, "image_height");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[2].default_value}, "0");
    AURORA_TEST_CHECK_EQ(std::string{iv.describe().name}, "Image");
}

AURORA_TEST_CASE(empty_bitmap_uses_placeholder_natural_size) {
    // 无图占位自然尺寸 100×100。
    ImageView iv;
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 100.0F, 1e-4F);

    // 占位自然尺寸同样受父约束钳制。
    ImageView squeezed;
    LayoutEngine::layout(squeezed, bounded(50.0F, 500.0F));
    AURORA_TEST_CHECK_NEAR(squeezed.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(squeezed.size().height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(bitmap_natural_size_and_constraint_clamp) {
    ImageView small{make_image(32, 16)};
    LayoutEngine::layout(small, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(small.size().width, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(small.size().height, 16.0F, 1e-4F);

    // 位图大于约束时钳到约束上限。
    ImageView huge{make_image(500, 400)};
    LayoutEngine::layout(huge, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(huge.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(huge.size().height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(fixed_length_strict_override) {
    ImageView iv{make_image(32, 16)};
    iv.width(Length::fixed(200.0F));
    iv.height(Length::fixed(50.0F));
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 50.0F, 1e-4F);

    // Fixed 显式盒语义（widget.cpp）：显式轴最终尺寸严格等于设定值，不受 max 约束钳制
    // （约束先被夹成 [v,v]，布局后 size 再被覆写为 v；ImageView::on_layout 内的钳制不构成最终尺寸）。
    ImageView strict{make_image(32, 16)};
    strict.width(Length::fixed(1000.0F));
    LayoutEngine::layout(strict, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(strict.size().width, 1000.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(strict.size().height, 16.0F, 1e-4F);
}

AURORA_TEST_CASE(paint_empty_bitmap_draws_placeholder_rect) {
    ImageView iv;
    Painter p;
    p.begin(64, 64);
    const Rect vp{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 64.0F}};
    const BuildContext ctx;
    iv.paint(p, vp, ctx);
    AURORA_TEST_CHECK_TRUE(iv.paint_bounds() == vp);
    // 占位为 `roles.outline` 的 1px 描边框（Painter::draw_rect = 四条 1px fill_rect 边带），非实心填充：
    // 边框像素等于兜底主题（无注入作用域即 `Theme::light()`）的描边色，框内保持 begin 后的全零底色。
    const Color border = inherit_theme(ctx).roles.outline;
    AURORA_TEST_CHECK_TRUE(p.get_pixel(32, 0) == border);
    AURORA_TEST_CHECK_TRUE(p.get_pixel(0, 32) == border);

    const Color center = p.get_pixel(32, 32);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.r), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.g), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.b), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(center.a), 0);
}

AURORA_TEST_CASE(paint_decoded_bitmap_rasterizes_pixels) {
    auto img = make_image(2, 2);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4U) {
        img.pixels[i] = 255U;  // R
        img.pixels[i + 3U] = 255U;  // A
    }
    ImageView iv{std::move(img)};
    Painter p;
    p.begin(8, 8);
    const Rect vp{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}};
    iv.paint(p, vp, BuildContext{});
    // 位图缩放铺满目标矩形，四角均为红色。
    AURORA_TEST_CHECK_EQ(p.get_pixel(0, 0), Color{255, 0, 0, 255});
    AURORA_TEST_CHECK_EQ(p.get_pixel(7, 7), Color{255, 0, 0, 255});
}

AURORA_TEST_CASE(source_serialization_roundtrip) {
    ImageView src{make_image(32, 16)};
    src.set_source("logo.png");
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "source"), "logo.png");
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "image_width"), 32);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "image_height"), 16);
    AURORA_TEST_CHECK_FALSE(props.contains("bitmap"));  // 运行期载荷不是属性键

    ImageViewProbe dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.present(ImageViewProps::source_row()));
    AURORA_TEST_CHECK_EQ(dst.source(), "logo.png");
    // 派生只读键不参与重建：位图不回填，两枚尺寸键只是解码结果的现值。
    AURORA_TEST_CHECK_EQ(dst.bitmap.width, 0);
    AURORA_TEST_CHECK_EQ(dst.bitmap.height, 0);

    // 未表态 source 时不落盘该键；派生键照写（它们恒可现算）。
    Json bare = Json::object();
    ImageView{make_image(3, 2)}.serialize_props(bare);
    AURORA_TEST_CHECK_FALSE(bare.contains("source"));
    AURORA_TEST_CHECK_EQ(require_field<int>(bare, "image_width"), 3);
    AURORA_TEST_CHECK_EQ(require_field<int>(bare, "image_height"), 2);
}

AURORA_TEST_CASE(from_file_missing_path_falls_back_to_empty) {
    ImageView iv = ImageView::from_file("./aurora_utest_no_such_dir/missing.png");
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
    AURORA_TEST_CHECK_TRUE(iv.bitmap.pixels.empty());
    // 解码失败回退空图像：布局走占位自然尺寸。
    LayoutEngine::layout(iv, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(iv.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(iv.size().height, 100.0F, 1e-4F);
}

// ===========================================================================
// 权威表的读消费面（属性单一声明迁移批）：撤表态 / 两档恒等 / 非法值降级 /
// 挂载后逐档标脏
// ===========================================================================

AURORA_TEST_CASE(image_props_clear_unstates_source_and_drops_key) {
    ImageViewProbe iv{make_image(4, 4)};
    iv.set_source("logo.png");
    Json stated = Json::object();
    iv.serialize_props(stated);
    AURORA_TEST_CHECK_TRUE(stated.contains("source"));
    AURORA_TEST_CHECK_FALSE(stated.contains("bitmap"));  // 表外名字（运行期载荷）不是属性键

    iv.clear(ImageViewProps::source_row());
    AURORA_TEST_CHECK_FALSE(iv.present(ImageViewProps::source_row()));
    AURORA_TEST_CHECK_TRUE(iv.source().empty());  // 回落基线
    Json cleared = Json::object();
    iv.serialize_props(cleared);
    AURORA_TEST_CHECK_FALSE(cleared.contains("source"));
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 4);  // 撤表态不动位图所有权
    AURORA_TEST_CHECK_EQ(require_field<int>(cleared, "image_width"), 4);  // 派生键照写
}

AURORA_TEST_CASE(image_props_constant_row_reads_identically_under_light_and_dark) {
    // 恒等关系的正面取证：本行是常量档，没有 `x(ctx)` 第二件，其无参读件**就是**生效值——
    // 明 / 暗两档主题下与库内 `resolve(row, ctx)` 取值全同。
    static_assert(!ScopedSourceReader<ImageView>);

    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    ImageViewProbe iv;
    AURORA_TEST_CHECK_TRUE(iv.source() == iv.resolve(ImageViewProps::source_row(), light));
    AURORA_TEST_CHECK_TRUE(iv.source() == iv.resolve(ImageViewProps::source_row(), dark));

    // 控件面同名写件返回控件引用，链式表的是「同一个对象」而非第二份属性。
    ImageView &chain = iv.set_source("logo.png");
    AURORA_TEST_CHECK(&chain == static_cast<ImageView *>(&iv));
    AURORA_TEST_CHECK_EQ(iv.source(), "logo.png");
    AURORA_TEST_CHECK_TRUE(iv.source() == iv.resolve(ImageViewProps::source_row(), light));
    AURORA_TEST_CHECK_TRUE(iv.source() == iv.resolve(ImageViewProps::source_row(), dark));

    // 未使用的 holder 显式丢弃，避免 -Wunused 噪声（作用域仍由下面两次解析覆盖）。
    (void)env_l;
    (void)env_d;
}

AURORA_TEST_CASE(image_props_string_row_is_lenient_and_derived_keys_are_inert) {
    // 字符串行没有登记 `validate`：非字符串按既有宽容口径退空串、仍表态、不产生降级上报
    // （收紧它属于与本口径无关的行为变更）。派生只读键不进表：给它们喂值既不触达位图，也不产生上报。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级
    const std::size_t baseline = Diagnostics::count();

    ImageViewProbe iv;
    iv.set_source("logo.png");
    Json not_string = Json::object();
    not_string.set("source", Json{42});  // 形态不符：喂数
    iv.deserialize_props(not_string);
    AURORA_TEST_CHECK_TRUE(iv.present(ImageViewProps::source_row()));
    AURORA_TEST_CHECK_TRUE(iv.source().empty());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline);

    // 派生只读键不进表：给它们喂值既不触达位图，也不产生降级上报。
    Json derived = Json::object();
    derived.set("image_width", Json{99});
    derived.set("image_height", Json{77});
    iv.deserialize_props(derived);
    AURORA_TEST_CHECK_EQ(iv.bitmap.width, 0);
    AURORA_TEST_CHECK_EQ(iv.bitmap.height, 0);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline);

    // 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("source", Json{"ok.png"});
    iv.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(iv.present(ImageViewProps::source_row()));
    AURORA_TEST_CHECK_EQ(iv.source(), "ok.png");
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline);  // 全程无降级上报

    Diagnostics::take();
}

AURORA_TEST_CASE(image_mounted_writers_mark_expected_tiers) {
    // source 行只服务序列化，不声明任何脏档：挂载后写入零落账。
    // 位图是表外运行期载荷，控件面写件自己声明布局档（连带 Display List 失效）。
    ImageView iv;
    const BuildContext ctx;
    iv.mount(ctx);
    ImageDirty dirty;
    dirty.attach_to(iv);

    iv.set_source("logo.png");
    AURORA_TEST_CHECK_EQ(dirty.layouts, 0);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    iv.set_source("logo.png");  // 同值：判等短路，不落账
    AURORA_TEST_CHECK_EQ(dirty.layouts, 0);

    iv.set_bitmap(make_image(2, 2));
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
}

/// @brief 测试基础设施：领一枚空唤醒的值班租约，回投滞留进程级唯一队列，
/// 由用例线程手动排水（与 Application::run 的 drain_posted 同构），使控件回填在测试线程确定性执行。
class PosterGuard {
  public:
    PosterGuard() : duty_(MainQueue::on_duty([]() -> void {})) {}
    ~PosterGuard() {
        if (duty_.has_value()) {
            duty_->release();
        }
    }
    PosterGuard(const PosterGuard &) = delete;
    auto operator=(const PosterGuard &) -> PosterGuard & = delete;
    PosterGuard(PosterGuard &&) = delete;
    auto operator=(PosterGuard &&) -> PosterGuard & = delete;
    /// @brief 排空全局队列（执行期间新入队的项在后续轮次继续执行，直到队列空）。
    auto drain() -> void {
        for (;;) {
            auto batch = MainQueue::take_batch();
            if (batch.empty()) {
                return;
            }
            for (std::function<void()> &fn : batch) {
                if (fn) {
                    fn();
                }
            }
        }
    }

  private:
    std::optional<MainQueue::Lease> duty_;  ///< 在岗租约（空 wake）：发起过闸、回投入全局队列待排空
};

/// @brief 排水直至控件进入期望状态（worker 写 result → 投递入队 → 排水执行 存在微秒级窗口，
///        轮询消除竞态；上限 ~5s 防挂死）。
auto drain_until(PosterGuard &poster, const ImageView &w, ImageLoadState expect) -> bool {
    for (int i = 0; i < 5000; ++i) {
        poster.drain();
        if (w.load_state() == expect) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    poster.drain();
    return w.load_state() == expect;
}

AURORA_TEST_CASE(from_url_loads_bytes_and_caches) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterGuard poster;
    ImageCache::instance().remove("mock://ok");

    // mock fetcher：worker 内返回一张经编码器产出的合法 PNG 字节（成功态）。
    auto png_bytes = image::ImageCodecRegistry::instance().encode(make_image(4, 3), image::EncodeOptions{});
    AURORA_TEST_REQUIRE_TRUE(png_bytes.ok());
    const auto &payload = png_bytes.value();

    auto iv = ImageView::from_url("mock://ok", [&](std::string_view) -> Task<std::vector<std::uint8_t>> {
        return async([&payload]() -> std::vector<std::uint8_t> { return payload; });
    });
    // 占位 → 加载中（fetcher 已启动；回填须待排水）
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Loading));

    // 主线程排水：字节 → 解码 → 缓存 → 回填 bitmap（drain_until 消除投递窗口竞态）
    AURORA_TEST_REQUIRE_TRUE(drain_until(poster, *iv, ImageLoadState::Loaded));
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Loaded));
    AURORA_TEST_CHECK_EQ(iv->bitmap.width, 4);
    AURORA_TEST_CHECK_EQ(iv->bitmap.height, 3);

    // 缓存命中：同一 URL 的第二个实例不经 fetcher、构造即 Loaded（同步）。
    int fetch_calls = 0;
    auto cached = ImageView::from_url("mock://ok", [&](std::string_view) -> Task<std::vector<std::uint8_t>> {
        ++fetch_calls;
        return async([]() -> std::vector<std::uint8_t> { return {}; });
    });
    AURORA_TEST_CHECK_EQ(static_cast<int>(cached->load_state()), static_cast<int>(ImageLoadState::Loaded));
    AURORA_TEST_CHECK_EQ(cached->bitmap.width, 4);
    AURORA_TEST_CHECK_EQ(fetch_calls, 0);
    poster.drain();
    AURORA_TEST_CHECK_EQ(fetch_calls, 0);  // 缓存命中：fetcher 始终未被调用
}

AURORA_TEST_CASE(from_url_fetch_error_degrades_to_failed) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterGuard poster;

    auto iv = ImageView::from_url("mock://err", [&](std::string_view) -> Task<std::vector<std::uint8_t>> {
        return async([]() -> Result<std::vector<std::uint8_t>> {
            return make_error(ErrorCode::IOFileNotFound, "mock fetch unavailable");
        });
    });
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Loading));
    AURORA_TEST_REQUIRE_TRUE(drain_until(poster, *iv, ImageLoadState::Failed));
    // 失败态：降级占位（bitmap 仍空 → 绘制走占位框路径）
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Failed));
    AURORA_TEST_CHECK_TRUE(iv->bitmap.pixels.empty());
}

AURORA_TEST_CASE(from_url_undecodable_bytes_degrade_to_failed) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterGuard poster;

    auto iv = ImageView::from_url("mock://garbage", [&](std::string_view) -> Task<std::vector<std::uint8_t>> {
        return async([]() -> std::vector<std::uint8_t> { return {1, 2, 3, 4}; });
    });
    AURORA_TEST_REQUIRE_TRUE(drain_until(poster, *iv, ImageLoadState::Failed));
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Failed));
    AURORA_TEST_CHECK_TRUE(iv->bitmap.pixels.empty());
}

AURORA_TEST_CASE(from_url_without_fetcher_stays_placeholder) {
    // 未注入 fetcher（显式空 + 进程默认空 + 无 Environment 注入）：优雅降级，停留占位。
    auto saved = default_image_fetcher();
    set_default_image_fetcher(nullptr);

    auto iv = ImageView::from_url("mock://none");
    AURORA_TEST_CHECK_EQ(static_cast<int>(iv->load_state()), static_cast<int>(ImageLoadState::Placeholder));

    set_default_image_fetcher(std::move(saved));  // 还原进程级默认
}

}  // namespace aurora::test_cases::utest_image_widget
