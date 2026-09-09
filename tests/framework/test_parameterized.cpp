#include "parameterized.h"

namespace aurora::testing::detail {

auto ParamFamilyRegistry::instance() noexcept -> ParamFamilyRegistry& {
    static ParamFamilyRegistry registry;
    return registry;
}

auto ParamFamilyRegistry::push(ParamFamily& node) noexcept -> void {
    node.next = nullptr;
    if (tail_ == nullptr) {
        head_ = &node;
    } else {
        tail_->next = &node;
    }
    tail_ = &node;
}

auto ParamFamilyRegistry::families() const -> std::vector<const ParamFamily*> {
    std::vector<const ParamFamily*> result;
    for (const auto* node = head_; node != nullptr; node = node->next) {
        result.push_back(node);
    }
    return result;
}

ParamFamilyRegistrar::ParamFamilyRegistrar(std::string_view suite, std::string_view fixture, std::string_view case_name,
                                           const char* file, int line, TestParamBody run_at) noexcept
    : family_{.suite = suite,
              .fixture = fixture,
              .case_name = case_name,
              .file = file,
              .line = line,
              .run_at = run_at,
              .next = nullptr} {
    ParamFamilyRegistry::instance().push(family_);
}

}  // namespace aurora::testing::detail
