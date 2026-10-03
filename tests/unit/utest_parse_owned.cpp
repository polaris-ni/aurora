/// 测试类型: unit
/// 目标单元: include/aurora/cli/args.h（parse_owned / Arguments::adopt_spec）
/// 测试说明: 覆盖「声明树以临时量形态存在时借用型 parse 会悬空、拥有型 parse_owned 不会」这一
/// 契约分界，以及拥有型的三条附带保证：重指向按**身份路径**而非同名反查、matched_command 落到
/// 副本内、--help 短路路径不因升级而失效。判据刻意针对「悬空后的静默错误值」而非仅断言不崩。

#include <string>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_parse_owned {

namespace cli = aurora::cli;

/// @brief 声明树以**函数返回的临时量**形态给出——这正是悬空的触发条件。
///
/// 为什么必须用这个形态而非局部变量：`parse` 的契约是「root 比返回的 Invocation 活得久」，
/// 局部变量天然满足，测它恒绿、毫无鉴别力。真实缺陷就出在调用方把声明表写成返回值时。
[[nodiscard]] inline auto temporary_spec() -> cli::CommandSpec {
    return cli::CommandSpec{
        .name = "probe",
        .about = "parse-owned lifetime probe",
        .options = {cli::OptionSchema{.long_name = "interactive",
                                      .kind = cli::ValueKind::Bool,
                                      .arity = cli::Arity::flag(),
                                      .help = "flag under test"},
                    cli::OptionSchema{.long_name = "level", .kind = cli::ValueKind::Int, .help = "value under test"}},
        .subcommands = {cli::CommandSpec{.name = "child",
                                         .options = {cli::OptionSchema{.long_name = "interactive",
                                                                       .kind = cli::ValueKind::Bool,
                                                                       .arity = cli::Arity::flag(),
                                                                       .help = "same long name, other level"}}}}};
}

/// @brief 把 token 序列包成 argc/argv 形态，走 C 入口重载。
struct Argv {
    std::vector<std::string> storage;
    std::vector<const char *> pointers;

    /// @brief 以程序名 + 给定 token 构造 argv。
    /// @param tokens 不含程序名的参数序列。
    explicit Argv(std::vector<std::string> tokens) {
        storage.push_back("probe");
        for (auto &token : tokens) {
            storage.push_back(std::move(token));
        }
        for (const auto &item : storage) {
            pointers.push_back(item.c_str());
        }
    }

    /// @brief argv 元素个数。
    [[nodiscard]] auto argc() const -> int { return static_cast<int>(pointers.size()); }
    /// @brief argv 指针数组。
    [[nodiscard]] auto argv() const -> const char *const * { return pointers.data(); }
};

/// @brief 给「活对象」对照提供长寿命声明树。
[[nodiscard]] inline auto live_reference_spec() -> const cli::CommandSpec & {
    static const cli::CommandSpec value = temporary_spec();
    return value;
}

AURORA_TEST_CASE(owned_parse_reads_flag_correctly_when_spec_is_temporary) {
    // 借用型对照组：形态合法但契约被违反，`flag()` 静默恒错。不断言其具体值——
    // 那是 UB 的表现、随实现与分配器而变；此处只固定「它不是可靠来源」这一事实的前提。
    const Argv args{{"--interactive", "--level", "3"}};
    const auto borrowed = cli::parse(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(borrowed);

    // 拥有型：同一 token 序列、同一临时量形态，flag 与值类型必须都读对。
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);
    AURORA_TEST_CHECK(owned.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.count("interactive"), 1);
    AURORA_TEST_CHECK(owned.value().arguments.explicitly_given("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.get<int>("level").unwrap(), 3);
}

AURORA_TEST_CASE(owned_flag_does_not_depend_on_stale_memory_happening_to_survive) {
    // 「悬空读」有个陷阱：它**有时**会碰巧读对——`spec` 销毁后那块堆内存若还没被复用，
    // `find_slot` 按 `long_name` 匹配仍可能命中，于是判据变成靠运气（实测：关闭 rebind 后
    // 「多次重读 + 分配搅扰」那条仍全绿）。故本例不与悬空读对照，而是**直接验证重指向这件事
    // 本身可观测**：拥有型结果里读出的选项，其声明地址必须落在 owned 副本内——
    // 借用型读出的是已销毁的原对象地址，两者必然不同。
    const Argv args{{"--interactive", "--level", "3"}};
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);

    // 值读数正确（这一条与地址判据互补：地址对但值错、或值对但地址错，都要能发现）。
    AURORA_TEST_CHECK(owned.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.get<int>("level").unwrap(), 3);

    // 命令链与位置参数在重指向后仍自洽。
    AURORA_TEST_CHECK_EQ(owned.value().arguments.command_display(), std::string{"probe"});
    AURORA_TEST_CHECK_EQ(owned.value().arguments.positionals().size(), std::size_t{0});
}

AURORA_TEST_CASE(owned_parse_reports_absent_flag_as_false) {
    // 反向：未给出的旗标必须仍报 false。若 rebind 绑错（例如按同名子命令选项反查），
    // 根层 `interactive` 会被子命令那个同名项串味，读出错误的 true。
    const Argv args{{"--level", "1"}};
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);
    AURORA_TEST_CHECK(!owned.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.count("interactive"), 0);
}

AURORA_TEST_CASE(owned_rebind_targets_the_right_level_by_identity_not_by_name) {
    // 根与子命令**都**声明了 `--interactive`。若 rebind 按 long_name 搜索，
    // 子命令槽位会被绑到根层那份（或反之），而 `count` / 值类型随之失真。
    // 这里命中子命令后，根层那个同名字段必须仍报「未给出」。
    const Argv args(std::vector<std::string>{"child", "--interactive"});
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);
    AURORA_TEST_CHECK(owned.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.count("interactive"), 1);
}

AURORA_TEST_CASE(owned_matched_command_points_into_the_copy_and_stays_readable) {
    const Argv args(std::vector<std::string>{"child", "--interactive"});
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);
    const cli::CommandSpec *const leaf = owned.value().arguments.matched_command();
    AURORA_TEST_REQUIRE(leaf != nullptr);
    AURORA_TEST_CHECK_EQ(leaf->name, std::string{"child"});
    // 叶子节点的选项也须经由副本可读（其 options 的 spec 指针被重指向了）。
    AURORA_TEST_CHECK(!leaf->options.empty());
    AURORA_TEST_CHECK_EQ(leaf->options[0].long_name, std::string{"interactive"});
}

AURORA_TEST_CASE(owned_parse_keeps_the_borrowing_contract_for_long_lived_specs) {
    // 不回归既有语义：声明树是活对象时，`parse_owned` 与 `parse` 的读数必须一致，
    // 且 `matched_command` 在 `parse` 路径上仍返回调用方原对象的地址。
    const cli::CommandSpec &live = live_reference_spec();
    const Argv args{{"--interactive", "--level", "5"}};

    const auto borrowed = cli::parse(live, args.argc(), args.argv());
    const auto owned = cli::parse_owned(live, args.argc(), args.argv());
    AURORA_TEST_REQUIRE(borrowed);
    AURORA_TEST_REQUIRE(owned);

    AURORA_TEST_CHECK(borrowed.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK(owned.value().arguments.flag("interactive"));
    AURORA_TEST_CHECK_EQ(owned.value().arguments.get<int>("level").unwrap(),
                         borrowed.value().arguments.get<int>("level").unwrap());
    // 借用路径的指针身份契约不变：仍等于调用方对象地址。
    AURORA_TEST_CHECK(borrowed.value().arguments.matched_command() == &live);
}

AURORA_TEST_CASE(owned_parse_preserves_the_display_short_circuit) {
    // --help 走 early_view 短路：不进业务、不建槽位。升级路径不得把它变成普通成功结果。
    const Argv args{{"--help"}};
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    AURORA_TEST_REQUIRE(owned);
    AURORA_TEST_CHECK(owned.value().shows_display());
    AURORA_TEST_CHECK(owned.value().view == cli::EarlyView::Help);
    AURORA_TEST_CHECK(!owned.value().display_text.empty());
}

AURORA_TEST_CASE(owned_parse_propagates_usage_errors_unchanged) {
    // 错误路径不产生 Invocation，错误码须与借用型同源（否则调用方按码分流的脚本会错判）。
    const Argv args{{"--nope"}};
    const auto owned = cli::parse_owned(temporary_spec(), args.argc(), args.argv());
    const auto borrowed = cli::parse(live_reference_spec(), args.argc(), args.argv());
    AURORA_TEST_CHECK(!owned);
    AURORA_TEST_CHECK(!borrowed);
    AURORA_TEST_CHECK_EQ(owned.error().code_enum, borrowed.error().code_enum);
}

}  // namespace aurora::test_cases::utest_parse_owned
