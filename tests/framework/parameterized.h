#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 参数化用例
// ------------------------------------------------------------
// 两类参数化，能力与 GoogleTest 对齐（函数名按本仓库 lower_case 规范调整）：
//
//   值参数化（fixture 须继承 `aurora::testing::TestWithParam<V>`，体内用 `param()` 取值）：
//     AURORA_TEST_P(square_fixture, area_matches_edge) { AURORA_TEST_CHECK(...param()...); }
//     AURORA_INSTANTIATE_TEST_SUITE_P(edges, square_fixture, values_of(1, 2, 4));
//     → 展开用例名 `<prefix>/<fixture>_<case>/<取值名或序号>`
//
//   类型参数化（`suite` 是「以单个类型为模板参数、派生自 Fixture」的类模板；体内 `TestType`）：
//     template <typename TestType> class numeric_fixture : public aurora::testing::Fixture { ... };
//     AURORA_TYPED_TEST_SUITE(numeric_fixture, int, float, double)
//     AURORA_TYPED_TEST(numeric_fixture, keeps_identity) { ... }
//     → 展开用例名 `<case>/<类型短名>`
//
// ⚠️ 展开时机：参数化用例的**数量与名字**取决于取值表，无法在静态初始化期注册（那需要
// 动态分配，会踩 bugprone-throwing-static-initialization）。故 TEST_P / INSTANTIATE /
// TYPED_TEST 在静态期只挂链登记描述符，真正的 `add_dynamic` 发生在 runner 起手的
// `TestRegistry::finalize()` —— `--list` / `--run` / `--filter` 看到的即展开后的全集。
// 同一文件内 `AURORA_INSTANTIATE_TEST_SUITE_P` 须写在对应 `AURORA_TEST_P` 之后
// （跨文件无此约束：展开一律在 finalize 阶段）。
//
// ⚠️ 取值传递：取值表按 `FixtureClass` 汇总成一张**扁平表**，展开出的用例体只携带一个
// 全局序号；构造 fixture 前把当次取值写入「按取值类型分槽」的槽位，`TestWithParam` 在
// 构造期快照到自己那份 —— 与 GoogleTest 的 `GetParam()` 语义等价，且派生 fixture
// 不必自己转发构造函数。
// ============================================================

#include <cstddef>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_fixture.h"
#include "test_registry.h"
#include "test_types.h"
#include "value_print.h"

namespace aurora::testing {

/// @brief 类型参数化的类型清单（`AURORA_TYPED_TEST_SUITE` 的第二实参）。
template <typename... Types>
struct TypeList {
    using Tuple = std::tuple<Types...>;  ///< 按下标取类型的元组形态
};

namespace detail {

/// @brief 当前取值槽位（按取值类型分槽；单线程 runner 内在用例边界写入）。
///
/// 槽位放这里而非 `TestWithParam` 内部，是为了让「展开器写入」与「fixture 读取」
/// 必然指向同一对象：展开器只认 `FixtureClass`，fixture 只认 `Value`。
template <typename Value>
[[nodiscard]] auto param_slot() -> std::optional<Value>& {
    static std::optional<Value> storage;
    return storage;
}

/// @brief 写入当前取值（构造 fixture 前调用）。
template <typename FixtureClass>
auto set_current_param(const typename FixtureClass::ParamType& value) -> void {
    param_slot<typename FixtureClass::ParamType>() = value;
}

}  // namespace detail

/// @brief 值参数化用例的 fixture 基类。
///
/// 方法取名 `param()` 而非 GoogleTest 的 `GetParam()`：本仓库方法命名一律 lower_case。
/// 取值由框架在用例边界写入槽位、本类构造时快照，故派生 fixture 无需转发构造函数。
template <typename Value>
class TestWithParam : public Fixture {
  public:
    using ParamType = Value;  ///< 供框架推导取值表类型

    TestWithParam() : value_(detail::param_slot<Value>().value()) {}

    /// @brief 本用例的取值。
    [[nodiscard]] auto param() const -> const Value& { return value_; }

  private:
    Value value_;
};

namespace detail {

/// @brief 默认名字生成器：返回空串表示「改用取值序号」。
struct DefaultParamName {
    template <typename Value>
    [[nodiscard]] auto operator()(const Value&) const -> std::string {
        return {};
    }
};

/// @brief 一次 `AURORA_INSTANTIATE_TEST_SUITE_P` 的取值区间与命名信息。
template <typename Value>
struct ParamInstantiation {
    std::string prefix;  ///< 实例化名
    std::function<std::string(const Value&)> name_of;  ///< 名字生成器（可空）
    std::size_t offset = 0;  ///< 扁平取值表中的起始下标
    std::size_t count = 0;  ///< 取值个数
};

/// @brief 某个 fixture 的扁平取值表（多实例化共享，用例体只带全局序号）。
template <typename FixtureClass>
[[nodiscard]] auto param_values() -> std::vector<typename FixtureClass::ParamType>& {
    static std::vector<typename FixtureClass::ParamType> values;
    return values;
}

/// @brief 按全局序号取值（生成的用例体使用）。
template <typename FixtureClass>
[[nodiscard]] auto param_at(std::size_t index) -> const typename FixtureClass::ParamType& {
    return param_values<FixtureClass>()[index];
}

/// @brief 值参数化用例族：`AURORA_TEST_P` 登记的静态描述符。
struct ParamFamily {
    std::string_view suite;  ///< 文件 stem（与静态用例同规则，保证 `--run=<stem>` 筛得中）
    std::string_view fixture;  ///< `#fixture_class` —— 与 INSTANTIATE 的挂接键
    std::string_view case_name;  ///< `#case_name`
    const char* file = nullptr;  ///< TEST_P 所在源文件
    int line = 0;  ///< TEST_P 所在行
    TestParamBody run_at = nullptr;  ///< 按取值序号执行的用例体
    const ParamFamily* next = nullptr;
};

/// @brief 用例族注册表（侵入式链表：静态初始化期只改指针，不分配）。
class ParamFamilyRegistry {
  public:
    [[nodiscard]] static auto instance() noexcept -> ParamFamilyRegistry&;

    auto push(ParamFamily& node) noexcept -> void;

    /// @brief 全部用例族（按登记顺序）。
    [[nodiscard]] auto families() const -> std::vector<const ParamFamily*>;

  private:
    ParamFamily* head_ = nullptr;
    ParamFamily* tail_ = nullptr;
};

/// @brief 用例族的静态登记器：构造即挂链，不分配、不抛异常。
class ParamFamilyRegistrar {
  public:
    ParamFamilyRegistrar(std::string_view suite, std::string_view fixture, std::string_view case_name, const char* file,
                         int line, TestParamBody run_at) noexcept;

  private:
    ParamFamily family_;
};

/// @brief 类型名的短形态（截到最后一个限定符/空格，供类型参数化用例名使用）。
template <typename T>
[[nodiscard]] inline auto short_type_name() -> std::string {
    const std::string qualified = type_name_str<T>();
    const auto last = qualified.find_last_of(": ");
    return (last == std::string::npos) ? qualified : qualified.substr(last + 1);
}

/// @brief 用例名末段：名字生成器给出的名字优先，否则退回本次实例化内的序号。
template <typename Value, typename Values>
[[nodiscard]] auto param_case_suffix(const ParamInstantiation<Value>& inst, const Values& values,
                                     std::size_t global_index) -> std::string {
    if (inst.name_of) {
        const auto generated = inst.name_of(values[global_index]);
        if (!generated.empty()) {
            return generated;
        }
    }
    return std::to_string(global_index - inst.offset);
}

/// @brief 展开某次实例化下的全部用例族：每个「族 × 取值」生成一条可执行用例。
template <typename FixtureClass, typename Value>
auto expand_param_families(std::string_view fixture_key, const ParamInstantiation<Value>& inst) -> void {
    const auto& values = param_values<FixtureClass>();
    auto matched = std::size_t{0};
    for (const auto* family : ParamFamilyRegistry::instance().families()) {
        if (family->fixture != fixture_key) {
            continue;
        }
        ++matched;
        for (std::size_t index = 0; index < inst.count; ++index) {
            const auto global = inst.offset + index;
            std::string case_name = inst.prefix + "/" + std::string{family->fixture} + "_" +
                                    std::string{family->case_name} + "/" + param_case_suffix(inst, values, global);
            TestRegistry::instance().add_dynamic(family->suite, std::move(case_name), family->run_at, global,
                                                 family->file, family->line);
        }
    }
    if (matched == 0) {
        // 取值表已登记却没有任何用例族命中：通常是 INSTANTIATE 写在了同文件 TEST_P 之前。
        // 只报诊断不改判定，避免框架用法错误升级成红灯。
        std::fprintf(stderr, "[test] no AURORA_TEST_P family for fixture '%s' (instantiation '%s')\n",
                     std::string{fixture_key}.c_str(), inst.prefix.c_str());
    }
}

/// @brief 登记一次实例化：写入扁平取值表，随后展开该 fixture 的全部用例族。
template <typename FixtureClass, typename Range, typename Gen>
auto register_instantiation(std::string_view instantiation, std::string_view fixture_key, const Range& range,
                            Gen generator) -> void {
    using Value = typename FixtureClass::ParamType;
    auto& values = param_values<FixtureClass>();

    ParamInstantiation<Value> inst;
    inst.prefix = std::string{instantiation};
    inst.name_of = [generator](const Value& value) -> std::string { return generator(value); };
    inst.offset = values.size();
    for (const auto& item : range) {
        values.push_back(static_cast<Value>(item));
        ++inst.count;
    }
    expand_param_families<FixtureClass>(fixture_key, inst);
}

/// @brief 类型参数化：为一个类型清单逐项注册用例（BodyHolder 形如 `template <typename> class`）。
template <typename List, template <typename> class BodyHolder, std::size_t... Is>
auto register_typed_cases_impl(std::string_view suite, std::string_view case_name, const char* file, int line,
                               std::index_sequence<Is...>) -> void {
    (TestRegistry::instance().add_dynamic(
         suite, std::string{case_name} + "/" + short_type_name<std::tuple_element_t<Is, typename List::Tuple>>(),
         &BodyHolder<std::tuple_element_t<Is, typename List::Tuple>>::body, file, line),
     ...);
}

template <typename List, template <typename> class BodyHolder>
auto register_typed_cases(std::string_view suite, std::string_view case_name, const char* file, int line) -> void {
    register_typed_cases_impl<List, BodyHolder>(suite, case_name, file, line,
                                                std::make_index_sequence<std::tuple_size_v<typename List::Tuple>>{});
}

}  // namespace detail

/// @brief 取值表（字面量形态，同 GoogleTest 的 `Values(...)`）：元素类型取 `common_type`。
///
/// 用函数而非花括号：宏实参里写 `{1, 2}` 会被预处理器切成多个实参。
template <typename... Values>
[[nodiscard]] auto values_of(Values... items) -> std::vector<std::common_type_t<Values...>> {
    return std::vector<std::common_type_t<Values...>>{std::forward<Values>(items)...};
}

/// @brief 取值表（容器 / 区间形态）。
template <typename Range>
[[nodiscard]] auto values_in(const Range& range) -> std::vector<std::decay_t<decltype(*std::begin(range))>> {
    using Value = std::decay_t<decltype(*std::begin(range))>;
    return std::vector<Value>{std::begin(range), std::end(range)};
}

}  // namespace aurora::testing

// ---------------------------------------------------------------------------
// 宏层
// ---------------------------------------------------------------------------

/// @brief 注册一个值参数化用例（对标 `TEST_P`）：体内用 `param()` 取当次取值。
///
/// `fixture_class` 必须派生自 `aurora::testing::TestWithParam<V>`；生成的用例体是
/// `CaseBase<fixture_class>` 的成员函数，故 fixture 的 protected 成员同样直接可见。
#define AURORA_TEST_P(fixture_class, case_name)                                                                \
    class aurora_test_param_##fixture_class##_##case_name                                                      \
        : public ::aurora::testing::detail::CaseBase<fixture_class> {                                          \
      protected:                                                                                               \
        auto case_body() -> void override;                                                                     \
    };                                                                                                         \
    namespace {                                                                                                \
    auto aurora_test_param_run_##fixture_class##_##case_name(std::size_t index) -> void {                      \
        ::aurora::testing::detail::set_current_param<fixture_class>(                                           \
            ::aurora::testing::detail::param_at<fixture_class>(index));                                        \
        ::aurora::testing::detail::run_case_instance<aurora_test_param_##fixture_class##_##case_name>();       \
    }                                                                                                          \
    const ::aurora::testing::detail::ParamFamilyRegistrar aurora_test_param_reg_##fixture_class##_##case_name{ \
        ::aurora::testing::suite_from_path(__FILE__),        #fixture_class, #case_name, __FILE__, __LINE__,   \
        &aurora_test_param_run_##fixture_class##_##case_name};                                                 \
    }                                                                                                          \
    auto aurora_test_param_##fixture_class##_##case_name::case_body() -> void

/// @brief 实例化一个值参数化 fixture（对标 `INSTANTIATE_TEST_SUITE_P`）。
///
/// 第三实参须是单一表达式（`values_of(...)` / `values_in(container)` / 具名容器）。
#define AURORA_INSTANTIATE_TEST_SUITE_P(instantiation, fixture_class, ...)         \
    AURORA_INSTANTIATE_TEST_SUITE_P_GEN(instantiation, fixture_class, __VA_ARGS__, \
                                        ::aurora::testing::detail::DefaultParamName{})

/// @brief 带名字生成器的实例化：生成器须能以 `std::string(const ParamType&)` 调用。
#define AURORA_INSTANTIATE_TEST_SUITE_P_GEN(instantiation, fixture_class, values, generator)                          \
    namespace {                                                                                                       \
    auto aurora_test_instantiate_##instantiation##_##fixture_class() -> void {                                        \
        ::aurora::testing::detail::register_instantiation<fixture_class>(#instantiation, #fixture_class, values,      \
                                                                         generator);                                  \
    }                                                                                                                 \
    const ::aurora::testing::detail::FinalizeRegistrar aurora_test_instantiate_reg_##instantiation##_##fixture_class{ \
        &aurora_test_instantiate_##instantiation##_##fixture_class};                                                  \
    }

/// @brief 声明一个类型参数化套件（对标 `TYPED_TEST_SUITE`）：`suite_name` 须是类模板名。
///
/// 用法：`AURORA_TYPED_TEST_SUITE(numeric_fixture, int, float, double)`。
#define AURORA_TYPED_TEST_SUITE(suite_name, ...) \
    using aurora_typed_list_##suite_name = ::aurora::testing::TypeList<__VA_ARGS__>

/// @brief 注册一个类型参数化用例（对标 `TYPED_TEST`）：体内 `TestType` 即当前类型。
///
/// `suite_name` 须是「以单个类型为模板参数、派生自 `aurora::testing::Fixture`」的类模板；
/// 生成的用例体是该 fixture 派生类的成员函数，故其 protected 成员直接可见。
/// ⚠️ fixture 是本类的**依赖基**，故体内引用其成员须写 `this->member_`。
#define AURORA_TYPED_TEST(suite_name, case_name)                                                                    \
    template <typename TestType>                                                                                    \
    class aurora_typed_case_##suite_name##_##case_name                                                              \
        : public ::aurora::testing::detail::CaseBase<suite_name<TestType>> {                                        \
      protected:                                                                                                    \
        auto case_body() -> void override;                                                                          \
    };                                                                                                              \
    template <typename TestType>                                                                                    \
    class aurora_typed_holder_##suite_name##_##case_name {                                                          \
      public:                                                                                                       \
        static auto body() -> void {                                                                                \
            ::aurora::testing::detail::run_case_instance<aurora_typed_case_##suite_name##_##case_name<TestType>>(); \
        }                                                                                                           \
    };                                                                                                              \
    namespace {                                                                                                     \
    auto aurora_typed_expand_##suite_name##_##case_name() -> void {                                                 \
        ::aurora::testing::detail::register_typed_cases<aurora_typed_list_##suite_name,                             \
                                                        aurora_typed_holder_##suite_name##_##case_name>(            \
            ::aurora::testing::suite_from_path(__FILE__), #case_name, __FILE__, __LINE__);                          \
    }                                                                                                               \
    const ::aurora::testing::detail::FinalizeRegistrar aurora_typed_hook_##suite_name##_##case_name{                \
        &aurora_typed_expand_##suite_name##_##case_name};                                                           \
    }                                                                                                               \
    template <typename TestType>                                                                                    \
    auto aurora_typed_case_##suite_name##_##case_name<TestType>::case_body() -> void
