#pragma once

// 桌面外壳集成的**平台桥缝**：内部头（src/aurora/app/detail/）。
//
// 与 `window/detail/recent_documents_translate.h` 的 `ProjectionBackend` 同格：公共入口是
// 进程级、且必须在**无头 CI** 里被证到「未接线报 X-unsupported / 失败报 X-failed / 三态不互顶」。
// 故每项特性持一个可注入的函数表：真机上由平台桥登记，单测里由伪造后端登记，两侧走同一条代码
// 路径——被测的是产品代码本身，不是它的复刻。
//
// 四条共同纪律：
// 1. **值持有**：槽位整体值持有函数表（桥若登记栈上对象也不会留悬垂引用），单线程 UI 不加锁；
// 2. **缺回调即未接线**：只有全部必需回调都在时槽位才算有效，公共入口据此返回 `X-unsupported`；
// 3. **错误原样透传**：桥返回的 `Result` 不被公共层重写（公共层只做参数折算与最终兜底）；
// 4. **本头零平台依赖**：不引 `<windows.h>` / Cocoa / Xlib，任何 CI 作业都能编译。
//
// 门控：无。本头在**所有**平台编译。

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/document_output.h"
#include "aurora/app/file_association.h"
#include "aurora/app/run_at_login.h"
#include "aurora/app/single_instance.h"
#include "aurora/app/url_scheme.h"
#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora::desktop_shell {

/// @brief URL 方案注册平台桥（登记 / 撤销 / 归属查询）。
struct UrlSchemeBackend {
    std::function<Result<void>(std::string_view scheme, std::string_view display_name, const AppIdentity &identity)>
        register_scheme;  ///< 登记（覆盖式）
    std::function<Result<void>(std::string_view scheme, const AppIdentity &identity)>
        unregister_scheme;  ///< 撤销（须先验归属）
    std::function<Result<std::string>(std::string_view scheme)> owner;  ///< 归属查询（空串 = 未登记）
};

/// @brief 文件关联平台桥（查询 / 声明 / 撤销声明）。
struct FileAssociationBackend {
    std::function<Result<AssociationInfo>(std::string_view extension)> query;  ///< 当前生效归属
    std::function<Result<void>(const AssociationDeclaration &declaration, std::string_view mime,
                               const AppIdentity &identity)>
        declare;  ///< 声明候选（不抢默认）
    std::function<Result<void>(std::string_view extension, const AppIdentity &identity)>
        undeclare;  ///< 撤销声明（须先验归属）
};

/// @brief 登录项平台桥（三态查询 / 开关写入）。
struct RunAtLoginBackend {
    std::function<RunAtLoginState(const AppIdentity &identity)> query;  ///< 三态查询（不抛错）
    std::function<Result<void>(bool enabled, const AppIdentity &identity)> set;  ///< 开 / 关
};

/// @brief 一次已绑定的打印会话（平台后端的抽象）。
///
/// 由 `PrintBackend::open` 产出；公共 `PrintSession` 持有它并在提交前完成 RGBA → 不透明合成。
/// @note Thread: 与公共 `PrintSession` 同线程
class PrintSessionBackend {
  public:
    virtual ~PrintSessionBackend() = default;

    PrintSessionBackend(const PrintSessionBackend &) = delete;
    auto operator=(const PrintSessionBackend &) -> PrintSessionBackend & = delete;
    PrintSessionBackend(PrintSessionBackend &&) = delete;
    auto operator=(PrintSessionBackend &&) -> PrintSessionBackend & = delete;

    /// @brief 提交一页（已合成到不透明底色的 RGB/RGBA 位图）。
    [[nodiscard]] virtual auto submit_page(const Image &page) -> Result<void> = 0;
    /// @brief 结束文档（幂等）。
    [[nodiscard]] virtual auto finish() -> Result<void> = 0;
    /// @brief 中止（丢弃已提交页）。
    virtual auto abort() -> void = 0;

  protected:
    PrintSessionBackend() = default;
};

/// @brief 打印平台桥（目标枚举 / 会话打开）。
struct PrintBackend {
    std::function<Result<std::vector<PrintTarget>>()> targets;  ///< 枚举打印目标（空列表 = 没有打印机）
    std::function<Result<std::unique_ptr<PrintSessionBackend>>(std::string_view target_id, const PageSpec &spec)>
        open;  ///< 打开会话
};

/// @brief 单实例平台桥：判首实例 / 转交 URI / 排空通道 / 释放。
///
/// `acquire` 的 `sink` 由公共层提供：平台桥把「后续收到的新 URI」交给它入队（**只入队**，
/// 不直接调用应用回调——回调统一由公共 `poll()` 在调用线程交付）。
struct SingleInstanceBackend {
    std::function<Result<SingleInstanceOutcome>(const AppIdentity &identity, const std::vector<std::string> &uris,
                                                const std::function<void(std::string)> &sink)>
        acquire;  ///< 获取（首实例 / 已转交 / 回落新实例）
    std::function<void()> poll;  ///< 排空平台通道（Linux 套接字；Windows/macOS 为空操作）
    std::function<void()> release;  ///< 释放平台资源（幂等）
};

// ---------- 槽位登记（覆盖式；每项独立，测试可只注入其一） ----------

/// @brief 登记 URL 方案平台桥。
auto register_url_scheme_backend(UrlSchemeBackend backend) -> void;
/// @brief 注销 URL 方案平台桥（幂等）。
auto unregister_url_scheme_backend() -> void;
/// @brief 当前 URL 方案平台桥；未接线或缺必需回调为 nullptr。
[[nodiscard]] auto url_scheme_backend() -> const UrlSchemeBackend *;

/// @brief 登记文件关联平台桥。
auto register_file_association_backend(FileAssociationBackend backend) -> void;
/// @brief 注销文件关联平台桥（幂等）。
auto unregister_file_association_backend() -> void;
/// @brief 当前文件关联平台桥；未接线或缺必需回调为 nullptr。
[[nodiscard]] auto file_association_backend() -> const FileAssociationBackend *;

/// @brief 登记登录项平台桥。
auto register_run_at_login_backend(RunAtLoginBackend backend) -> void;
/// @brief 注销登录项平台桥（幂等）。
auto unregister_run_at_login_backend() -> void;
/// @brief 当前登录项平台桥；未接线或缺必需回调为 nullptr。
[[nodiscard]] auto run_at_login_backend() -> const RunAtLoginBackend *;

/// @brief 登记打印平台桥。
auto register_print_backend(PrintBackend backend) -> void;
/// @brief 注销打印平台桥（幂等）。
auto unregister_print_backend() -> void;
/// @brief 当前打印平台桥；未接线或缺必需回调为 nullptr。
[[nodiscard]] auto print_backend() -> const PrintBackend *;

/// @brief 登记单实例平台桥。
auto register_single_instance_backend(SingleInstanceBackend backend) -> void;
/// @brief 注销单实例平台桥（幂等）。
auto unregister_single_instance_backend() -> void;
/// @brief 当前单实例平台桥；未接线或缺必需回调为 nullptr。
[[nodiscard]] auto single_instance_backend() -> const SingleInstanceBackend *;

}  // namespace aurora::desktop_shell
