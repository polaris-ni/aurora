/// 测试类型: unit
/// 目标单元: include/aurora/app/file_association.h
/// 测试说明: 文件关联公共入口的契约——扩展名规范化（去前导点 / 小写 / 拒绝路径分隔符与内嵌点）先于
/// 平台桥发生、查询结果原样透传（空归属不是错误）、声明的 MIME 解析优先级（显式值 → `mime_types.h`
/// 扩展名表 → 报错）、缺失可执行路径的拒收、撤销时对「归属是别人」的原样透传，以及无桥时的
/// `file-association-unsupported` 退化。
///
/// 为何单测能跑产品代码路径：文件关联桥是进程级单槽（`desktop_shell::FileAssociationBackend`），
/// 本文件注入伪后端，公共入口走同一条代码；真机上平台桥登记同名后端。
///
/// 本文件**不改真实注册表 / `mimeapps.list`**：伪后端一经登记即被采用，按需自举的真桥不会启动。
///
/// 变异自证（改坏实现应转红的五条）：
///  * 把扩展名规范化挪到平台桥之后 → `normalization_precedes_the_backend` 的「桥未被调用」断言转红；
///  * 声明时忽略显式 MIME 而恒用扩展名表 → `declaration_prefers_the_explicit_mime` 转红；
///  * 未知扩展名 + 空 MIME 不报错（交给桥猜）→ `declaration_requires_a_resolvable_mime` 转红；
///  * 撤销时不透传桥的 `file-association-failed` → `undeclare_propagates_ownership_conflicts` 转红；
///  * 无桥时返回成功 → `degradation_reports_unsupported_without_a_wired_bridge` 转红。

#include <cstddef>
#include <string>
#include <utility>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/file_association.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_file_association {

namespace {

/// @brief 伪造文件关联后端的观测状态与可编程结论。
struct FakeAssociation {
    int query_calls = 0;  ///< 查询次数
    int declare_calls = 0;  ///< 声明次数
    int undeclare_calls = 0;  ///< 撤销次数
    std::string last_extension;  ///< 最近一次收到的规范化扩展名
    std::string last_mime;  ///< 最近一次收到的 MIME
    std::string last_display_name;  ///< 最近一次收到的显示名
    std::string last_path;  ///< 最近一次收到的可执行路径
    AssociationInfo query_result{.application = "com.example.other", .source = "mimeapps.list"};  ///< 查询成功值
    bool query_fails = false;  ///< 查询是否报 `file-association-failed`
    bool declare_fails = false;  ///< 声明是否报 `file-association-failed`
    bool undeclare_fails = false;  ///< 撤销是否报 `file-association-failed`
    std::string failure_message{"file-association: backend refused"};  ///< 失败 detail
};

/// @brief 每个用例进出清空文件关联通路的伪造后端。
class FileAssociationBackendGuard {
  public:
    FileAssociationBackendGuard() { desktop_shell::unregister_file_association_backend(); }
    ~FileAssociationBackendGuard() { desktop_shell::unregister_file_association_backend(); }
    FileAssociationBackendGuard(const FileAssociationBackendGuard &) = delete;
    auto operator=(const FileAssociationBackendGuard &) -> FileAssociationBackendGuard & = delete;
    FileAssociationBackendGuard(FileAssociationBackendGuard &&) = delete;
    auto operator=(FileAssociationBackendGuard &&) -> FileAssociationBackendGuard & = delete;
};

/// @brief 安装伪造文件关联桥。
void install_fake_backend(FakeAssociation &state) {
    desktop_shell::FileAssociationBackend backend;
    backend.query = [&state](std::string_view extension) -> Result<AssociationInfo> {
        ++state.query_calls;
        state.last_extension = std::string(extension);
        if (state.query_fails) {
            return make_error(ErrorCode::FileAssociationFailed, state.failure_message);
        }
        return state.query_result;
    };
    backend.declare = [&state](const AssociationDeclaration &declaration, std::string_view mime,
                               const AppIdentity &identity) -> Result<void> {
        ++state.declare_calls;
        state.last_extension = declaration.extension;
        state.last_mime = std::string(mime);
        state.last_display_name = declaration.display_name;
        state.last_path = identity.executable_path;
        if (state.declare_fails) {
            return make_error(ErrorCode::FileAssociationFailed, state.failure_message);
        }
        return Result<void>{};
    };
    backend.undeclare = [&state](std::string_view extension, const AppIdentity &identity) -> Result<void> {
        ++state.undeclare_calls;
        state.last_extension = std::string(extension);
        state.last_path = identity.executable_path;
        if (state.undeclare_fails) {
            return make_error(ErrorCode::FileAssociationFailed, state.failure_message);
        }
        return Result<void>{};
    };
    desktop_shell::register_file_association_backend(std::move(backend));
}

/// @brief 带可执行路径的身份（命令模板可拼装）。
[[nodiscard]] auto identity_with_path() -> AppIdentity {
    AppIdentity identity;
    identity.executable_path = "/opt/aurora/bin/app";
    return identity;
}

}  // namespace

AURORA_TEST_CASE(normalization_precedes_the_backend) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    const auto expect_invalid = [](std::string_view extension) {
        auto result = associated_application(extension);
        AURORA_TEST_REQUIRE(!result.ok());
        AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    };
    expect_invalid("");
    expect_invalid(".");
    expect_invalid("a.b");
    expect_invalid("a/b");
    expect_invalid("a\\b");
    expect_invalid("a b");
    expect_invalid("a\tb");
    AURORA_TEST_CHECK_EQ(state.query_calls, 0);

    // 前导点与大写折叠：`.PNG` / `PNG` / `png` 是同一个键。
    auto result = associated_application(".PNG");
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK_EQ(state.last_extension, std::string{"png"});
    AURORA_TEST_CHECK_EQ(state.query_calls, 1);

    AssociationDeclaration declaration;
    declaration.extension = ".PNG";
    AURORA_TEST_REQUIRE(register_file_association(declaration, identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.last_extension, std::string{"png"});
    AURORA_TEST_CHECK_EQ(state.declare_calls, 1);

    AURORA_TEST_REQUIRE(unregister_file_association(".PNG", identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.last_extension, std::string{"png"});
    AURORA_TEST_CHECK_EQ(state.undeclare_calls, 1);
}

AURORA_TEST_CASE(query_passes_the_backend_answer_through) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    auto result = associated_application("png");
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK_EQ(result.value().application, std::string{"com.example.other"});
    AURORA_TEST_CHECK_EQ(result.value().source, std::string{"mimeapps.list"});

    // 无关联：空串配成功（空集合不是错误）。
    state.query_result = AssociationInfo{};
    result = associated_application("png");
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK(result.value().application.empty());
    AURORA_TEST_CHECK(result.value().source.empty());

    state.query_fails = true;
    result = associated_application("png");
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::FileAssociationFailed);
}

AURORA_TEST_CASE(declaration_prefers_the_explicit_mime) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    AssociationDeclaration mapped;
    mapped.extension = "png";
    mapped.display_name = "PNG image";
    AURORA_TEST_REQUIRE(register_file_association(mapped, identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.last_mime, std::string{"image/png"});
    AURORA_TEST_CHECK_EQ(state.last_display_name, std::string{"PNG image"});
    AURORA_TEST_CHECK_EQ(state.last_path, std::string{"/opt/aurora/bin/app"});

    AssociationDeclaration explicit_mime;
    explicit_mime.extension = "png";
    explicit_mime.mime_type = "application/x-aurora-png";
    AURORA_TEST_REQUIRE(register_file_association(explicit_mime, identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.last_mime, std::string{"application/x-aurora-png"});

    // 未知扩展名但显式给了 MIME：仍可声明（本表的「未登记」不等于「不可用」）。
    AssociationDeclaration unknown;
    unknown.extension = "xyz";
    unknown.mime_type = "application/x-xyz";
    AURORA_TEST_REQUIRE(register_file_association(unknown, identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.last_mime, std::string{"application/x-xyz"});
    AURORA_TEST_CHECK_EQ(state.declare_calls, 3);
}

AURORA_TEST_CASE(declaration_requires_a_resolvable_mime) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    AssociationDeclaration unknown;
    unknown.extension = "xyz";
    auto result = register_file_association(unknown, identity_with_path());
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.declare_calls, 0);
}

AURORA_TEST_CASE(declaration_requires_an_executable_path) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    AssociationDeclaration declaration;
    declaration.extension = "png";
    AppIdentity no_path;
    auto result = register_file_association(declaration, no_path);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.declare_calls, 0);

    auto undeclare = unregister_file_association("png", no_path);
    AURORA_TEST_REQUIRE(!undeclare.ok());
    AURORA_TEST_CHECK_EQ(undeclare.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.undeclare_calls, 0);
}

AURORA_TEST_CASE(undeclare_propagates_ownership_conflicts) {
    FileAssociationBackendGuard guard;
    FakeAssociation state;
    install_fake_backend(state);

    // 归属是别人：桥拒改，公共层不得改写成成功（撤销越过别人的声明属越界动作）。
    state.undeclare_fails = true;
    state.failure_message = "file-association: 'png' is registered to com.other.app";
    auto result = unregister_file_association("PNG", identity_with_path());
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::FileAssociationFailed);
    AURORA_TEST_CHECK(result.error().message.find("com.other.app") != std::string::npos);
    AURORA_TEST_CHECK_EQ(state.undeclare_calls, 1);
    AURORA_TEST_CHECK_EQ(state.last_extension, std::string{"png"});

    // 声明失败同样原样透传。
    state.undeclare_fails = false;
    state.declare_fails = true;
    AssociationDeclaration declaration;
    declaration.extension = "png";
    auto failed = register_file_association(declaration, identity_with_path());
    AURORA_TEST_REQUIRE(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::FileAssociationFailed);
    AURORA_TEST_CHECK_EQ(failed.error().message, state.failure_message);
}

AURORA_TEST_CASE(degradation_reports_unsupported_without_a_wired_bridge) {
    FileAssociationBackendGuard guard;
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_LINUX) || \
    (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    // 这三条腿有可按需自举的真桥：清空槽位后公共入口会去碰真实注册表 / 桌面条目，不适合无头断言。
    AURORA_TEST_SKIP("native file-association bridge boots on demand on this platform");
#else
    auto query = associated_application("png");
    AURORA_TEST_REQUIRE(!query.ok());
    AURORA_TEST_CHECK_EQ(query.error().code_enum, ErrorCode::FileAssociationUnsupported);

    AssociationDeclaration declaration;
    declaration.extension = "png";
    auto declare = register_file_association(declaration, identity_with_path());
    AURORA_TEST_REQUIRE(!declare.ok());
    AURORA_TEST_CHECK_EQ(declare.error().code_enum, ErrorCode::FileAssociationUnsupported);

    auto undeclare = unregister_file_association("png", identity_with_path());
    AURORA_TEST_REQUIRE(!undeclare.ok());
    AURORA_TEST_CHECK_EQ(undeclare.error().code_enum, ErrorCode::FileAssociationUnsupported);
#endif
}

}  // namespace aurora::test_cases::utest_file_association
