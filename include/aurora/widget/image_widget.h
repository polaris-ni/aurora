#pragma once

#include <optional>
#include <string>
#include <utility>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

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
};

}  // namespace aurora
