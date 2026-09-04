// ImageView 控件 demo：Image::load（内置 BMP 解码，失败降级占位）。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto img_res = au::Image::load("test.bmp");
    au::Node hero = img_res ? au::Node{au::ImageView{img_res.value()}}
                            : au::Node{au::Text{au::TextProps{.content = au::LocalizedString{"[image placeholder]"},
                                                              .text_color = pal::AURORA_MUTED}}};

    au::Node root = au::Column{
        GradientTitle{"ImageView widget"},
        gap(12),
        au::Text{au::LocalizedString{"Built-in BMP decoder; shows placeholder text on load failure"}},
        std::move(hero),
    };
    return run_demo(Card{std::move(root)}, "ImageView · Aurora Demo", 520.0F, 420.0F);
}