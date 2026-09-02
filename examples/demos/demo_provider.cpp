// Provider 控件 demo：ThemeProvider / LocaleProvider / MediaQueryProvider 三件套。
#include "demo_common.h"

auto main() -> int {
    auto light = au::Theme::light();
    auto zh = au::Locale{ .language = "zh", .region = "CN" };
    auto mq = au::MediaQuery::of(2.0);

    au::Node content = au::Column{
        GradientTitle{ "Provider trio" },
        gap(12),
        au::Text{ au::LocalizedString{ "ThemeProvider / LocaleProvider / MediaQueryProvider" } },
        BrandBadge{ "brand badge", pal::AURORA_PRIMARY },
        au::Text{ au::LocalizedString{ "scale = 2.0 (from MediaQuery)" } },
    };

    au::Node root =
        Card{ au::MediaQueryProvider{ mq, au::LocaleProvider{ zh, au::ThemeProvider{ light, std::move(content) } } } };
    return run_demo(std::move(root), "Provider · Aurora Demo", 520.0f, 420.0f);
}
