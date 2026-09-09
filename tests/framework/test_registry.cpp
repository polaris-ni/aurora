#include "test_registry.h"

#include <algorithm>
#include <string>

namespace aurora::testing {

auto TestCase::full_name() const -> std::string {
    std::string result;
    result.reserve(suite.size() + 1 + case_name.size());
    result.append(suite);
    result.push_back('.');
    result.append(case_name);
    return result;
}

auto TestRegistry::instance() noexcept -> TestRegistry& {
    static TestRegistry registry;
    return registry;
}

auto TestRegistry::push(TestCase& node) noexcept -> void {
    node.next = nullptr;
    if (tail_ == nullptr) {
        head_ = &node;
    } else {
        tail_->next = &node;
    }
    tail_ = &node;
}

auto TestRegistry::push_hook(FinalizeHook& hook) noexcept -> void {
    hook.next = nullptr;
    if (hook_tail_ == nullptr) {
        hook_head_ = &hook;
    } else {
        hook_tail_->next = &hook;
    }
    hook_tail_ = &hook;
}

auto TestRegistry::finalize() -> void {
    if (finalized_) {
        return;  // 幂等：main 与自检都可能触发，展开只发生一次
    }
    finalized_ = true;  // 先置位：展开期若再登记钩子属误用，不会被静默执行
    for (const auto* hook = hook_head_; hook != nullptr; hook = hook->next) {
        if (hook->run != nullptr) {
            hook->run();
        }
    }
}

auto TestRegistry::add_dynamic(std::string_view suite, std::string case_name, TestBody body, const char* file, int line)
    -> void {
    names_.push_back(std::move(case_name));
    dynamic_.push_back(TestCase{
        .suite = suite, .case_name = std::string_view{names_.back()}, .file = file, .line = line, .body = body});
    push(dynamic_.back());
}

auto TestRegistry::add_dynamic(std::string_view suite, std::string case_name, TestParamBody param_body,
                               std::size_t param_index, const char* file, int line) -> void {
    names_.push_back(std::move(case_name));
    dynamic_.push_back(TestCase{.suite = suite,
                                .case_name = std::string_view{names_.back()},
                                .file = file,
                                .line = line,
                                .param_body = param_body,
                                .param_index = param_index});
    push(dynamic_.back());
}

auto TestRegistry::cases() const -> std::vector<const TestCase*> {
    std::vector<const TestCase*> result;
    auto count = std::size_t{0};
    for (const auto* node = head_; node != nullptr; node = node->next) {
        ++count;
    }
    result.reserve(count);
    for (const auto* node = head_; node != nullptr; node = node->next) {
        result.push_back(node);
    }
    return result;
}

auto TestRegistry::suites() const -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto* test_case : cases()) {
        const std::string suite{test_case->suite};
        if (std::ranges::find(result, suite) == result.end()) {
            result.push_back(suite);
        }
    }
    return result;
}

namespace detail {

Registrar::Registrar(std::string_view suite, std::string_view case_name, const char* file, int line,
                     TestBody body) noexcept
    : node_{.suite = suite, .case_name = case_name, .file = file, .line = line, .body = body, .next = nullptr} {
    TestRegistry::instance().push(node_);
}

FinalizeRegistrar::FinalizeRegistrar(void (*run)()) noexcept : hook_{.run = run, .next = nullptr} {
    TestRegistry::instance().push_hook(hook_);
}

}  // namespace detail

}  // namespace aurora::testing
