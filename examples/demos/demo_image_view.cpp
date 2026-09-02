// ImageView 控件 demo：Image::load（内置 BMP 解码，失败降级占位）。
#include "demo_common.h"

auto main() -> int {
    auto img_res = au::Image::load("test.bmp");
    au::Node hero = img_res
                        ? au::Node{ au::ImageView{ img_res.value() } }
                        : au::Node{ au::Text{ au::TextProps{ .content = au::LocalizedString{ "[image placeholder]" },
                                                             .text_color = pal::AURORA_MUTED } } };

    au::Node root = au::Column{
        GradientTitle{ "ImageView 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "内置 BMP 解码；加载失败则显示占位文本" } },
        std::move(hero),
    };
    return run_demo(Card{ std::move(root) }, "ImageView · Aurora Demo", 520.0f, 420.0f);
}
