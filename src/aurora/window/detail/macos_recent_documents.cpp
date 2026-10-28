// macos_recent_documents.cpp — macOS 最近文档投影桥（NSDocumentController）。
//
// 与 win32_recent_documents.cpp 同一套折算层契约：本文件只做形态转换与投递，次序 / 去重 /
// 截断由 `recent_documents_translate.h` 唯一决定。门控在头文件内，非 macOS 编译为空 TU。
//
// 与 Win32 的两处平台差异（已在头注申报）：
// 1) `noteNewRecentDocumentURL:` 把每次投递放到 MRU 最前，故这里按计划**逆序**投递，
//    使最终「新 → 旧」序与折算计划一致；
// 2) 撤销没有逐条 API，只能 `clearRecentDocuments:` 清掉本应用的整份最近文档集合。
#include "aurora/window/detail/macos_recent_documents.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/window/detail/recent_documents_translate.h"

namespace aurora::detail {

namespace {

/// @brief 桥的进程级状态：宿主引用计数 + 本进程投影记账（单线程 UI 契约，无锁）。
struct BridgeState {
    int refs = 0;  ///< 活跃宿主守卫数（归零即注销通路）
    std::vector<std::string> projected;  ///< 已投影未撤销的路径（诊断 / 计数用）
};

/// @brief 进程唯一状态（函数内 static，规避静态初始化顺序问题）。
auto state() -> BridgeState & {
    static BridgeState s;
    return s;
}

/// @brief 校验并转换一条 UTF-8 路径为 NSURL。
/// @param path 原始路径（字符串级口径，不校验存在性）。
/// @return 成功给出 autorelease 的 NSURL；形态非法时 nullptr。
auto make_url(const std::string &path) -> NSURL * {
    NSString *text = [NSString stringWithUTF8String:path.c_str()];
    if (text == nil || [text length] == 0) {
        return nil;
    }
    return [NSURL fileURLWithPath:text];
}

/// @brief 该 URL 是否已在 `recentDocumentURLs` 中（按解析符号链接后的规范路径比较）。
auto is_listed(NSDocumentController *controller, NSURL *url) -> bool {
    NSURL *wanted = [url URLByResolvingSymlinksInPath];
    for (NSURL *listed in [controller recentDocumentURLs]) {
        if ([[listed URLByResolvingSymlinksInPath] isEqual:wanted]) {
            return true;
        }
    }
    return false;
}

/// @brief 后端回调：逐条 `noteNewRecentDocumentURL:`（逆序投递以保持新 → 旧）。
///
/// 投递后**回读核验**：macOS 只在「已签名的应用包」内才真正记录最近文档——未签名或不在
/// .app 包内运行时，`noteNewRecentDocumentURL:` 静默丢弃、无返回值可查（本机实测 macOS 26）。
/// 故这里以 `recentDocumentURLs` 为准绳，未记为一条就如实报错，不谎报成功。
auto project_backend(const std::vector<std::string> &plan) -> Result<void> {
    @autoreleasepool {
        if (plan.empty()) {
            return Result<void>{};
        }
        NSDocumentController *controller = [NSDocumentController sharedDocumentController];
        if (controller == nil) {
            return make_error(ErrorCode::PlatformUnavailable, "recent-documents: NSDocumentController is unavailable");
        }
        // 先把整份计划转换成 URL：任一条形态非法则一条都不投，避免半截计划落进 OS。
        std::vector<NSURL *> urls;
        urls.reserve(plan.size());
        for (const auto &path : plan) {
            NSURL *url = make_url(path);
            if (url == nil) {
                return make_error(ErrorCode::PlatformUnavailable, "recent-documents: path is not convertible to NSURL");
            }
            urls.push_back(url);
        }
        // 逆序投递：每次调用把该条放到 MRU 最前，故最后投计划首条（最新）。
        for (auto it = urls.rbegin(); it != urls.rend(); ++it) {
            [controller noteNewRecentDocumentURL:*it];
        }
        for (NSURL *url : urls) {
            if (!is_listed(controller, url)) {
                return make_error(
                    ErrorCode::PlatformUnavailable,
                    "recent-documents: the OS did not record the entry (requires a code-signed app bundle)");
            }
        }
        auto &record = state().projected;
        for (const auto &path : plan) {
            if (std::find(record.begin(), record.end(), path) == record.end()) {
                record.emplace_back(path);
            }
        }
        if (record.size() > AURORA_MACOS_RECENT_DOCUMENTS_MAX_ENTRIES) {
            record.erase(record.begin(),
                         record.begin() +
                             static_cast<std::ptrdiff_t>(record.size() - AURORA_MACOS_RECENT_DOCUMENTS_MAX_ENTRIES));
        }
        AURORA_LOG_DEBUG("recent-documents", "projected ", plan.size(), " entries to NSDocumentController");
        return Result<void>{};
    }
}

/// @brief 后端回调：`clearRecentDocuments:`（清本应用整份集合）。
auto clear_backend() -> Result<void> {
    @autoreleasepool {
        NSDocumentController *controller = [NSDocumentController sharedDocumentController];
        if (controller == nil) {
            return make_error(ErrorCode::PlatformUnavailable, "recent-documents: NSDocumentController is unavailable");
        }
        [controller clearRecentDocuments:nil];
        if ([[controller recentDocumentURLs] count] != 0) {
            return make_error(ErrorCode::PlatformUnavailable,
                              "recent-documents: the OS did not clear the recent-document list");
        }
        state().projected.clear();
        AURORA_LOG_DEBUG("recent-documents", "cleared the application recent-document list");
        return Result<void>{};
    }
}

}  // namespace

auto macos_recent_documents_project(const std::vector<std::string> &plan) -> Result<void> {
    return project_backend(plan);
}

auto macos_recent_documents_clear() -> Result<void> { return clear_backend(); }

auto macos_recent_documents_projected_count() -> std::size_t { return state().projected.size(); }

MacOSRecentDocumentsBridge::MacOSRecentDocumentsBridge() {
    auto &self = state();
    if (self.refs == 0) {
        recent_documents::register_projection_backend(recent_documents::ProjectionBackend{
            .max_entries = AURORA_MACOS_RECENT_DOCUMENTS_MAX_ENTRIES,
            .project = &project_backend,
            .clear = &clear_backend,
        });
    }
    ++self.refs;
    installed_ = true;
}

auto MacOSRecentDocumentsBridge::detach() -> void {
    if (!installed_) {
        return;  // 幂等：窗口销毁路径与析构共用一条出口
    }
    installed_ = false;
    auto &self = state();
    self.refs = self.refs > 0 ? self.refs - 1 : 0;
    if (self.refs == 0) {
        recent_documents::unregister_projection_backend();
    }
}

MacOSRecentDocumentsBridge::~MacOSRecentDocumentsBridge() { detach(); }

auto macos_recent_documents_ensure_registered() -> bool {
    // 进程级长存守卫：登记一次即不释放（投影记录与通路都与窗口无关）。
    static MacOSRecentDocumentsBridge bootstrap;
    return bootstrap.installed();
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
