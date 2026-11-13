/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/recent_documents_translate.h
/// 测试说明: 最近文档 OS 投影的**零平台头折算层**与它两侧的契约——投影计划的三条折算规则（丢空串、
/// 去重保持首次出现序、按平台上限截断且新→旧优先）、路径口径（相对路径与不存在的路径照进计划，
/// 与「空串被丢弃」是两条不同处置）、能力位矩阵（逐 `SurfaceKind` 申报 `recent_documents_os_projection`），
/// 以及经进程级通路接缝驱动**公共入口本身**（`RecentDocuments::project_to_os` / `clear_os_projection`）的
/// 三条契约：显式性（`add`/`clear` 不隐式推送）、推送量等于计划条目数、失败隔离（桥报错不动应用内列表、
/// 错误码不与「能力缺位」混淆）。
///
/// 为何单测能跑产品代码路径：投影通路是折算层持有的进程级单槽（`ProjectionBackend`），无头侧由本文件
/// 注入伪造后端、真机侧由平台桥注入同名后端——两侧走的是 `project_to_os()` 的同一条代码，被测的不是
/// 它的复刻。这也是能力位与运行期判据分开的收益：位是编译期申报，通路是否在才是运行期事实。
///
/// 本文件**不触碰真实 OS**：伪造后端一经登记就被 `active_projection_backend()` 直接采用，Win32 腿上
/// 也就不会走到按需自举那枚真桥。「无后端 ⇒ 报不支持」那条只在没有可自举桥的腿上判（其余腿如实 SKIP，
/// 判它会真往用户的「最近使用」里写一条）。
///
/// 变异自证（改坏实现应转红的六条）：
///  * 把截断方向改成保留最旧（从头部丢）→ `plan_truncates_from_the_oldest_tail` 转红；
///  * 摘掉空串过滤 → `plan_drops_empty_entries` 转红（OS 侧多出一条无主记录）；
///  * 把去重改成保最后一次出现序或干脆不去重 → `plan_dedupes_keeping_first_occurrence` 转红；
///  * 让 `add` 自动投影 → `add_and_clear_never_project_implicitly` 转红（投影是有外部副作用的动作）；
///  * 让投影失败时回滚内存列表 → `backend_failure_leaves_the_app_list_untouched` 转红；
///  * 为未落桥的方向（macOS / Linux 三形态）翻能力位 → `capability_bit_is_declared_only_for_wired_surfaces`
///    转红（「机制在」的谎报会让调用方在静默失败时丢掉降级路径）。

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/app/recent_documents.h"
#include "aurora/core/result.h"
#include "aurora/window/detail/recent_documents_translate.h"
#include "aurora/window/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_recent_documents_translate {

using recent_documents::build_projection_plan;
using recent_documents::ProjectionBackend;

namespace {

/// 用例隔离：通路是进程级单槽，用例进出各清一次，避免残留流向其它套件（与 store 的 `clear_all` 同纪律）。
class BackendGuard {
  public:
    BackendGuard() { recent_documents::unregister_projection_backend(); }
    ~BackendGuard() { recent_documents::unregister_projection_backend(); }
    BackendGuard(const BackendGuard &) = delete;
    auto operator=(const BackendGuard &) -> BackendGuard & = delete;
    BackendGuard(BackendGuard &&) = delete;
    auto operator=(BackendGuard &&) -> BackendGuard & = delete;
};

/// @brief 伪造 OS 后端的观测状态（进程级单槽后端只能持有一枚，观测器随之进程级）。
struct FakeOs {
    std::vector<std::vector<std::string>> pushes;  ///< 每次 project 调用收到的计划（按调用序）
    std::size_t clear_calls = 0;  ///< clear 被调用的次数
};

/// 进程级观测器（lambda 侧经本函数取用，不捕获静态对象）。
auto fake_os() -> FakeOs & {
    static FakeOs observed;
    return observed;
}

/// @brief 登记一枚伪造后端并复位观测器。
/// @param max_entries 该「平台」申报的条目上限（默认 3，够触发截断）。
/// @param fail_project 为 true 时 `project` 返回平台侧结构化错误（`platform-unavailable`），
///        用于验证「真实故障」与「能力缺位」两枚码不混淆。
/// @return 观测器引用。
auto install_fake_os(std::size_t max_entries = 3, bool fail_project = false) -> FakeOs & {
    fake_os() = FakeOs{};
    recent_documents::register_projection_backend(ProjectionBackend{
        .max_entries = max_entries,
        .project = [fail_project](const std::vector<std::string> &plan) -> Result<void> {
            if (fail_project) {
                return make_error(ErrorCode::PlatformUnavailable, "fake os: projection failed");
            }
            fake_os().pushes.emplace_back(plan);
            return Result<void>{};
        },
        .clear = []() -> Result<void> {
            ++fake_os().clear_calls;
            return Result<void>{};
        },
    });
    return fake_os();
}

/// 用例隔离：store 是进程级单例，复位后再交回调用方。
auto fresh_store() -> RecentDocuments & {
    auto &store = RecentDocuments::instance();
    store.clear_all();
    return store;
}

/// 累计推送条目数（显式性判据：0 表示从未隐式触发）。
auto pushed_entry_count(const FakeOs &observed) -> std::size_t {
    std::size_t total = 0;
    for (const auto &plan : observed.pushes) {
        total += plan.size();
    }
    return total;
}

}  // namespace

// ---------- 折算层纯逻辑 ----------

AURORA_TEST_CASE(plan_drops_empty_entries) {
    const std::vector<std::string> documents{"", "docs/alpha.txt", "", "docs/beta.txt", ""};
    const auto plan = build_projection_plan(documents, 10);
    AURORA_TEST_CHECK_EQ(plan.size(), std::size_t{2});  // 变异自证：摘掉空串过滤即转红
    AURORA_TEST_CHECK_STREQ(plan[0], "docs/alpha.txt");
    AURORA_TEST_CHECK_STREQ(plan[1], "docs/beta.txt");
}

AURORA_TEST_CASE(plan_truncates_from_the_oldest_tail) {
    // 入参是新 → 旧序（与 `RecentDocuments::list()` 同口径），超上限时**尾部（最旧）先丢**。
    const std::vector<std::string> documents{"newest.txt", "middle.txt", "older.txt", "oldest.txt"};
    const auto plan = build_projection_plan(documents, 2);
    AURORA_TEST_CHECK_EQ(plan.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(plan[0], "newest.txt");
    AURORA_TEST_CHECK_STREQ(plan[1], "middle.txt");  // 变异自证：从头部丢（保留最旧）即转红
}

AURORA_TEST_CASE(plan_dedupes_keeping_first_occurrence) {
    const std::vector<std::string> documents{"a.txt", "b.txt", "a.txt", "c.txt", "b.txt"};
    const auto plan = build_projection_plan(documents, 10);
    AURORA_TEST_CHECK_EQ(plan.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(plan[0], "a.txt");  // 变异自证：保最后一次出现序会排成 a→c→b
    AURORA_TEST_CHECK_STREQ(plan[1], "b.txt");
    AURORA_TEST_CHECK_STREQ(plan[2], "c.txt");
}

AURORA_TEST_CASE(empty_list_yields_empty_plan_without_error) {
    const std::vector<std::string> empty;
    const auto plan = build_projection_plan(empty, 5);
    AURORA_TEST_CHECK(plan.empty());  // 空列表是合法输入，产出空计划而非错误
    const std::vector<std::string> one{"a.txt"};
    AURORA_TEST_CHECK_EQ(build_projection_plan(one, 0).size(), std::size_t{0});  // 上限 0 ⇒ 空计划
}

AURORA_TEST_CASE(relative_and_absent_paths_enter_the_plan_unchanged) {
    // 路径口径：折算层不规范化、不校验存在性（与 store 的字符串级口径同源）。
    const std::vector<std::string> documents{"../up_one/notes.txt", "Z:/no/such/place.bin", "a\\b\\c.txt"};
    const auto plan = build_projection_plan(documents, 10);
    AURORA_TEST_CHECK_EQ(plan.size(), std::size_t{3});  // 三条全部照进：相对路径与不存在的路径都不被剔除
    AURORA_TEST_CHECK_STREQ(plan[0], "../up_one/notes.txt");
    AURORA_TEST_CHECK_STREQ(plan[1], "Z:/no/such/place.bin");
    AURORA_TEST_CHECK_STREQ(plan[2], "a\\b\\c.txt");  // 分隔符原样透传，不做平台改写
}

// ---------- 能力位矩阵 ----------

AURORA_TEST_CASE(capability_bit_is_declared_only_for_wired_surfaces) {
    // 逐 SurfaceKind 申报「OS 侧有该机制且本库已落桥」。翻位纪律：桥未落地的方向不预先声称支持
    // ——`MacOS` 已落 `NSDocumentController` 桥（`noteNewRecentDocumentURL:`）故为 true，
    // Linux 三形态恒 false（OS 无该机制）。
    struct Expectation {
        SurfaceKind kind;
        bool expected;
    };
    const Expectation table[] = {
        {.kind = SurfaceKind::Headless, .expected = false}, {.kind = SurfaceKind::Win32, .expected = true},
        {.kind = SurfaceKind::D3D11, .expected = true},     {.kind = SurfaceKind::Glfw, .expected = false},
        {.kind = SurfaceKind::X11, .expected = false},      {.kind = SurfaceKind::Wayland, .expected = false},
        {.kind = SurfaceKind::MacOS, .expected = true},     {.kind = SurfaceKind::Wasm, .expected = false},
    };
    for (const Expectation &row : table) {
        Platform p;
        p.surface = row.kind;
        const auto caps = p.capabilities();
        AURORA_TEST_CHECK_EQ(static_cast<int>(caps.recent_documents_os_projection), static_cast<int>(row.expected));
    }
}

// ---------- 公共入口的三条契约（经进程级通路接缝驱动产品代码本身） ----------

AURORA_TEST_CASE(add_and_clear_never_project_implicitly) {
    const BackendGuard guard;
    auto &observed = install_fake_os();
    auto &store = fresh_store();

    store.add("docs/alpha.txt");
    store.add("docs/beta.txt");
    store.clear();
    AURORA_TEST_CHECK_EQ(pushed_entry_count(observed), std::size_t{0});  // 变异自证：add 自动投影即转红
    AURORA_TEST_CHECK_EQ(observed.clear_calls, std::size_t{0});  // store 的 clear 也不碰 OS 侧
}

AURORA_TEST_CASE(project_to_os_pushes_exactly_the_plan) {
    const BackendGuard guard;
    auto &observed = install_fake_os(/*max_entries=*/2);
    auto &store = fresh_store();
    store.add("docs/oldest.txt");
    store.add("docs/middle.txt");
    store.add("docs/newest.txt");

    const auto result = store.project_to_os();
    AURORA_TEST_CHECK(result.ok());
    AURORA_TEST_CHECK_EQ(observed.pushes.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(observed.pushes[0].size(), std::size_t{2});  // 上限 2 ⇒ 截断到最新两条
    AURORA_TEST_CHECK_STREQ(observed.pushes[0][0], "docs/newest.txt");
    AURORA_TEST_CHECK_STREQ(observed.pushes[0][1], "docs/middle.txt");

    // 幂等：同一列表再投影一次，投递的是同一份计划（OS 侧去重由桥负责，本层保证不自己加条）。
    AURORA_TEST_CHECK(store.project_to_os().ok());
    AURORA_TEST_CHECK_EQ(observed.pushes.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(observed.pushes[1].size(), observed.pushes[0].size());
    AURORA_TEST_CHECK_STREQ(observed.pushes[1][0], observed.pushes[0][0]);
    AURORA_TEST_CHECK_STREQ(observed.pushes[1][1], observed.pushes[0][1]);
}

AURORA_TEST_CASE(empty_store_projection_is_a_success_noop) {
    const BackendGuard guard;
    auto &observed = install_fake_os();
    auto &store = fresh_store();

    AURORA_TEST_CHECK(store.project_to_os().ok());
    AURORA_TEST_CHECK_EQ(observed.pushes.size(), std::size_t{0});  // 空计划不打扰 OS，也不是错误
}

AURORA_TEST_CASE(backend_failure_leaves_the_app_list_untouched) {
    const BackendGuard guard;
    install_fake_os(/*max_entries=*/3, /*fail_project=*/true);
    auto &store = fresh_store();
    store.add("docs/alpha.txt");
    store.add("docs/beta.txt");

    const auto result = store.project_to_os();
    AURORA_TEST_CHECK_FALSE(result.ok());
    // 失败隔离：投影只是「没推出去」，`add` 已成立的内存列表与容量口径不变。
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{2});
    const auto items = store.list();
    AURORA_TEST_CHECK_STREQ(items.front(), "docs/beta.txt");
    // 错误码不混淆：平台侧真实故障是 `platform-unavailable`，不是「能力缺位」那枚。
    AURORA_TEST_CHECK_NE(result.error().code_enum, ErrorCode::RecentDocumentsProjectionUnsupported);
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::PlatformUnavailable);
}

AURORA_TEST_CASE(clear_os_projection_calls_the_backend_clear) {
    const BackendGuard guard;
    auto &observed = install_fake_os();
    auto &store = fresh_store();
    store.add("docs/alpha.txt");
    AURORA_TEST_CHECK(store.project_to_os().ok());
    AURORA_TEST_CHECK(store.clear_os_projection().ok());
    AURORA_TEST_CHECK_EQ(observed.clear_calls, std::size_t{1});
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});  // 撤销 OS 侧记录不动应用内列表
}

AURORA_TEST_CASE(no_backend_reports_projection_unsupported) {
    const BackendGuard guard;
    recent_documents::unregister_projection_backend();
    if (platform().capabilities().recent_documents_os_projection) {
        // Win32 / D3D11 腿上通路会按需自举（这正是该腿该有的行为），此处判「无通路」就得把真桥
        // 拉起来，而那是往用户的「最近使用」里写一条——单测不碰 OS，如实跳过。
        AURORA_TEST_SKIP(
            "this leg bootstraps the real win32 bridge; unit cases must not write to the user's OS "
            "recent-documents list, the no-backend verdict runs on legs without a bridge");
    }
    auto &store = fresh_store();
    store.add("docs/alpha.txt");
    const auto projected = store.project_to_os();
    const auto cleared = store.clear_os_projection();
    AURORA_TEST_CHECK_FALSE(projected.ok());
    AURORA_TEST_CHECK_EQ(projected.error().code_enum, ErrorCode::RecentDocumentsProjectionUnsupported);
    AURORA_TEST_CHECK_EQ(cleared.error().code_enum, ErrorCode::RecentDocumentsProjectionUnsupported);
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});  // 应用内列表照旧
}

}  // namespace aurora::test_cases::utest_recent_documents_translate
