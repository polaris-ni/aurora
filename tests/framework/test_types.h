#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 基础类型
// ------------------------------------------------------------
// 本框架是 Aurora 仓库私有的测试基础设施：不进 include/、不进 aurora_api.json、
// 不受公共 API 兼容承诺约束。
//
// 本头只放跨模块共用的基础类型：用例状态 / 失败记录 / 执行上下文（含作用域追踪栈）/
// 注册节点与套件名推导。断言家族见 assertions.h，实际值渲染见 value_print.h，
// 注册与执行见 test_registry.* / test_runner.*；fixture、参数化、死亡测试与隔离设施
// 均在本目录内按模块扩展。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::testing {

/// @brief 用例执行结果状态。
enum class TestStatus : std::uint8_t {
    Passed,  ///< 通过（含无断言的纯执行）
    Failed,  ///< 失败（断言失败或未捕获异常）
    Skipped,  ///< 主动跳过（前置条件未满足）
};

/// @brief 单次断言失败记录。
struct Failure {
    std::string file;  ///< 断言所在文件
    int line = 0;  ///< 断言所在行
    std::string message;  ///< 人类可读的失败描述
};

/// @brief 用例执行期上下文：收集失败与诊断笔记。
///
/// 由 runner 在调用用例体前接管、用例体结束后释放（见 current_context_slot()）。
/// 断言宏经 current_context() 写入，故用例体内部无需显式传递上下文。
class TestContext {
  public:
    /// @brief 记录一次非致命失败（用例继续执行）。
    auto add_failure(Failure failure) -> void { failures_.push_back(std::move(failure)); }

    /// @brief 追加一条诊断笔记（verbose 模式下随用例结果输出）。
    auto add_note(std::string note) -> void { notes_.push_back(std::move(note)); }

    /// @brief 压入一层作用域追踪（AURORA_TEST_TRACE）。
    auto push_trace(std::string note) -> void { trace_.push_back(std::move(note)); }

    /// @brief 当前用例全名 `Suite.Case`（runner 接管上下文时写入）。
    ///
    /// 死亡测试需要让子进程重跑同一个用例，故用例身份必须在执行期可查。
    auto set_subject(std::string full_name) -> void { subject_ = std::move(full_name); }

    /// @brief 当前用例全名；非用例外执行期为空。
    [[nodiscard]] auto subject() const -> const std::string& { return subject_; }

    /// @brief 弹出一层作用域追踪（追踪块离开作用域时）。
    auto pop_trace() -> void {
        if (!trace_.empty()) {
            trace_.pop_back();
        }
    }

    /// @brief 是否已有失败记录。
    [[nodiscard]] auto failed() const -> bool { return !failures_.empty(); }

    /// @brief 全部失败记录。
    [[nodiscard]] auto failures() const -> const std::vector<Failure>& { return failures_; }

    /// @brief 全部诊断笔记。
    [[nodiscard]] auto notes() const -> const std::vector<std::string>& { return notes_; }

    /// @brief 追踪栈文本（外 → 内）；无追踪时返回空串。
    ///
    /// 失败记录时由断言内核拼进 message，使 console / 报告文件共享同一份上下文。
    [[nodiscard]] auto trace_text() const -> std::string {
        if (trace_.empty()) {
            return {};
        }
        std::string out = "\n  trace (outer → inner):";
        for (const auto& note : trace_) {
            out += "\n    ";
            out += note;
        }
        return out;
    }

  private:
    std::vector<Failure> failures_;
    std::vector<std::string> notes_;
    std::vector<std::string> trace_;
    std::string subject_;
};

/// @brief 当前线程正在执行的用例上下文槽位（runner 负责设置与清空）。
///
/// 采用 thread_local 而非进程级全局：并行度由 CTest 进程隔离承担（见计划 §3），
/// 此处线程局部仅为将来「若启用线程级并行」预留语义正确性，无额外运行时成本。
auto current_context_slot() -> TestContext*&;

/// @brief 当前线程正在执行的用例上下文；无活动用例（如静态初始化期断言）时为空指针。
[[nodiscard]] auto current_context() -> TestContext*;

/// @brief 用例函数体签名。
using TestBody = void (*)();

/// @brief 参数化用例函数体签名：按取值序号取参数后执行。
using TestParamBody = void (*)(std::size_t index);

/// @brief 静态注册的用例描述。
///
/// ⚠️ 注册发生在静态初始化期，故本结构刻意**不使用任何动态分配**：suite / case_name
/// 为指向字面量（静态存储期）的 string_view，节点间以侵入式 next 指针串联。
/// 由此 Registrar 构造恒为 noexcept，静态初始化期不可能抛异常
/// （clang-tidy bugprone-throwing-static-initialization）。
///
/// `body` 与 `param_body` **二选一**：普通用例、fixture 用例与类型参数化用例走 `body`
/// （展开在编译期即确定），值参数化用例走 `param_body`（取值序号在展开时才确定）。
struct TestCase {
    std::string_view suite;  ///< 套件名，恒等于测试文件 stem（计划 §3.3 第 3 条）
    std::string_view case_name;  ///< 用例名（文件内唯一）
    const char* file = nullptr;  ///< 注册所在源文件
    int line = 0;  ///< 注册所在行
    TestBody body = nullptr;  ///< 用例体（与 param_body 互斥）
    TestParamBody param_body = nullptr;  ///< 参数化用例体（与 body 互斥）
    std::size_t param_index = 0;  ///< 参数化用例的取值序号
    const TestCase* next = nullptr;  ///< 注册链表后继（由 TestRegistry 维护）

    /// @brief 全名 `Suite.Case`；按需拼接（运行期调用，不参与静态初始化）。
    [[nodiscard]] auto full_name() const -> std::string;

    /// @brief 执行用例体：按 body / param_body 分派。
    auto run_body() const -> void {
        if (body != nullptr) {
            body();
            return;
        }
        if (param_body != nullptr) {
            param_body(param_index);
        }
    }
};

/// @brief 从源路径推导套件名（去目录与扩展名）。
///
/// 例：`D:\\repo\\tests\\unit\\utest_color.cpp` → `utest_color`；POSIX 分隔符同样处理。
/// constexpr：注册期无运行时开销，且允许编译期断言套件名推导正确。
/// constexpr + noexcept：注册期在静态初始化期执行，整条调用链不得抛
/// （clang-tidy bugprone-throwing-static-initialization）。
[[nodiscard]] constexpr auto suite_from_path(std::string_view path) noexcept -> std::string_view {
    const auto sep = path.find_last_of("/\\");
    const auto begin = (sep == std::string_view::npos) ? 0 : sep + 1;
    auto end = path.rfind('.');
    if (end == std::string_view::npos || end < begin) {
        end = path.size();
    }
    return path.substr(begin, end - begin);
}

}  // namespace aurora::testing
