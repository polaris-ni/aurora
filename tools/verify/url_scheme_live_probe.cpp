// tools/verify/url_scheme_live_probe.cpp — URL 方案注册真机验收探针（非 CTest）。
//
// 覆盖：`register_url_scheme()` / `unregister_url_scheme()` / `registered_url_scheme_owner()` /
// `is_url_scheme_registered()`（include/aurora/app/url_scheme.h）在真实用户级数据库上的整条接线：
// Windows `HKCU\Software\Classes\<scheme>`、Linux 桌面条目 + `mimeapps.list`、macOS Launch Services。
//
// 为什么必须有本探针（无头单测证明不了的部分）：
//   * 单测只验折算层与伪后端；「OS 数据库真的承认归属、撤下后归属真的回到空」只有读真机才证得了；
//   * 撤销的**归属校验**（归属是别人时一个字节都不改）在伪后端上只能验传参，真机上要看注册表 / ini
//     文件是否原封不动；
//   * 保留方案拒收与文法拒收不碰 OS，自动段先把这两道闸口钉死，再进入真机写入段。
//
// 构建（AURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_url_scheme
//   build-verify\aurora_verify_url_scheme.exe [--interactive]
//
// 自动段（无人值守）：
//   1. 能力位 `url_scheme_registration` 为 true（本探针只在接了三套桥之一的构建上有意义）；
//   2. 文法 / 保留方案拒收：`mailto` 被拒且 detail 点明「保留」，`1bad` / `a_b` / 空串被拒；
//   3. 注册一枚本次运行独占的方案（`aurora-verify-<ticks>`），归属查询非空且指向本应用；
//   4. Windows 追加归属冲突判据：用另一个身份撤销必须失败并点名当前归属，且归属不变；
//   5. 撤销后归属回到空、`is_url_scheme_registered` 为 false；重复撤销幂等成功；
//   6. macOS 的撤销按平台不对称返回 `url-scheme-unsupported`（Launch Services 无运行期撤接口），
//      本探针如实核对这一条，不把平台不对称算成失败。
// 人工段（`--interactive`）：打印方案名与归属，由人决定是否在系统设置里再核对一次。
//
// 平台说明：macOS 的注册要求探针运行在 **Info.plist 声明了 CFBundleURLTypes 的 .app 包**内。
// 裸可执行文件上注册如实失败（`url-scheme-failed` 点名缺包内声明），本探针按环境不可用收尾，
// 不冒充已验收。
//
// 退出码：
//   0  自动段全部通过（未跑人工段时人工项如实打印为待确认）
//   1  自动段断言失败
//   2  环境不可用（无 URL 方案桥 / macOS 裸可执行 / 临时身份不可用）
//   7  人工段存在未确认项

#include <cstdint>
#include <iostream>
#include <string>

#include "aurora/app/single_instance.h"
#include "aurora/app/url_scheme.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/window/platform.h"
#include "verify_args.h"
#include "verify_shell.h"

namespace {

#ifdef AURORA_PLATFORM_MACOS
/// @brief macOS 探针包内的固定方案（须与 probe 的 Info.plist 一致；本批未随构建提供 .app 封装）。
constexpr const char *AURORA_MACOS_PROBE_SCHEME = "aurora-verify-scheme";
/// @brief macOS 探针包的 bundle identifier（与 Info.plist 一致时归属查询应回它）。
constexpr const char *AURORA_MACOS_PROBE_BUNDLE = "com.aurora.verify.url-scheme";
#endif

/// @brief 构造探针应用身份。
/// @param exe 可执行路径。
/// @return 应用身份（macOS 另带探针 bundle id）。
[[nodiscard]] auto make_identity(const std::string &exe) -> aurora::AppIdentity {
    aurora::AppIdentity identity;
    identity.executable_path = exe;
#ifdef AURORA_PLATFORM_MACOS
    identity.bundle_identifier = AURORA_MACOS_PROBE_BUNDLE;
#endif
    return identity;
}

/// @brief 断言某次注册调用以非法参数被拒，并打印错误摘要。
/// @param scheme 待注册方案。
/// @param identity 应用身份。
/// @param label 判据描述。
/// @param require_reserved_detail true 时还要求 detail 点明「保留方案」。
auto expect_invalid(const std::string &scheme, const aurora::AppIdentity &identity, const std::string &label,
                    bool require_reserved_detail) -> void {
    const auto result = aurora::register_url_scheme(scheme, "Aurora Shell Verify", identity);
    const bool invalid = !result.ok() && result.error().code_enum == aurora::ErrorCode::GeneralInvalidArgument;
    const bool reserved_ok = !require_reserved_detail ||
                             (!result.ok() && aurora_verify::contains_ascii_ci(result.error().message, "reserved"));
    aurora_verify::check(invalid && reserved_ok, label);
    if (!result.ok()) {
        aurora_verify::note("[scheme] '" + scheme + "' -> " + result.error().code + " / " + result.error().message);
    }
}

/// @brief 自动段：能力位 / 闸口 / 真机注册 / 归属 / 撤销。
/// @param exe 可执行路径。
/// @param scheme 本次运行的独占方案名（输出）。
/// @return 进程退出码（0 / 1 / 2）。
auto run_auto_stage(const std::string &exe, std::string &scheme) -> int {
    aurora_verify::emit("== Aurora URL scheme live probe ==");
    aurora_verify::emit("executable: " + exe);

    const auto caps = aurora::platform().capabilities();
    aurora_verify::check(caps.url_scheme_registration, "[capability] url_scheme_registration is true");
    if (!caps.url_scheme_registration) {
        aurora_verify::env_fail(
            "[env] this build has no URL-scheme bridge",
            "configure with the platform shell-integration bridge (Windows / Linux / macOS backend)");
        return 2;
    }

    const auto identity = make_identity(exe);
    expect_invalid("mailto", identity, "[grammar] reserved scheme 'mailto' is rejected as reserved", true);
    expect_invalid("1bad", identity, "[grammar] scheme starting with a digit is rejected", false);
    expect_invalid("a_b", identity, "[grammar] scheme containing '_' is rejected", false);
    expect_invalid("", identity, "[grammar] empty scheme is rejected", false);

#ifdef AURORA_PLATFORM_MACOS
    scheme = AURORA_MACOS_PROBE_SCHEME;
#else
    scheme = "aurora-verify-" + aurora_verify::unique_suffix();
#endif
    aurora_verify::note("[scheme] probe scheme = " + scheme);

    const auto registered = aurora::register_url_scheme(scheme, "Aurora Shell Verify", identity);
    if (!registered.ok()) {
#ifdef AURORA_PLATFORM_MACOS
        if (aurora_verify::contains_ascii_ci(registered.error().message, "CFBundleURLTypes") ||
            aurora_verify::contains_ascii_ci(registered.error().message, "bundle")) {
            aurora_verify::env_fail(
                "[env] macOS URL-scheme registration requires a .app bundle that declares "
                "CFBundleURLTypes for the scheme",
                "run this probe from a probe .app whose Info.plist declares " + std::string(AURORA_MACOS_PROBE_SCHEME));
            return 2;
        }
#endif
        aurora_verify::check(false, "[register] register_url_scheme() succeeds");
        aurora_verify::note("[register] error: " + registered.error().code + " / " + registered.error().message);
        return 1;
    }
    aurora_verify::check(true, "[register] register_url_scheme() succeeds");

    const auto owner = aurora::registered_url_scheme_owner(scheme);
    aurora_verify::check(owner.ok() && !owner.value().empty(), "[owner] the OS database reports a non-empty owner");
    if (owner.ok()) {
        aurora_verify::note("[owner] " + owner.value());
    } else {
        aurora_verify::note("[owner] error: " + owner.error().code + " / " + owner.error().message);
    }
    bool owner_is_ours = false;
    if (owner.ok()) {
#ifdef AURORA_PLATFORM_WINDOWS
        owner_is_ours = aurora_verify::contains_ascii_ci(owner.value(), exe);
#elif defined(AURORA_PLATFORM_LINUX)
        owner_is_ours = aurora_verify::contains_ascii_ci(owner.value(), "aurora-url-");
#elif defined(AURORA_PLATFORM_MACOS)
        owner_is_ours = owner.value() == identity.bundle_identifier;
#else
        owner_is_ours = !owner.value().empty();
#endif
    }
    aurora_verify::check(owner_is_ours, "[owner] the owner identifies this probe application");
    aurora_verify::check(aurora::is_url_scheme_registered(scheme),
                         "[owner] is_url_scheme_registered() agrees with the owner query");

#ifdef AURORA_PLATFORM_WINDOWS
    // 归属冲突：另一个身份撤销必须失败、点名当前归属，且一个字节都不改。
    auto other = identity;
    other.executable_path = exe + ".other-owner";
    const auto hijack = aurora::unregister_url_scheme(scheme, other);
    aurora_verify::check(!hijack.ok(), "[ownership] unregister with a foreign identity is refused");
    if (!hijack.ok()) {
        aurora_verify::note("[ownership] refusal: " + hijack.error().code + " / " + hijack.error().message);
        aurora_verify::check(aurora_verify::contains_ascii_ci(hijack.error().message, exe),
                             "[ownership] the refusal names the current owner");
    }
    const auto still = aurora::registered_url_scheme_owner(scheme);
    aurora_verify::check(still.ok() && aurora_verify::contains_ascii_ci(still.value(), exe),
                         "[ownership] a refused unregister leaves the registration untouched");
#endif

    const auto revoked = aurora::unregister_url_scheme(scheme, identity);
#ifdef AURORA_PLATFORM_MACOS
    aurora_verify::check(!revoked.ok() && revoked.error().code_enum == aurora::ErrorCode::UrlSchemeUnsupported,
                         "[revoke] macOS reports url-scheme-unsupported (no runtime removal API)");
    if (!revoked.ok()) {
        aurora_verify::note("[revoke] " + revoked.error().code + " / " + revoked.error().message);
    }
    aurora_verify::note(
        "[revoke] macOS keeps the static CFBundleURLTypes declaration; the probe scheme stays owned "
        "by the probe bundle");
#else
    aurora_verify::check(revoked.ok(), "[revoke] unregister_url_scheme() succeeds");
    if (!revoked.ok()) {
        aurora_verify::note("[revoke] error: " + revoked.error().code + " / " + revoked.error().message);
        return 1;
    }
    const auto after = aurora::registered_url_scheme_owner(scheme);
    aurora_verify::check(after.ok() && after.value().empty(), "[revoke] the owner query returns empty after removal");
    aurora_verify::check(!aurora::is_url_scheme_registered(scheme), "[revoke] is_url_scheme_registered() is false");
    const auto again = aurora::unregister_url_scheme(scheme, identity);
    aurora_verify::check(again.ok(), "[revoke] a second unregister is an idempotent success");
#endif
    return 0;
}

/// @brief 人工段：打印方案与归属，供人在系统设置里复核。
/// @param scheme 探针方案名。
/// @return 0 = 已确认；7 = 未确认 / 无输入。
auto run_interactive_stage(const std::string &scheme) -> int {
    aurora_verify::emit("---- interactive segment ----");
    aurora_verify::emit("Probe scheme: " + scheme);
    aurora_verify::emit("Type y if the OS-side association looked sane to you, anything else to keep it pending: ");
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return 7;
    }
    for (char &ch : answer) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (!answer.empty() && answer.front() == 'y') {
        aurora_verify::emit("[PASS] human confirmed the URL-scheme association");
        return 0;
    }
    aurora_verify::emit("[PEND] human confirmation not given; the item stays unverified");
    return 7;
}

}  // namespace

// 探针入口不做异常兜底：任何抛出都该让进程带非零码终止，而不是被吞成「通过了」。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("URL scheme registration live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    aurora::init_console();

    const std::string exe = aurora_verify::executable_path(argc > 0 ? *argv : nullptr);
    std::string scheme;
    const int auto_code = run_auto_stage(exe, scheme);
    if (auto_code != 0) {
        return auto_code;
    }
    if (!interactive) {
        aurora_verify::emit("Skipping the interactive stage (re-run with --interactive to confirm visually).");
        aurora_verify::emit(aurora_verify::failures() == 0 ? "PASS (auto stage)" : "FAIL");
        return aurora_verify::failures() == 0 ? 0 : 1;
    }
    const int human_code = run_interactive_stage(scheme);
    if (aurora_verify::failures() != 0) {
        return 1;
    }
    if (aurora_verify::env_unavailable()) {
        return 2;
    }
    return human_code;
}
