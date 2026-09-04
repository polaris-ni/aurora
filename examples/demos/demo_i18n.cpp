// 国际化 demo：Locale / LocaleProvider / LocalizedString::tr。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const auto zh = au::Locale{.language = "zh", .region = "CN"};
    const au::LocalizedString greeting = au::LocalizedString::tr("hello_user", {"Ada"});

    au::Node content = au::Column{
        GradientTitle{"Internationalization / i18n"},
        gap(12),
        au::Text{au::LocalizedString{"Locale: zh-CN"}},
        au::Text{greeting},
        au::Text{au::LocalizedString{"LocalizedString injects locale via LocaleProvider"}},
    };
    au::Node root = Card{au::LocaleProvider{zh, std::move(content)}};
    return run_demo(std::move(root), "i18n · Aurora Demo", 520.0F, 380.0F);
}