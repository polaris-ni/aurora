/// 测试类型: unit
/// 目标单元: src/aurora/app/detail/shell_integration_translate.h
/// 测试说明: 桌面外壳集成**零平台折算层**的逐值契约——URL 方案文法与保留名单、扩展名规范化与
/// MIME 方向映射、命令行 URI 提取、`Exec=` 参数引用、桌面条目的拼装 / 取值 / 禁用态翻转、
/// `mimeapps.list` 形态的 INI 读写与分号列表增删。
///
/// 为何单测能跑产品代码路径：三平台桥（Win32 / Linux / macOS）都调用这一份折算层——URL 方案文法、
/// 扩展名冲突判定、Linux 桌面条目文本、单实例键派生都不在各自 TU 里复制。本文件直接断言这些纯文本
/// 语义，无需 OS。
///
/// 变异自证（改坏实现应转红的六条）：
///  * 从保留名单删掉 `mailto` → `reserved_schemes_cannot_be_claimed` 转红；
///  * 扩展名规范化不再折叠大小写 / 放行内嵌点 → `extension_normalization_is_one_key` 转红；
///  * `set_desktop_entry_hidden` 顺手删掉别的键 → `toggling_hidden_preserves_the_rest_of_the_entry` 转红；
///  * `list_add` 不去重 → `semicolon_lists_dedupe_and_keep_order` 转红；
///  * `ini_set` 写 `[Default Applications]` 时覆盖 `[Added Associations]` → `ini_edit_is_section_scoped` 转红；
///  * `extract_uri_argument` 认 `C:\...` 盘符路径为 URI → `uri_extraction_skips_windows_drive_paths` 转红。

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "aurora/app/detail/shell_integration_translate.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_shell_integration_translate {

namespace {

using desktop_shell::SchemeCheck;

/// @brief 文本是否包含子串。
[[nodiscard]] auto contains(std::string_view text, std::string_view needle) -> bool {
    return text.find(needle) != std::string_view::npos;
}

}  // namespace

AURORA_TEST_CASE(scheme_grammar_follows_rfc_3986) {
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("") == SchemeCheck::Empty);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("   ") == SchemeCheck::Empty);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("1abc") == SchemeCheck::InvalidSyntax);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("a_b") == SchemeCheck::InvalidSyntax);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("a:b") == SchemeCheck::InvalidSyntax);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("-abc") == SchemeCheck::InvalidSyntax);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("a b") == SchemeCheck::InvalidSyntax);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("a/b") == SchemeCheck::InvalidSyntax);

    // `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`：单字母合法，尾随符号亦合法。
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("a") == SchemeCheck::Valid);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("A1+.-") == SchemeCheck::Valid);
    AURORA_TEST_CHECK(desktop_shell::check_url_scheme("  myapp  ") == SchemeCheck::Valid);

    AURORA_TEST_CHECK_EQ(desktop_shell::normalize_url_scheme("  MyApp-Debug "), std::string{"myapp-debug"});
}

AURORA_TEST_CASE(reserved_schemes_cannot_be_claimed) {
    for (const char *reserved : {"http", "https", "file", "mailto", "ftp", "tel", "javascript", "WS", "MailTo"}) {
        AURORA_TEST_CHECK(desktop_shell::is_reserved_scheme(reserved));
        AURORA_TEST_CHECK(desktop_shell::check_url_scheme(reserved) == SchemeCheck::Reserved);
    }
    AURORA_TEST_CHECK_FALSE(desktop_shell::is_reserved_scheme("myapp"));
    AURORA_TEST_CHECK_FALSE(desktop_shell::is_reserved_scheme(""));
}

AURORA_TEST_CASE(extension_normalization_is_one_key) {
    const auto normalize = [](std::string_view raw) -> std::optional<std::string> {
        return desktop_shell::normalize_extension(raw);
    };
    AURORA_TEST_CHECK_EQ(normalize(".PNG").value_or(""), std::string{"png"});
    AURORA_TEST_CHECK_EQ(normalize("PNG").value_or(""), std::string{"png"});
    AURORA_TEST_CHECK_EQ(normalize("  .JpEg  ").value_or(""), std::string{"jpeg"});
    AURORA_TEST_CHECK_FALSE(normalize("").has_value());
    AURORA_TEST_CHECK_FALSE(normalize(".").has_value());
    AURORA_TEST_CHECK_FALSE(normalize("a.b").has_value());
    AURORA_TEST_CHECK_FALSE(normalize("a/b").has_value());
    AURORA_TEST_CHECK_FALSE(normalize("a\\b").has_value());
    AURORA_TEST_CHECK_FALSE(normalize("a b").has_value());
    AURORA_TEST_CHECK_FALSE(normalize("a:b").has_value());
}

AURORA_TEST_CASE(extension_mime_mapping_uses_the_shared_vocabulary) {
    AURORA_TEST_CHECK_EQ(std::string(desktop_shell::extension_to_mime("png").value_or("")), std::string{"image/png"});
    AURORA_TEST_CHECK_EQ(std::string(desktop_shell::extension_to_mime(".JPG").value_or("")), std::string{"image/jpeg"});
    AURORA_TEST_CHECK_EQ(std::string(desktop_shell::extension_to_mime(".PDF").value_or("")),
                         std::string{"application/pdf"});
    AURORA_TEST_CHECK_EQ(std::string(desktop_shell::extension_to_mime("txt").value_or("")), std::string{"text/plain"});
    AURORA_TEST_CHECK_FALSE(desktop_shell::extension_to_mime("aurora-unknown").has_value());
}

AURORA_TEST_CASE(uri_extraction_skips_windows_drive_paths) {
    using desktop_shell::extract_uri_argument;

    AURORA_TEST_CHECK_FALSE(extract_uri_argument({}).has_value());
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"--verbose", "-j4"}).has_value());
    // 盘符路径：单字母方案 + 反斜杠，不是 URI。
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"C:\\data\\file.txt"}).has_value());
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"c:/data/file.txt"}).has_value());
    // 太短 / 文法不合法。
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"ab:"}).has_value());
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"my_app:payload"}).has_value());
    AURORA_TEST_CHECK_FALSE(extract_uri_argument({"1abc:x"}).has_value());

    AURORA_TEST_CHECK_EQ(extract_uri_argument({"--flag", "myapp://open/1"}).value_or(""),
                         std::string{"myapp://open/1"});
    // 取第一条命中的，保持原样（不做解码 / 归一化）。
    AURORA_TEST_CHECK_EQ(extract_uri_argument({"myapp://one", "myapp://two"}).value_or(""), std::string{"myapp://one"});
    // 保留方案在**命令行**里放行：用户显式传参就是要路由。
    AURORA_TEST_CHECK_EQ(extract_uri_argument({"https://example.com"}).value_or(""),
                         std::string{"https://example.com"});
}

AURORA_TEST_CASE(exec_arguments_are_quoted_and_percent_escaped) {
    using desktop_shell::quote_exec_argument;

    AURORA_TEST_CHECK_EQ(quote_exec_argument("/opt/aurora/bin/app"), std::string{"/opt/aurora/bin/app"});
    AURORA_TEST_CHECK_EQ(quote_exec_argument("/opt/my app/run"), std::string{"\"/opt/my app/run\""});
    // Desktop Entry 规范：字面 `%` 必须成对，否则会被当成字段码。
    AURORA_TEST_CHECK_EQ(quote_exec_argument("100%"), std::string{"100%%"});
    AURORA_TEST_CHECK_EQ(quote_exec_argument(""), std::string{"\"\""});
    AURORA_TEST_CHECK_EQ(quote_exec_argument("a\"b"), std::string{"\"a\\\"b\""});
    AURORA_TEST_CHECK_EQ(quote_exec_argument("a$b"), std::string{"\"a\\$b\""});
    AURORA_TEST_CHECK_EQ(quote_exec_argument("a%b c"), std::string{"\"a%%b c\""});
    AURORA_TEST_CHECK_EQ(quote_exec_argument("a;b"), std::string{"\"a;b\""});
}

AURORA_TEST_CASE(desktop_entry_assembly_is_deterministic) {
    desktop_shell::DesktopEntry entry;
    entry.name = "Aurora Viewer";
    entry.exec = "\"/opt/aurora/bin/app\" %u";
    entry.icon = "aurora";
    entry.mime_types = "image/png;";
    entry.url_scheme = "myapp";
    entry.comment = "Preview images";
    const auto text = desktop_shell::build_desktop_entry(entry);
    AURORA_TEST_CHECK(contains(text, "[Desktop Entry]\n"));
    AURORA_TEST_CHECK(contains(text, "Type=Application\n"));
    AURORA_TEST_CHECK(contains(text, "Name=Aurora Viewer\n"));
    AURORA_TEST_CHECK(contains(text, "Comment=Preview images\n"));
    AURORA_TEST_CHECK(contains(text, "Exec=\"/opt/aurora/bin/app\" %u\n"));
    AURORA_TEST_CHECK(contains(text, "Icon=aurora\n"));
    AURORA_TEST_CHECK(contains(text, "MimeType=image/png;\n"));
    AURORA_TEST_CHECK(contains(text, "X-Aurora-Url-Scheme=myapp\n"));
    AURORA_TEST_CHECK(contains(text, "Terminal=false\n"));
    AURORA_TEST_CHECK(contains(text, "Hidden=false\n"));
    AURORA_TEST_CHECK_FALSE(contains(text, "X-GNOME-Autostart-enabled="));

    desktop_shell::DesktopEntry autostart;
    autostart.exec = "/opt/aurora/bin/app";
    autostart.autostart = true;
    const auto autostart_text = desktop_shell::build_desktop_entry(autostart);
    // 空显示名回落为可执行名之外的稳定默认值。
    AURORA_TEST_CHECK(contains(autostart_text, "Name=Aurora Application\n"));
    AURORA_TEST_CHECK(contains(autostart_text, "Hidden=false\n"));
    AURORA_TEST_CHECK(contains(autostart_text, "X-GNOME-Autostart-enabled=true\n"));

    // 取值：首个命中、去首尾空白；未出现为 nullopt。
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(text, "Name").value_or(""), std::string{"Aurora Viewer"});
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(text, "Exec").value_or(""),
                         std::string{"\"/opt/aurora/bin/app\" %u"});
    AURORA_TEST_CHECK_FALSE(desktop_shell::desktop_entry_value(text, "NoSuchKey").has_value());
}

AURORA_TEST_CASE(toggling_hidden_preserves_the_rest_of_the_entry) {
    // 用户手工留下的条目：带一条非本库添加的注释与一个自定义键。
    const std::string user_entry =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Aurora Viewer\n"
        "Exec=/opt/aurora/bin/app\n"
        "X-User-Note=keep me\n"
        "Hidden=false\n"
        "X-GNOME-Autostart-enabled=true\n";

    AURORA_TEST_CHECK_FALSE(desktop_shell::desktop_entry_disabled(user_entry));

    const auto disabled = desktop_shell::set_desktop_entry_hidden(user_entry, true);
    AURORA_TEST_CHECK(desktop_shell::desktop_entry_disabled(disabled));
    AURORA_TEST_CHECK(contains(disabled, "Hidden=true\n"));
    AURORA_TEST_CHECK(contains(disabled, "X-GNOME-Autostart-enabled=false\n"));
    // 禁用不删文件、不抹其它键：除开关键外逐行保留。
    AURORA_TEST_CHECK(contains(disabled, "X-User-Note=keep me\n"));
    AURORA_TEST_CHECK(contains(disabled, "Name=Aurora Viewer\n"));
    AURORA_TEST_CHECK(contains(disabled, "Exec=/opt/aurora/bin/app\n"));

    // 开 / 关两态只差开关键（验收判据：用户禁用后条目实体仍在）。
    for (const auto &line :
         {std::string{"[Desktop Entry]"}, std::string{"Type=Application"}, std::string{"Name=Aurora Viewer"},
          std::string{"Exec=/opt/aurora/bin/app"}, std::string{"X-User-Note=keep me"}}) {
        AURORA_TEST_CHECK(contains(disabled, line + "\n"));
    }

    // 严格往返：重新启用后与原文逐字节一致。
    const auto reenabled = desktop_shell::set_desktop_entry_hidden(disabled, false);
    AURORA_TEST_CHECK_EQ(reenabled, user_entry);
}

AURORA_TEST_CASE(desktop_entry_set_value_replaces_appends_and_deletes) {
    const std::string text =
        "[Desktop Entry]\n"
        "Name=Old\n"
        "Exec=/old\n"
        "Hidden=false\n";

    const auto replaced = desktop_shell::desktop_entry_set_value(text, "Name", "New");
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(replaced, "Name").value_or(""), std::string{"New"});
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(replaced, "Exec").value_or(""), std::string{"/old"});

    const auto appended = desktop_shell::desktop_entry_set_value(text, "Comment", "note");
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(appended, "Comment").value_or(""), std::string{"note"});

    const auto deleted = desktop_shell::desktop_entry_set_value(text, "Exec", "");
    AURORA_TEST_CHECK_FALSE(desktop_shell::desktop_entry_value(deleted, "Exec").has_value());
    AURORA_TEST_CHECK_EQ(desktop_shell::desktop_entry_value(deleted, "Name").value_or(""), std::string{"Old"});
}

AURORA_TEST_CASE(ini_edit_is_section_scoped) {
    const std::string mimeapps =
        "[Default Applications]\n"
        "image/png=other-app.desktop\n"
        "\n"
        "[Added Associations]\n"
        "image/png=other-app.desktop;aurora.desktop;\n";

    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(mimeapps, "Default Applications", "image/png").value_or(""),
                         std::string{"other-app.desktop"});
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(mimeapps, "Added Associations", "image/png").value_or(""),
                         std::string{"other-app.desktop;aurora.desktop;"});
    AURORA_TEST_CHECK_FALSE(desktop_shell::ini_value(mimeapps, "NoSuchSection", "image/png").has_value());

    // 只改目标节：另一个节逐字保留（声明候选不得动默认归属）。
    const auto updated = desktop_shell::ini_set(mimeapps, "Default Applications", "image/png", "aurora.desktop");
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(updated, "Default Applications", "image/png").value_or(""),
                         std::string{"aurora.desktop"});
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(updated, "Added Associations", "image/png").value_or(""),
                         std::string{"other-app.desktop;aurora.desktop;"});

    // 新增键落进已有节；新增节追加在末尾。
    const auto added_key = desktop_shell::ini_set(mimeapps, "Added Associations", "text/plain", "aurora.desktop;");
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(added_key, "Added Associations", "text/plain").value_or(""),
                         std::string{"aurora.desktop;"});
    const auto added_section = desktop_shell::ini_set(mimeapps, "Removed Associations", "image/png", "aurora.desktop");
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(added_section, "Removed Associations", "image/png").value_or(""),
                         std::string{"aurora.desktop"});

    // 删除键保留空节（用户留下的记录不被整体抹掉）。
    const auto removed = desktop_shell::ini_set(mimeapps, "Added Associations", "image/png", "");
    AURORA_TEST_CHECK_FALSE(desktop_shell::ini_value(removed, "Added Associations", "image/png").has_value());
    AURORA_TEST_CHECK(contains(removed, "[Added Associations]\n"));

    // CRLF 输入按 LF 规范输出。
    const auto crlf = desktop_shell::ini_set("[Default Applications]\r\nimage/png=a.desktop\r\n",
                                             "Default Applications", "image/png", "b.desktop");
    AURORA_TEST_CHECK_FALSE(contains(crlf, "\r"));
    AURORA_TEST_CHECK_EQ(desktop_shell::ini_value(crlf, "Default Applications", "image/png").value_or(""),
                         std::string{"b.desktop"});
}

AURORA_TEST_CASE(semicolon_lists_dedupe_and_keep_order) {
    AURORA_TEST_CHECK_EQ(desktop_shell::list_add("image/png;", "image/jpeg"), std::string{"image/png;image/jpeg;"});
    // 去重且保持首次出现序。
    AURORA_TEST_CHECK_EQ(desktop_shell::list_add("image/png;image/jpeg;", "image/png"),
                         std::string{"image/png;image/jpeg;"});
    AURORA_TEST_CHECK_EQ(desktop_shell::list_add("", "image/png;"), std::string{"image/png;"});
    AURORA_TEST_CHECK_EQ(desktop_shell::list_add("  image/png ;  ", "image/jpeg"),
                         std::string{"image/png;image/jpeg;"});

    AURORA_TEST_CHECK_EQ(desktop_shell::list_remove("image/png;image/jpeg;", "image/png"), std::string{"image/jpeg;"});
    AURORA_TEST_CHECK_EQ(desktop_shell::list_remove("image/png;", "image/png"), std::string{});
    AURORA_TEST_CHECK_EQ(desktop_shell::list_remove("image/png;", "text/plain"), std::string{"image/png;"});

    AURORA_TEST_CHECK(desktop_shell::list_contains("image/png;image/jpeg;", "image/jpeg"));
    AURORA_TEST_CHECK_FALSE(desktop_shell::list_contains("image/png;", "image/jpeg"));
}

}  // namespace aurora::test_cases::utest_shell_integration_translate
