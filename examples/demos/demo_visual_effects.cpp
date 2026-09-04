// 视觉修饰 demo：blend_mode（像素混合）/ shader_mask（渐变淡出）/ cache_layer（离屏缓存）。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Text blend{au::LocalizedString{"blend_mode: Multiply + blue tint"}};
    blend.modifier.set(au::Modifier{}
                           .padding(12.0F)
                           .background(au::Color{255, 255, 255})
                           .blend_mode(au::BlendMode::Multiply, au::Color::blue()));

    au::Text fade{au::LocalizedString{"shader_mask: LinearFade top bright -> bottom fade"}};
    fade.modifier.set(au::Modifier{}
                          .size(300.0F, 56.0F)
                          .background(pal::AURORA_ACCENT)
                          .shader_mask(au::ShaderMaskKind::LinearFade, 1.0F));

    au::Text cached{au::LocalizedString{"cache_layer: subtree offscreen cache (reused while size unchanged)"}};
    cached.modifier.set(au::Modifier{}
                            .padding(12.0F)
                            .background(pal::AURORA_PRIMARY_SOFT)
                            .border(1.0F, pal::AURORA_BORDER)
                            .cache_layer());

    au::Node root = au::Column{
        GradientTitle{"Visual Effects"},
        gap(10),
        std::move(blend),
        gap(10),
        std::move(fade),
        gap(10),
        std::move(cached),
    };
    return run_demo(Card{std::move(root)}, "Visual Effects · Aurora Demo", 560.0F, 420.0F);
}