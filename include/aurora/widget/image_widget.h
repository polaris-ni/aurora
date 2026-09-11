#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/image/image_codec.h"
#include "aurora/render/image_cache.h"
#include "aurora/render/painter.h"
#include "aurora/state/async.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 图片加载状态（D0 异步 URL 源的三态 + 初始占位）。
enum class ImageLoadState : std::uint8_t {
    Placeholder,  ///< 占位（初始 / 未注入 fetcher 的降级态）
    Loading,  ///< 加载中（fetcher 已启动、尚未回填）
    Loaded,  ///< 已加载（bitmap 就绪）
    Failed,  ///< 加载/解码失败（降级占位框）
};

/// @brief 图片获取器（D0，需求 #1 fetcher 方案）：URL → 字节流的异步任务工厂。
///
/// 库核心**不内置 HTTP**：App 提供实现（如 WinHTTP/curl/浏览器 fetch 包装），返回
/// `Task<std::vector<std::uint8_t>>`（内部经 `async` 跑线程池，`then` 回主线程投递器）。
using ImageFetcher = std::function<Task<std::vector<std::uint8_t>>(std::string_view url)>;

/// @brief 进程级默认 fetcher（读写口；`Environment` 注入 `ImageFetcher` 优先于此值）。
inline auto default_image_fetcher() -> ImageFetcher & {
    static ImageFetcher f;  // NOLINT(misc-use-anonymous-namespace) 单例读写口
    return f;
}

/// @brief 设置进程级默认 fetcher（App 启动时注入一次；传空 = 撤销，URL 源降级占位）。
inline auto set_default_image_fetcher(ImageFetcher f) -> void { default_image_fetcher() = std::move(f); }

/// @brief ImageView 属性（聚合）：位图图片。
struct ImageViewProps {
    Image bitmap{};  ///< 已解码图像（可空）
    std::optional<std::string> source = std::nullopt;  ///< 源文件路径（用于序列化/占位）
};

/**
 * @brief 图像 widget（specification/04-widget.md §3.6）。
 *
 * 持有已解码的 `Image`（来自 `Image::load`，支持 BMP 内置解码 + PNG/JPG 等 stb 解码），
 * 在布局阶段按自身或被约束尺寸确定绘制矩形，绘制阶段经 `Painter::drawImage` 栅格化。
 *
 * 命名为 `ImageView` 以区别于 `core::Image`（解码后的像素数据结构）。
 *
 * 采用**继承式双模 API**（specification/04-widget.md §2.5）：`ImageViewProps` 字段即本控件公有字段，
 * `bitmap`/`source` 可直接访问或以配置块构造
 * `au::ImageView{ au::ImageViewProps{ .bitmap = img, .source = "..." } }`。
 * 渲染宽高 `width()`/`height()` 等 widget 级属性沿用基类（不进 Props）。
 *
 * 用法：
 * @code
 *   auto img = Image::load("logo.png");            // Result<Image>
 *   au::ImageView(std::move(img.value()));         // 传入已解码图像
 *   au::ImageView::from_file("logo.png");          // 便捷：自动解码（失败返回占位）
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ImageView : public Widget, public ImageViewProps {
  public:
    ImageView() = default;
    explicit ImageView(Image bmp) : ImageViewProps{.bitmap = std::move(bmp)} {}

    ImageView(ImageViewProps props) : ImageViewProps(std::move(props)) {}

    /// @brief 便捷工厂：从文件解码（失败返回空图像，不抛异常）。
    [[nodiscard]] static auto from_file(std::string_view path) -> ImageView {
        auto r = Image::load(path);
        if (r) {
            return ImageView{std::move(r.value())};
        }
        return ImageView{Image{}};
    }

    /// @brief 异步加载 URL 源（D0）：占位 → fetcher 取字节 → 解码 → `ImageCache` 缓存 →
    ///        主线程回填 bitmap。**未注入 fetcher 时优雅降级**：停留在 `Placeholder`。
    ///
    /// fetcher 解析优先级：显式实参 > 进程级默认（`set_default_image_fetcher`）> `Environment`
    /// 注入（`on_mount` 时经 `ctx.environment<ImageFetcher>()` 补尝试）。
    /// 线程契约：回填回调线程由 `Task` 主线程投递器决定——`Application::run` 已接线
    /// （经 `drain_posted` 主线程执行）；无投递器（headless）时在 worker 内联执行。
    /// @param url    图片 URL（同时作为 `ImageCache` 缓存键）
    /// @param fetcher 显式 fetcher（可选；测试注入 mock 的入口）
    /// @return 持有加载中实例的 shared_ptr（回填经弱引用守卫，实例销毁后安全丢弃）
    [[nodiscard]] static auto from_url(std::string url, ImageFetcher fetcher = {})
        -> std::shared_ptr<ImageView> {
        auto w = std::make_shared<ImageView>();
        w->url_ = std::move(url);
        if (!fetcher) {
            fetcher = default_image_fetcher();
        }
        if (fetcher) {
            w->begin_load(std::move(fetcher));
        }
        return w;
    }

    /// @brief 当前加载状态（三态 + 占位；测试与上层 UI 判断用）。
    [[nodiscard]] auto load_state() const -> ImageLoadState { return load_state_; }

    /// @brief 当前 URL 源（区别于 `source` 文件路径；空 = 非异步源）。
    [[nodiscard]] auto url() const -> const std::string & { return url_; }

    [[nodiscard]] auto type_name() const -> const char * override { return "Image"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Image",
            .properties =
                {
                    {.name = "source",
                     .type = "string",
                     .default_value = "nullopt",
                     .required = false,
                     .note = "源文件路径",
                     .json_type = "string"},
                    {.name = "image_width",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "图像宽度(px)",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "image_height",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "图像高度(px)",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "none",
            .examples = {"au::ImageView::from_file(\"logo.png\")"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        if (source.has_value()) {
            props["source"] = *source;
        }
        props["image_width"] = bitmap.width;
        props["image_height"] = bitmap.height;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("source") && props["source"].is_string()) {
            source = props["source"].get<std::string>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float natural_w = bitmap.width > 0 ? static_cast<float>(bitmap.width) : 100.0F;
        const float natural_h = bitmap.height > 0 ? static_cast<float>(bitmap.height) : 100.0F;
        const float w = resolve_width(c, natural_w);
        const float h = resolve_height(c, natural_h);
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        if (!bitmap.pixels.empty()) {
            p.draw_image(bitmap, bounds);
        } else {
            p.draw_rect(bounds, Color{180, 180, 180, 255});  // 占位框
        }
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        // Environment 注入的 fetcher（D0）：构造时无显式/进程默认 fetcher 且尚未开始加载，
        // 挂载后经环境链补取注入值再启动；仍未注入则保持 Placeholder 降级。
        if (!url_.empty() && !loading_ && load_state_ == ImageLoadState::Placeholder) {
            if (const auto *injected = ctx.environment<ImageFetcher>()) {
                begin_load(*injected);
            }
        }
    }

  private:
    auto resolve_width(const Constraints &c, float natural) const -> float {
        if (width_.kind == LengthKind::Fixed) {
            return std::max(c.min.width, std::min(width_.value, c.max.width));
        }
        return std::max(c.min.width, std::min(natural, c.max.width));
    }
    auto resolve_height(const Constraints &c, float natural) const -> float {
        if (height_.kind == LengthKind::Fixed) {
            return std::max(c.min.height, std::min(height_.value, c.max.height));
        }
        return std::max(c.min.height, std::min(natural, c.max.height));
    }

    /// @brief 启动异步加载：缓存命中直读；否则 fetcher（worker）→ 解码 → 回填（主线程）。
    auto begin_load(ImageFetcher fetcher) -> void {
        if (url_.empty() || loading_) {
            return;
        }
        ImageCache &cache = ImageCache::instance();
        if (cache.contains(url_)) {
            auto cached = cache.get(url_);
            if (cached) {
                bitmap = std::move(cached.value());
                load_state_ = ImageLoadState::Loaded;
                return;
            }
        }
        loading_ = true;
        load_state_ = ImageLoadState::Loading;
        // 弱引用守卫：实例可能先于任务完成被销毁；回调线程见 Task 主线程投递器契约。
        // Widget 基类持有 enable_shared_from_this<Widget>，锁回后安全下转回 ImageView。
        std::weak_ptr<Widget> guard = weak_from_this();
        fetcher(url_).then([guard, url = url_](const Result<std::vector<std::uint8_t>> &r) -> void {
            auto base = guard.lock();
            if (base == nullptr) {
                return;  // 控件已销毁：丢弃结果
            }
            auto self = std::static_pointer_cast<ImageView>(base);  // NOLINT 笃定自身类型（唯一守卫来源）
            self->loading_ = false;
            if (!r.ok()) {
                self->load_state_ = ImageLoadState::Failed;
                self->mark_needs_paint();
                return;
            }
            auto img = image::ImageCodecRegistry::instance().decode_memory(r.value());
            if (!img.ok()) {
                self->load_state_ = ImageLoadState::Failed;
                self->mark_needs_paint();
                return;
            }
            ImageCache::instance().put(url, std::move(img.value()));
            auto cached = ImageCache::instance().get(url);
            if (cached) {
                self->bitmap = std::move(cached.value());
            }
            self->load_state_ = ImageLoadState::Loaded;
            self->mark_needs_paint();
        });
    }

    std::string url_;  ///< URL 源（异步加载；区别于 source 文件路径）
    ImageLoadState load_state_ = ImageLoadState::Placeholder;
    bool loading_ = false;  ///< 防重入：同一实例同一时刻至多一个在途任务
};

}  // namespace aurora
