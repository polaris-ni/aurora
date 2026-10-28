/* macOS 最近文档 OS 投影真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `RecentDocuments::project_to_os()` 经 `NSDocumentController` 把路径真正投进应用的
//      最近文档集合——探针用**独立取证**（`recentDocumentURLs`）核对条目出现与内容；
//   ✅ 重复投影不产生重复条目（幂等）；
//   ✅ `clear_os_projection()` 后 OS 集合清空；
//   ✅ 投影不改变应用内 store；
//   ✅ `PlatformCapabilities::recent_documents_os_projection` 在 macOS 后端上翻位；
//   ✅ 无窗口宿主路径（本探针不建窗口，投影通路自举）。
//
//   ❌ 不证明系统菜单（Dock / 文件菜单）里「肉眼可见」——那取决于应用是否有 NSMenu；
//      本探针证明的是 NSDocumentController 的集合内容，即系统菜单的数据源。
//
// ⚠️ 运行形态是判据的一部分（本机实测 macOS 26）：`NSDocumentController` 只对**已签名的
//    .app 包**真正记录最近文档；裸可执行文件（未进包/未签名）调用成功但集合恒空。故 CMake
//    目标在构建后把本探针封装为 `AuroraMacOSRecentProbe.app` 并 ad-hoc 签名，请经包内可执行
//    文件运行（直接跑 build-macos/aurora_verify_macos_recent_documents 会以退出码 2 报环境不可用）。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_recent_documents
//   ./build-macos/AuroraMacOSRecentProbe.app/Contents/MacOS/aurora_verify_macos_recent_documents
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（NSDocumentController 无法建立）
//   3  判据不适用
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_recent_documents 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/recent_documents.h"
#include "aurora/window/detail/macos_recent_documents.h"
#include "aurora/window/platform.h"

#import <AppKit/AppKit.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "verify_print.h"

namespace {

int g_checks = 0;
int g_failures = 0;

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 42), detail, "\n");
}

/// @brief 取 NSDocumentController 当前记录的文件名集合（独立取证，不经库内记账）。
auto recent_names() -> std::vector<std::string> {
    std::vector<std::string> names;
    NSDocumentController *controller = [NSDocumentController sharedDocumentController];
    for (NSURL *url in [controller recentDocumentURLs]) {
        NSString *name = [url lastPathComponent];
        const char *utf8 = name != nil ? [name UTF8String] : nullptr;
        if (utf8 != nullptr) {
            names.emplace_back(utf8);
        }
    }
    return names;
}

auto contains(const std::vector<std::string> &haystack, const std::string &needle) -> bool {
    for (const auto &item : haystack) {
        if (item == needle) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto main() -> int {
    @autoreleasepool {
        AURORA_LOG_RAW("verify", "aurora_verify_macos_recent_documents\n");
        AURORA_LOG_RAW("verify", "----------------------------------------\n");

        [NSApplication sharedApplication];
        if ([NSDocumentController sharedDocumentController] == nil) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: NSDocumentController could not be created\n");
            return 2;
        }
        if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: not running from an app bundle; "
                           "NSDocumentController does not record recent documents for bare executables\n");
            return 2;
        }

        const bool capability = aurora::platform().capabilities().recent_documents_os_projection;
        check(capability, "capability.recent_documents_os_projection");

        auto &store = aurora::RecentDocuments::instance();
        // 清掉本应用此前留下的最近文档（探针每次运行从确定状态起手）。
        (void)store.clear_os_projection();
        store.clear_all();
        check(recent_names().empty(), "recent document list starts empty",
              aurora_verify::format_uint(recent_names().size()));

        const std::string tag = "aurora-recent-" + std::to_string(::getpid());
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        std::vector<std::string> paths;
        for (int i = 0; i < 3; ++i) {
            const std::filesystem::path p = dir / (tag + "-" + std::to_string(i) + ".txt");
            std::ofstream out(p);
            out << "aurora recent documents probe\n";
            paths.push_back(p.string());
        }
        for (const auto &p : paths) {
            store.add(p);
        }
        check(store.size() == 3, "in-app store has 3 entries", aurora_verify::format_uint(store.size()));

        auto projected = store.project_to_os();
        check(projected.ok(), "project_to_os succeeds", projected ? "" : projected.error().message);
        if (!projected) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: projection failed; the app bundle must be code-signed\n");
            return 2;
        }
        const auto names_after = recent_names();
        check(names_after.size() == 3, "NSDocumentController records 3 entries",
              aurora_verify::format_uint(names_after.size()));
        bool all_present = true;
        for (int i = 0; i < 3; ++i) {
            const std::string name = tag + "-" + std::to_string(i) + ".txt";
            all_present = all_present && contains(names_after, name);
            check(contains(names_after, name), "OS list contains " + name);
        }
        check(all_present, "all in-app entries reached the OS list");
        check(aurora::detail::macos_recent_documents_projected_count() == 3, "bridge projected count is 3",
              aurora_verify::format_uint(aurora::detail::macos_recent_documents_projected_count()));

        // 重复投影同一列表：NSDocumentController 对已有 URL 去重，条数不应增长。
        auto projected_again = store.project_to_os();
        check(projected_again.ok(), "second project_to_os succeeds");
        check(recent_names().size() == 3, "repeated projection does not duplicate entries",
              aurora_verify::format_uint(recent_names().size()));

        check(store.size() == 3, "store unchanged by projection", aurora_verify::format_uint(store.size()));

        auto cleared = store.clear_os_projection();
        check(cleared.ok(), "clear_os_projection succeeds", cleared ? "" : cleared.error().message);
        check(recent_names().empty(), "OS recent list empty after clear",
              aurora_verify::format_uint(recent_names().size()));
        check(store.size() == 3, "store unchanged by clear", aurora_verify::format_uint(store.size()));
        check(aurora::detail::macos_recent_documents_projected_count() == 0, "bridge projected count reset");

        // 清理临时文件（store 只持有字符串，不依赖文件存在性）。
        for (const auto &p : paths) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }

        AURORA_LOG_RAW("verify", "----------------------------------------\n");
        AURORA_LOG_RAW("verify", "summary: ", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        return g_failures == 0 ? 0 : 1;
    }
}
