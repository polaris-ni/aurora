// Diagnostics::FixSuggestion 验证：上报携带修复建议的诊断，collect_fixes/apply_fix 正确工作。
// ── API 覆盖映射 ─────────────────────────────
// core/diagnostics.h（Diagnostics::FixSuggestion / collect_fixes / apply_fix）。

#include <iostream>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Diagnostics;
using aurora::FixSuggestion;

AURORA_TEST() {
    Diagnostics::take(); // 清空累计

    bool fixed = false;
    FixSuggestion fix;
    fix.code = "demo-fix-001";
    fix.description = "重置导航栈以恢复";
    fix.auto_fix = [&]() -> void { fixed = true; };

    Diagnostics::report("触发可自动修复的降级", "test_diagnostics_fix", "demo-fix-001", true, std::move(fix));

    // collect_fixes 应包含该修复
    const auto fixes = Diagnostics::collect_fixes();
    bool found = false;
    for (const auto &f : fixes) {
        if (f.code == "demo-fix-001") {
            found = true;
            AURORA_TEST_CHECK_MSG(f.has_auto_fix(), "collected fix carries auto_fix");
            AURORA_TEST_CHECK_MSG(f.description == "重置导航栈以恢复", "collected fix description preserved");
        }
    }
    AURORA_TEST_CHECK_MSG(found, "collect_fixes includes reported fix");

    // 未应用前 fixed 为 false
    AURORA_TEST_CHECK_MSG(!fixed, "auto_fix not run before apply_fix");

    // apply_fix 命中并执行
    AURORA_TEST_CHECK_MSG(Diagnostics::apply_fix("demo-fix-001"), "apply_fix returns true for known code");
    AURORA_TEST_CHECK_MSG(fixed, "apply_fix ran auto_fix (flag set)");

    // 未知 code 返回 false
    AURORA_TEST_CHECK_MSG(!Diagnostics::apply_fix("no-such-code"), "apply_fix returns false for unknown code");

    // take 取出后诊断累计被清空，但最近环形缓冲 m_recent 保留（explain/复查语义），
    // 故 apply_fix 仍可命中最近诊断。
    Diagnostics::take();
    AURORA_TEST_CHECK_MSG(Diagnostics::apply_fix("demo-fix-001"),
                          "recent buffer retains fix after take(), apply still works");
    // collect_fixes 仍可见
    const auto fixes2 = Diagnostics::collect_fixes();
    bool still_visible = false;
    for (const auto &f : fixes2) {
        if (f.code == "demo-fix-001") {
            still_visible = true;
        }
    }
    AURORA_TEST_CHECK_MSG(still_visible, "recent buffer retains fix after take() (explain/review semantics)");
}
