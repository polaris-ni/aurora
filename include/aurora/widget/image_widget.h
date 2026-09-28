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

/// @brief 图片加载状态（异步 URL 源的三态 + 初始占位）。
enum class ImageLoadState : std::uint8_t {
    Placeholder,  ///< 占位（初始 / 未注入 fetcher 的降级态）
    Loading,  ///< 加载中（fetcher 已启动、尚未回填）
    Loaded,  ///< 已加载（bitmap 就绪）
    Failed,  ///< 加载/解码失败（降级占位框）
};

/// @brief 图片获取器（需求 #1 fetcher 方案）：URL → 字节流的异步任务工厂。
///
/// 库核心**不内置 HTTP**：App 提供实现（如 WinHTTP/curl/浏览器 fetch 包装），返回
/// `Task<std::vector<std::uint8_t>>`（内部经 `async` 跑线程池，`then` 回主线程投递器）。
using ImageFetcher = std::function<Task<std::vector<std::uint8_t>>(std::string_view url)>;

/// @brief 进程级默认 fetcher（读写口；`Environment` 注入 `ImageFetcher` 优先于此值）。
/// @return 单例 `std::function` 的引用；未注入时为空（URL 源停留在占位态）。
inline auto default_image_fetcher() -> ImageFetcher & {
    // 惰性构造的函数内 static：单例读写口，首建时刻与跨 TU 静态初始化顺序无关（本检查的担心面）。
    // 仅浏览器口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static ImageFetcher f;  // NOLINT(misc-use-anonymous-namespace) 单例读写口
    return f;
}

/// @brief 设置进程级默认 fetcher（App 启动时注入一次；传空 = 撤销，URL 源降级占位）。
/// @param f 新的默认 fetcher（URL → 字节流 `Task`），移动进单例；空 `std::function` 表示撤销注入。
inline auto set_default_image_fetcher(ImageFetcher f) -> void { default_image_fetcher() = std::move(f); }

/// @brief ImageView 属性（聚合）：位图图片。
struct ImageViewProps {
    Image bitmap{};  ///< 已解码图像（可空）
    std::optional<std::string> source = std::nullopt;  ///< 源文件路径（用于序列化/占位）
};

/// @brief 图像 widget（specification/04-widget.md §3.6）。
///
/// 持有已解码的 `Image`（来自 `Image::load`，支持 BMP 内置解码 + PNG/JPG 等 stb 解码），
/// 在布局阶段按自身或被约束尺寸确定绘制矩形，绘制阶段经 `Painter::drawImage` 栅格化。
///
/// 命名为 `ImageView` 以区别于 `core::Image`（解码后的像素数据结构）。
///
/// 采用**继承式双模 API**（specification/04-widget.md §2.5）：`ImageViewProps` 字段即本控件公有字段，
/// `bitmap`/`source` 可直接访问或以配置块构造
/// `au::ImageView{ au::ImageViewProps{ .bitmap = img, .source = "..." } }`。
/// 渲染宽高 `width()`/`height()` 等 widget 级属性沿用基类（不进 Props）。
///
/// 用法：
/// @code
/// auto img = Image::load("logo.png");            // Result<Image>
/// au::ImageView(std::move(img.value()));         // 传入已解码图像
/// au::ImageView::from_file("logo.png");          // 便捷：自动解码（失败返回占位）
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class ImageView : public Widget, public ImageViewProps {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 转发 `has_input_semantics()`：挂了输入语义为 true，纯展示为 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    ImageView() = default;
    /// @brief 以已解码位图构造（其余字段取默认）。
    /// @param bmp 待显示的图像，移动进 `bitmap`；空图像时绘制占位框。
    explicit ImageView(Image bmp) : ImageViewProps{.bitmap = std::move(bmp)} {}

    /// @brief 配置块构造：整份属性直接作为本控件字段（继承式双模 API）。
    /// @param props 图像属性（`bitmap` / `source`），移动进基类子对象。
    ImageView(ImageViewProps props) : ImageViewProps(std::move(props)) {}

    /// @brief 便捷工厂：从文件解码（失败返回空图像，不抛异常）。
    /// @param path 图片文件路径，交给 `Image::load` 按扩展名选解码器。
    /// @return 携带解码结果的实例；解码失败时 `bitmap` 为空（渲染占位框）。
    [[nodiscard]] static auto from_file(std::string_view path) -> ImageView {
        auto r = Image::load(path);
        if (r) {
            return ImageView{std::move(r.value())};
        }
        return ImageView{Image{}};
    }

    /// @brief 异步加载 URL 源：占位 → fetcher 取字节 → 解码 → `ImageCache` 缓存 →
    ///        主线程回填 bitmap。**未注入 fetcher 时优雅降级**：停留在 `Placeholder`。
    ///
    /// fetcher 解析优先级：显式实参 > 进程级默认（`set_default_image_fetcher`）> `Environment`
    /// 注入（`on_mount` 时经 `ctx.environment<ImageFetcher>()` 补尝试）。
    /// 线程契约：回填回调线程由 `Task` 主线程投递器决定——`Application::run` 已接线
    /// （经 `drain_posted` 主线程执行）；无投递器（headless）时在 worker 内联执行。
    /// @param url    图片 URL（同时作为 `ImageCache` 缓存键）
    /// @param fetcher 显式 fetcher（可选；测试注入 mock 的入口）
    /// @return 持有加载中实例的 shared_ptr（回填经弱引用守卫，实例销毁后安全丢弃）
    [[nodiscard]] static auto from_url(std::string url, ImageFetcher fetcher = {}) -> std::shared_ptr<ImageView> {
        auto w = std::make_shared<ImageView>();
        w->url_ = std::move(url);
        if (!fetcher) {
            fetcher = default_image_fetcher();
        }
        if (fetcher) {
            w->begin_load(fetcher);  // 形参 const 引用：此处无需转移，调用即完成取用
        }
        return w;
    }

    /// @brief 当前加载状态（三态 + 占位；测试与上层 UI 判断用）。
    /// @return 内部状态副本：初始 `Placeholder`，在途 `Loading`，成功 `Loaded`，失败 `Failed`。
    [[nodiscard]] auto load_state() const -> ImageLoadState { return load_state_; }

    /// @brief 当前 URL 源（区别于 `source` 文件路径；空 = 非异步源）。
    /// @return URL 的常量引用；未经 `from_url` 构造时为空串。
    [[nodiscard]] auto url() const -> const std::string & { return url_; }

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Image"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Image"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "Image" 的描述表：含 source/image_width/image_height 与 widget 级 width/height/show，
    ///         子节点策略 "none"。
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
    /// @brief 实例级自描述：图像视图无实例差异，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：`ImageViewProps` 皆为普通值字段，无响应式属性可登记。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 序列化自有属性到 `props`（先由基类写通用属性）。
    /// @param props 输出目标 JSON 对象：写入 `source`（仅当有值）与取自 `bitmap` 的
    ///        `image_width` / `image_height`。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        if (source.has_value()) {
            props["source"] = *source;
        }
        props["image_width"] = bitmap.width;
        props["image_height"] = bitmap.height;
    }

    /// @brief 从 JSON 重建自有属性（键缺失或非字符串则保留现值）。
    /// @param props 序列化属性对象，本类只读 `source` 键。
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
        // Environment 注入的 fetcher：构造时无显式/进程默认 fetcher 且尚未开始加载，
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
    /// @note 形参按 const 引用：本函数只调用 `fetcher(url_)`（`operator()` 为 const），
    ///       而注入路径（`on_mount` 里的 `begin_load(*injected)`）每次都拷一遍 std::function。
    auto begin_load(const ImageFetcher &fetcher) -> void {
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
