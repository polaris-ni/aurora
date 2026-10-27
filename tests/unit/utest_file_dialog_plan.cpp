/// 测试类型: unit
/// 目标单元: src/aurora/app/detail/file_dialog_plan.h, src/aurora/app/detail/file_dialog_plan.cpp
/// 测试说明: 文件对话框折算层（**零平台头**，两座真实现腿共用）的四条纯规则逐值全覆盖——末段提取
/// （同时认 `/` 与 `\`、尾分隔符读作目录形态）、建议名的截断与目录形态各留一条可归因 WARN（不留静默
/// 丢弃）、缺省扩展名归一（前导点与首尾空白都不是扩展名的一部分）、按缺省扩展名补全的三条不补情形
/// （空名 / 空扩展名 / 名字末段已带点）。另证多字节名字在本层按字节搬运不被拆断（本机活动码页 936 下
/// 这条通道只能靠「逐字节相等」判，「看起来能用」不算）。
///
/// 本文件**不触碰任何 OS 对话框**：投递（`SetTitle` / `SetFolder` / `SetFileName` /
/// `SetDefaultExtension`）与自绘腿的播种各在 `utest_file_dialog.cpp`（自绘腿无头段）与真机探针
/// `aurora_verify_win32_file_dialog` 里判，本层只证「投出去的那一份字符串是怎么算出来的」。
///
/// 变异自证（改坏实现应转红的四条）：
///  * 把 `last_path_segment` 的分隔符集合改成只认 `/` → `last_path_segment_*` 与截断留痕两例转红
///    （Windows 风格的建议名会被整串投进名字栏，OS 直接拒掉）；
///  * 摘掉 `suggested_file_name` 的两条 WARN → 对应留痕判据转红（截断是改写用户输入，必须可归因）；
///  * 让 `normalize_extension` 不去前导点 → Win32 腿会落成 `name..txt`（自绘腿会拼出 `.txt.txt`）；
///  * 让 `with_default_extension` 对已带点的名字也补 → 三条不补情形转红（隐式改写用户打过的扩展名）。

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/detail/file_dialog_plan.h"
#include "aurora/core/log.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_file_dialog_plan {

namespace {

/// @brief RAII：接管 Logger sink 捕获日志行；析构恢复级别与默认 stderr，
///        不把 Logger 全局状态泄漏给其他用例（与 `utest_file_dialog` 同构）。
class CapturedLog {
  public:
    CapturedLog() : level_{aurora::Logger::instance().level()} {
        aurora::Logger::instance().set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLog() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_sink(nullptr);  // 恢复默认 stderr
    }
    CapturedLog(const CapturedLog &) = delete;
    auto operator=(const CapturedLog &) & = delete;
    CapturedLog(CapturedLog &&) = delete;
    auto operator=(CapturedLog &&) & = delete;

    /// @brief 已捕获行里是否有一条同时含全部给定片段（级别标签 + 分类 + 文案）。
    [[nodiscard]] auto has_line_with(std::initializer_list<const char *> fragments) const -> bool {
        for (const std::string &line : lines) {
            bool all = true;
            for (const char *f : fragments) {
                if (line.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

    /// @brief 同时含全部给定片段的行数（「截断留一条、不多留」这条判据要的是确切次数）。
    [[nodiscard]] auto count_lines_with(std::initializer_list<const char *> fragments) const -> std::size_t {
        std::size_t hits = 0;
        for (const std::string &line : lines) {
            bool all = true;
            for (const char *f : fragments) {
                if (line.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                ++hits;
            }
        }
        return hits;
    }

    std::vector<std::string> lines;  // NOLINT(*-non-private-member-variables-in-classes) 捕获到的日志行

  private:
    aurora::LogLevel level_;
};

}  // namespace

AURORA_TEST_CASE(last_path_segment_takes_the_tail_after_either_separator) {
    using aurora::file_dialog_plan::last_path_segment;

    // 两种分隔符都认（应用侧传来的串可能是任一种写法）。
    AURORA_TEST_CHECK_STREQ(last_path_segment("dir/sub/report.txt"), "report.txt");
    AURORA_TEST_CHECK_STREQ(last_path_segment("C:\\logs\\report.txt"), "report.txt");
    AURORA_TEST_CHECK_STREQ(last_path_segment("a/b\\c.txt"), "c.txt");
    // 无分隔符原样返回；空串回空串。
    AURORA_TEST_CHECK_STREQ(last_path_segment("report.txt"), "report.txt");
    AURORA_TEST_CHECK_TRUE(last_path_segment("").empty());
    // 尾分隔符 ⇒ 末段为空（那是目录不是文件名，交调用方当作「不表达意图」）。
    AURORA_TEST_CHECK_TRUE(last_path_segment("dir/").empty());
    AURORA_TEST_CHECK_TRUE(last_path_segment("dir\\").empty());
    // 分隔符出现在名字中间（如盘符相对段）不影响「取最后一个分隔符之后」。
    AURORA_TEST_CHECK_STREQ(last_path_segment("/data/archive.tar.gz"), "archive.tar.gz");
}

AURORA_TEST_CASE(suggested_file_name_truncates_with_attribution) {
    using aurora::file_dialog_plan::suggested_file_name;

    // 判据 3：三形同归 `report.txt`，前两形各留一条可归因 WARN，末形不留。
    {
        CapturedLog log;
        AURORA_TEST_CHECK_STREQ(suggested_file_name("dir/sub/report.txt"), "report.txt");
        AURORA_TEST_CHECK_EQ(log.count_lines_with({"WRN", "file_dialog", "suggested_name carries a path"}), 1U);
    }
    {
        CapturedLog log;
        AURORA_TEST_CHECK_STREQ(suggested_file_name("C:\\logs\\report.txt"), "report.txt");
        AURORA_TEST_CHECK_EQ(log.count_lines_with({"WRN", "file_dialog", "suggested_name carries a path"}), 1U);
    }
    {
        CapturedLog log;
        AURORA_TEST_CHECK_STREQ(suggested_file_name("report.txt"), "report.txt");
        AURORA_TEST_CHECK_FALSE(log.has_line_with({"WRN", "file_dialog"}));
    }
    // 空串 = 不表达意图：空结果且不落痕（与「不传该字段」同形）。
    {
        CapturedLog log;
        AURORA_TEST_CHECK_TRUE(suggested_file_name("").empty());
        AURORA_TEST_CHECK_FALSE(log.has_line_with({"WRN", "file_dialog"}));
    }
    // 目录形态（尾分隔符）：回空并留一条「这不是文件名」的痕——静默吞掉会让人以为投递失败。
    {
        CapturedLog log;
        AURORA_TEST_CHECK_TRUE(suggested_file_name("dir/sub/").empty());
        AURORA_TEST_CHECK_EQ(log.count_lines_with({"WRN", "file_dialog", "names a directory"}), 1U);
    }
    // WARN 里带上原文，归因才落到调用方传的那一串。
    {
        CapturedLog log;
        (void)suggested_file_name("dir/sub/secret.txt");
        AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "dir/sub/secret.txt", "secret.txt"}));
    }
}

AURORA_TEST_CASE(normalize_extension_drops_leading_dots_and_blanks) {
    using aurora::file_dialog_plan::normalize_extension;

    // `txt` 与 `.txt` 同解：Win32 的 `SetDefaultExtension` 要不含点的那一份（OS 自己补点）。
    AURORA_TEST_CHECK_STREQ(normalize_extension("txt"), "txt");
    AURORA_TEST_CHECK_STREQ(normalize_extension(".txt"), "txt");
    AURORA_TEST_CHECK_STREQ(normalize_extension("..txt"), "txt");
    // 首尾空白不是扩展名的一部分。
    AURORA_TEST_CHECK_STREQ(normalize_extension("  .txt  "), "txt");
    AURORA_TEST_CHECK_STREQ(normalize_extension(" \ttxt\n"), "txt");
    // 空 / 只有点 / 只有空白 ⇒ 空（= 不补全）。
    AURORA_TEST_CHECK_TRUE(normalize_extension("").empty());
    AURORA_TEST_CHECK_TRUE(normalize_extension(".").empty());
    AURORA_TEST_CHECK_TRUE(normalize_extension("   ").empty());
    // 只削前导点与空白，内部字符（含多字节）一律原样。
    AURORA_TEST_CHECK_STREQ(normalize_extension(".tar.gz"), "tar.gz");
    // 只削前导点：尾点是用户写的一部分，不当成空白处理。
    AURORA_TEST_CHECK_STREQ(normalize_extension("txt."), "txt.");
    AURORA_TEST_CHECK_STREQ(normalize_extension(".\xe4\xb8\xad\xe6\x96\x87"), "\xe4\xb8\xad\xe6\x96\x87");
}

AURORA_TEST_CASE(with_default_extension_appends_only_when_absent) {
    using aurora::file_dialog_plan::with_default_extension;

    // 判据 4 的四形。
    AURORA_TEST_CHECK_STREQ(with_default_extension("report", "txt"), "report.txt");
    AURORA_TEST_CHECK_STREQ(with_default_extension("report.md", "txt"), "report.md");
    AURORA_TEST_CHECK_STREQ(with_default_extension("report", ".txt"), "report.txt");
    AURORA_TEST_CHECK_TRUE(with_default_extension("", "txt").empty());
    AURORA_TEST_CHECK_STREQ(with_default_extension("report", ""), "report");
    // 三条不补情形各自独立：空名补出 `.txt` 就把「取消」变成了写入。
    AURORA_TEST_CHECK_STREQ(with_default_extension("", ""), "");
    AURORA_TEST_CHECK_STREQ(with_default_extension("report", "  .  "), "report");
    // 判定只看末段：带目录的串在末尾补，目录部分不动。
    AURORA_TEST_CHECK_STREQ(with_default_extension("dir/sub/report", "txt"), "dir/sub/report.txt");
    AURORA_TEST_CHECK_STREQ(with_default_extension("dir/sub/report.md", "txt"), "dir/sub/report.md");
    // 点出现在目录段而末段没点 ⇒ 仍补（否则「C:\\a.b\\report」会被误判成已带扩展名）。
    AURORA_TEST_CHECK_STREQ(with_default_extension("C:\\a.b\\report", "txt"), "C:\\a.b\\report.txt");
}

AURORA_TEST_CASE(non_ascii_names_pass_through_byte_for_byte) {
    using aurora::file_dialog_plan::last_path_segment;
    using aurora::file_dialog_plan::suggested_file_name;
    using aurora::file_dialog_plan::with_default_extension;

    // 转义写死的 UTF-8 中文名（「中文报告」）——判据 5 的字节串，不经 argv、不经码页。
    const std::string name = "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a";
    const std::string with_ext = name + ".txt";
    const std::string path = std::string("\xe7\x9b\xae\xe5\xbd\x95/") + name + ".txt";  // 「目录/中文报告.txt」

    // 分隔符是 ASCII 字节，多字节名字不会被误认成分隔符；取末段按字节不拆断。
    AURORA_TEST_CHECK_STREQ(last_path_segment(path), with_ext);
    // 折算结果是同一份字节（判据 5：投递前后逐字节相等，非「看起来能用」）。
    AURORA_TEST_CHECK_STREQ(suggested_file_name(name), name);
    AURORA_TEST_CHECK_STREQ(suggested_file_name(path), with_ext);
    // 扩展名补全对多字节名同样只做尾部追加。
    AURORA_TEST_CHECK_STREQ(with_default_extension(name, "txt"), with_ext);
    AURORA_TEST_CHECK_STREQ(with_default_extension(with_ext, "txt"), with_ext);
    // 多字节「扩展名」本身也不被拆断：前导点被削掉，其余字节原样。
    const std::string dotted_ext = std::string(".").append(name);
    AURORA_TEST_CHECK_STREQ(aurora::file_dialog_plan::normalize_extension(dotted_ext), name);
    AURORA_TEST_CHECK_STREQ(with_default_extension("report", dotted_ext), "report." + name);
}

}  // namespace aurora::test_cases::utest_file_dialog_plan
