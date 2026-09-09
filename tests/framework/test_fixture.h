#pragma once

// ============================================================
// 测试框架（tests/framework/）—— fixture 用例（AURORA_TEST_F）
// ------------------------------------------------------------
// 与 GoogleTest 同构：用例体是「派生自 fixture 的生成类」的成员函数，故可直接访问
// fixture 的 protected 成员；SetUp / TearDown 保持 protected（与 GoogleTest 一致），
// 生命周期由生成类自己的成员函数驱动 —— 框架外部代码不碰钩子。
//
// 用法：
//   class tmp_fixture : public aurora::testing::Fixture {
//   protected:
//       auto SetUp() -> void override { ... }
//       auto TearDown() -> void override { ... }
//       std::filesystem::path dir;
//   };
//   AURORA_TEST_F(tmp_fixture, creates_dir) { AURORA_TEST_CHECK(!dir.empty()); }
//
// 全名 = `<文件 stem>.<fixture>_<case>`：套件名仍由 __FILE__ 推导（TEST-R8），
// 用例名内嵌 fixture 名，避免不同文件里同名 fixture 的用例互相撞名。
// ============================================================

#include <utility>

#include "test_registry.h"
#include "test_types.h"

namespace aurora::testing {

/// @brief fixture 基类：AURORA_TEST_F / AURORA_TEST_P / AURORA_TYPED_TEST 的统一契约。
///
/// 用例实例由框架就地构造，故禁止拷贝与移动（与 GoogleTest 的 `::testing::Test` 同构）；
/// 派生 fixture 的共享状态放成员里，靠 SetUp 重建。
class Fixture {
  public:
    Fixture() = default;
    virtual ~Fixture() = default;

    Fixture(const Fixture&) = delete;
    auto operator=(const Fixture&) -> Fixture& = delete;
    Fixture(Fixture&&) = delete;
    auto operator=(Fixture&&) -> Fixture& = delete;

  protected:
    /// @brief 用例体执行前的准备（派生类按需覆写）。
    virtual auto SetUp() -> void {}

    /// @brief 用例体执行后的清理（含用例抛出的路径，保证恰好执行一次）。
    virtual auto TearDown() -> void {}
};

namespace detail {

/// @brief 生成用例类的基座：把「SetUp → 用例体 → TearDown」的次序与异常安全收在框架里。
///
/// 本类是 fixture 的派生类，故受保护的钩子可在此访问；`run()` 对外公开，供框架 shim 调用。
/// 用例体抛出（含致命断言的 `CaseAbort`）时先执行 TearDown 再原样重抛，使 runner 仍能
/// 按异常类型判定用例状态，同时不泄漏夹具持有的资源。
template <typename FixtureClass>
class CaseBase : public FixtureClass {
  public:
    auto run() -> void {
        this->SetUp();
        try {
            this->case_body();
        } catch (...) {
            this->TearDown();
            throw;
        }
        this->TearDown();
    }

  protected:
    /// @brief 用例体（由 AURORA_TEST_F / AURORA_TEST_P / AURORA_TYPED_TEST 生成的类实现）。
    virtual auto case_body() -> void = 0;
};

/// @brief 构造 fixture 用例实例并执行其 `run()`。
///
/// `Args` 供值参数化用例透传取值；AURORA_TEST_F / 类型参数化用例默认构造。
template <typename CaseClass, typename... Args>
auto run_case_instance(Args&&... args) -> void {
    CaseClass instance{std::forward<Args>(args)...};
    instance.run();
}

}  // namespace detail

}  // namespace aurora::testing

/// @brief 注册一个 fixture 用例（对标 GoogleTest 的 `TEST_F`）。
///
/// 生成类放在**当前命名空间**（与 GoogleTest 一致），使生成的用例体能以成员函数身份
/// 访问 fixture 的 protected 成员；测试文件本就以 `aurora::test_cases::<stem>` 自成一域。
#define AURORA_TEST_F(fixture_class, case_name)                                                             \
    class aurora_test_fixture_##fixture_class##_##case_name                                                 \
        : public ::aurora::testing::detail::CaseBase<fixture_class> {                                       \
      protected:                                                                                            \
        auto case_body() -> void override;                                                                  \
    };                                                                                                      \
    namespace {                                                                                             \
    auto aurora_test_fixture_run_##fixture_class##_##case_name() -> void {                                  \
        ::aurora::testing::detail::run_case_instance<aurora_test_fixture_##fixture_class##_##case_name>();  \
    }                                                                                                       \
    const ::aurora::testing::detail::Registrar aurora_test_fixture_registrar_##fixture_class##_##case_name{ \
        ::aurora::testing::suite_from_path(__FILE__), #fixture_class "_" #case_name, __FILE__, __LINE__,    \
        &aurora_test_fixture_run_##fixture_class##_##case_name};                                            \
    }                                                                                                       \
    auto aurora_test_fixture_##fixture_class##_##case_name::case_body() -> void
