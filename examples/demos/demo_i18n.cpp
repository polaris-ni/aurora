// 国际化 demo：Locale / LocaleProvider / LocalizedString::tr。
#include "demo_common.h"

auto main() -> int {
    const auto zh = au::Locale{ .language = "zh", .region = "CN" };
    const au::LocalizedString greeting = au::LocalizedString::tr("hello_user", { "Ada" });

    au::Node content = au::Column{
        GradientTitle{ "国际化 / i18n" },
        gap(12),
        au::Text{ au::LocalizedString{ "Locale: zh-CN" } },
        au::Text{ greeting },
        au::Text{ au::LocalizedString{ "LocalizedString 经 LocaleProvider 注入语言环境" } },
    };
    au::Node root = Card{ au::LocaleProvider{ zh, std::move(content) } };
    return run_demo(std::move(root), "i18n · Aurora Demo", 520.0f, 380.0f);
}
