#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "test_types.h"

namespace aurora::testing {

/// @brief 延后展开钩子：静态初始化期只挂链，真正的展开在 finalize 阶段执行。
///
/// 值参数化 / 类型参数化用例的**数量与名字**取决于运行期才确定的取值表，无法在
/// 静态初始化期注册（那需要动态分配）。故 INSTANTIATE 等宏只登记本结构（静态存储期、
/// 仅改写指针），展开动作推迟到 `TestRegistry::finalize()`。
struct FinalizeHook {
    void (*run)() = nullptr;  ///< 展开动作（运行期执行，可自由分配）
    const FinalizeHook* next = nullptr;  ///< 钩子链表后继
};

/// @brief 进程级用例注册表（静态注册的唯一落点）。
///
/// 用例经 AURORA_TEST_CASE 宏在静态初始化期注册；runner 只读遍历，执行期不加锁
/// （并行度由 CTest 进程隔离承担，见计划 §3）。
///
/// ⚠️ 内部以**侵入式单向链表**存储注册节点：注册发生在静态初始化期，此时做任何
/// 动态分配都可能抛异常并直接 terminate（bugprone-throwing-static-initialization）。
/// 链表节点即各 Registrar 自身的成员（静态存储期），注册动作仅改写指针。
/// 运行期展开的参数化用例走 `add_dynamic`（节点存 `std::deque`，地址稳定，
/// 供 `cases()` 返回的指针长期有效），只在 finalize 阶段调用。
class TestRegistry {
  public:
    /// @brief 进程唯一注册表。函数内 static，规避静态初始化顺序问题。
    ///
    /// noexcept：注册表默认构造平凡无分配，供静态初始化期的 Registrar 安全调用
    /// （clang-tidy bugprone-throwing-static-initialization 要求整条调用链 noexcept）。
    [[nodiscard]] static auto instance() noexcept -> TestRegistry&;

    /// @brief 尾插一个注册节点；不分配、不抛异常。
    auto push(TestCase& node) noexcept -> void;

    /// @brief 尾挂一个延后展开钩子；不分配、不抛异常。
    auto push_hook(FinalizeHook& hook) noexcept -> void;

    /// @brief 执行全部展开钩子（幂等）。`main` 起手调用一次，之后注册表只读。
    auto finalize() -> void;

    /// @brief 是否已展开完成。
    [[nodiscard]] auto finalized() const -> bool { return finalized_; }

    /// @brief 追加一个运行期展开的用例（普通用例体）。
    auto add_dynamic(std::string_view suite, std::string case_name, TestBody body, const char* file, int line) -> void;

    /// @brief 追加一个运行期展开的用例（参数化用例体 + 取值序号）。
    auto add_dynamic(std::string_view suite, std::string case_name, TestParamBody param_body, std::size_t param_index,
                     const char* file, int line) -> void;

    /// @brief 全部已注册用例（静态注册在前、展开追加在后；运行期展开链表）。
    [[nodiscard]] auto cases() const -> std::vector<const TestCase*>;

    /// @brief 全部套件名（去重，按首次注册顺序）。
    [[nodiscard]] auto suites() const -> std::vector<std::string>;

  private:
    TestCase* head_ = nullptr;
    TestCase* tail_ = nullptr;
    FinalizeHook* hook_head_ = nullptr;
    FinalizeHook* hook_tail_ = nullptr;
    bool finalized_ = false;
    std::deque<TestCase> dynamic_;  ///< 展开用例的节点存储（deque 保证元素地址稳定）
    std::deque<std::string> names_;  ///< 展开用例的名字池（TestCase 只持 string_view）
};

namespace detail {

/// @brief 静态注册器：构造即入链，无状态、不分配。
class Registrar {
  public:
    Registrar(std::string_view suite, std::string_view case_name, const char* file, int line, TestBody body) noexcept;

  private:
    TestCase node_;
};

/// @brief 延后展开钩子的静态登记器：构造即挂链，不分配、不抛异常。
class FinalizeRegistrar {
  public:
    explicit FinalizeRegistrar(void (*run)()) noexcept;

  private:
    FinalizeHook hook_;
};

}  // namespace detail

}  // namespace aurora::testing

/// @brief 注册一个用例。
///
/// 用法：
/// @code
/// AURORA_TEST_CASE(my_case) {
///     AURORA_TEST_CHECK(1 + 1 == 2);
/// }
/// @endcode
/// 全名为 `<文件 stem>.my_case`；套件名由 __FILE__ 推导、恒等于测试文件 stem，
/// 不可自定义 —— 这样 CTest 的 `--run=<stem>` 一定能筛中该文件下全部用例。
#define AURORA_TEST_CASE(case_name)                                                                                   \
    static auto aurora_test_body_##case_name() -> void;                                                               \
    namespace {                                                                                                       \
    const ::aurora::testing::detail::Registrar aurora_test_registrar_##case_name{                                     \
        ::aurora::testing::suite_from_path(__FILE__), #case_name, __FILE__, __LINE__, &aurora_test_body_##case_name}; \
    }                                                                                                                 \
    static auto aurora_test_body_##case_name() -> void
