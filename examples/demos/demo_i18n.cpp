// 国际化 demo：构建期外部词表（gen_i18n）→ 运行期 MessageCatalog 载入 → 运行期切 locale。
//
// 词条一律来自 examples/demos/i18n/*.json（本文件不含任何译文字面量），经
// `aurora_i18n_generate` 生成强类型构造函数 `demo_i18n::msg::demo::*` 与 AUM1 二进制词表；
// 切换由 `LocaleProvider` 注入 `State<Locale>` 完成（窗口级默认见 `Window::set_locale`，
// 子树 Provider 优先级更高）。文本随 locale 即时重解析，帧内不混排。
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "demo_common.h"
#include "messages_en.hpp"  // 生成件（勿手改）：Id 常量与具名形参构造函数

namespace {

// 两条 locale 常量在静态初始化期构造 3 枚短字符串，唯一抛出路径是 bad_alloc；demo 进程对
// 内存耗尽没有恢复语义（既有同类件按同口径豁免，见 src/aurora/render/font_discovery.cpp）。
// NOLINTBEGIN(bugprone-throwing-static-initialization)
const au::Locale AURORA_LOCALE_EN{.language = "en"};
const au::Locale AURORA_LOCALE_ZH{.language = "zh", .script = "Hans", .region = "CN"};
// NOLINTEND(bugprone-throwing-static-initialization)

/// @brief 整读生成出的 AUM1 词表。
///
/// 首选构建期注入的绝对路径（`aurora_i18n_generate` 随 bin 落 `$<TARGET_FILE_DIR>` 一并给出），
/// 退而求其次取当前目录同名文件——bin 不属任何源表，交付形态下由应用侧自行安置。
/// @return 词表字节；读不到时为空（调用方据此走未本地化回退）。
auto read_vocabulary() -> std::vector<std::byte> {
    std::vector<std::filesystem::path> candidates;
#ifdef AURORA_I18N_BIN_PATH
    candidates.emplace_back(AURORA_I18N_BIN_PATH);
#endif
    candidates.emplace_back("messages_en.bin");

    std::error_code ec;
    for (const std::filesystem::path &candidate : candidates) {
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        std::ifstream in(candidate, std::ios::binary);
        if (!in) {
            continue;
        }
        std::vector<char> raw{(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t k = 0; k < raw.size(); ++k) {
            bytes[k] = static_cast<std::byte>(static_cast<unsigned char>(raw[k]));
        }
        return bytes;
    }
    return {};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    const std::vector<std::byte> bin = read_vocabulary();
    if (bin.empty()) {
        AURORA_LOG_ERROR("demo", "[demo_i18n] vocabulary binary not found; texts render as their call-site fallback");
    } else if (auto loaded = au::default_catalog().load_binary(std::span<const std::byte>{bin}); !loaded) {
        AURORA_LOG_ERROR("demo", "[demo_i18n] load_binary failed: ", loaded.error().message);
    }
    // 基准语言档：Locale{} 缺省槽查不到时按它取模板（见 specification/07 §6 回退链）。
    au::default_catalog().set_default_locale(AURORA_LOCALE_EN);

    auto locale = std::make_shared<au::State<au::Locale>>(AURORA_LOCALE_EN);
    au::Button toggle{au::ButtonProps{.label = au::LocalizedString{"Switch locale (en / zh-Hans-CN)"}}};
    toggle.on_click = [locale]() -> void {
        locale->set(locale->get().language == AURORA_LOCALE_EN.language ? AURORA_LOCALE_ZH : AURORA_LOCALE_EN);
    };

    au::Node content = au::Column{
        GradientTitle{"Internationalization / i18n"},
        gap(12),
        // 具名形参构造函数由词表生成：形参类型与顺序都来自 JSON，误用在编译期即拦下。
        au::Text{demo_i18n::msg::demo::greeting(3, "Ada")},
        au::Text{demo_i18n::msg::demo::invoice(1234.56, au::CivilDate{.year = 2026, .month = 10, .day = 25})},
        au::Text{demo_i18n::msg::demo::rank(2)},
        au::Text{demo_i18n::msg::demo::role("guest")},
        au::Text{demo_i18n::msg::demo::attached(2)},
        gap(8),
        std::move(toggle),
    };
    au::Node root = Card{au::LocaleProvider{locale, std::move(content)}};
    return run_demo(std::move(root), "i18n · Aurora Demo", 560.0F, 420.0F);
}
