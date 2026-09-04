// test_debug_runtime.cpp — 运行时信息导出验证。
//
// 覆盖：
//  1) widget_tree 返回 Widget 树 JSON（DEBUG 含 "type"；Release available=false）。
//  2) perf_snapshot 聚合 FrameStats + PerfLog（DEBUG 含 fps/avg_frame_ms/perf_log 等）。
//  3) frame_phase_timeline 复用 FrameStats 相位环形缓冲（DEBUG 含 avg_*_ms/flamegraph/recent_frame_ms）。
//  4) diagnostics 薄封装 Diagnostics（DEBUG 含 count/diagnostics 数组）。
//  5) why_trace 热路径埋点（DEBUG 下 mark_needs_layout/paint 触发记录，区分根因 propagated=false
//     与父链传播 propagated=true；Release available=false，热路径未记录）。
//
// 宏一致约定：测试 TU 与 aurora 库同配置获得 AURORA_ENABLE_DEBUG；success-path 断言用
// #ifdef AURORA_ENABLE_DEBUG 分支，与库体编译分支对齐。

// ── API 覆盖映射 ─────────────────────────────
// debug/debug_trace.h(aurora::debug::detail::record_dirty 为内部命名空间，非对外承诺 API；
//   DirtyKind 经 debug_runtime 的 why_trace 输出间接行使)、perf/perf_log.h(经 perf_snapshot 快照路径行使)。

#include <memory>

#include "aurora/aurora.h"
#include "aurora/test_helpers.h"
#include "test_harness.h"

using aurora::Color;
using aurora::Column;
using aurora::Json;
using aurora::Modifier;
using aurora::Node;
using aurora::px;
using aurora::debug::diagnostics;
using aurora::debug::frame_phase_timeline;
using aurora::debug::perf_snapshot;
using aurora::debug::why_trace;
using aurora::debug::widget_tree;
using aurora::test::init_headless;
using aurora::test::TestEnv;

AURORA_TEST() {
    // ---- 1. widget_tree ----
    {
        TestEnv env = init_headless(200, 200);
        auto a = std::make_shared<Column>();
        a->width(px(80.0F));
        a->height(px(40.0F));
        a->modifier.set(Modifier{}.background(Color(200, 200, 200, 255)));
        env.root_widget->add(Node{a});
        pump(env);

        Json tree = widget_tree(env.root);
#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(tree.contains("type"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tree["type"].is_string());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tree["type"] == "Column");
#else
        AURORA_TEST_CHECK(tree.contains("available"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 调试关闭分支：Json operator[]
        // 用于读取键，非下标索引，at() 语义不符
        AURORA_TEST_CHECK(!tree["available"].get<bool>());
#endif
    }

    // ---- 2. perf_snapshot ----
    {
        Json ps = perf_snapshot();
#ifdef AURORA_ENABLE_DEBUG
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ps.contains("fps") && ps["fps"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ps.contains("avg_frame_ms") && ps["avg_frame_ms"].is_number());
        AURORA_TEST_CHECK(ps.contains("worst_frame_ms"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ps.contains("dropped_frames") && ps["dropped_frames"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ps.contains("hitches") && ps["hitches"].is_number());
        AURORA_TEST_CHECK(ps.contains("perf_log"));
#else
        AURORA_TEST_CHECK(ps.contains("available"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 调试关闭分支：Json operator[]
        // 用于读取键，非下标索引，at() 语义不符
        AURORA_TEST_CHECK(!ps["available"].get<bool>());
#endif
    }

    // ---- 3. frame_phase_timeline ----
    {
        Json tl = frame_phase_timeline();
#ifdef AURORA_ENABLE_DEBUG
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("avg_layout_ms") && tl["avg_layout_ms"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("avg_paint_ms") && tl["avg_paint_ms"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("avg_present_ms") && tl["avg_present_ms"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("fps") && tl["fps"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("flamegraph") && tl["flamegraph"].is_string());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tl.contains("recent_frame_ms") && tl["recent_frame_ms"].is_array());
#else
        AURORA_TEST_CHECK(tl.contains("available"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 调试关闭分支：Json operator[]
        // 用于读取键，非下标索引，at() 语义不符
        AURORA_TEST_CHECK(!tl["available"].get<bool>());
#endif
    }

    // ---- 4. diagnostics ----
    {
        Json dg = diagnostics();
#ifdef AURORA_ENABLE_DEBUG
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(dg.contains("count") && dg["count"].is_number());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(dg.contains("diagnostics") && dg["diagnostics"].is_array());
#else
        AURORA_TEST_CHECK(dg.contains("available"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 调试关闭分支：Json operator[]
        // 用于读取键，非下标索引，at() 语义不符
        AURORA_TEST_CHECK(!dg["available"].get<bool>());
#endif
    }

    // ---- 5. why_trace（热路径埋点 + propagated 区分）----
    {
        TestEnv env = init_headless(200, 200);
        auto child = std::make_shared<Column>();  // 非 relayout boundary（WrapContent）
        env.root_widget->add(Node{child});
        // 显式建立父链（pump 的布局入口也会登记，此处覆盖确保确定性）：
        // child → root；root 自身无 parent，传播到 root 记 propagated=true 后截断。
        child->set_layout_parent(env.root_widget.get());
        pump(env);

        // 对 child 触发：child 自身为根因（propagated=false），并沿父链传播至 root
        // （root 无 parent，记录 propagated=true 后截断）。
        child->mark_needs_layout();
        child->mark_needs_paint();

        Json wt = why_trace();
#ifdef AURORA_ENABLE_DEBUG
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(wt.contains("entries") && wt["entries"].is_array());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(wt.contains("total_recorded") && wt["total_recorded"].is_number());
        bool saw_root_cause = false;  // layout + propagated==false
        bool saw_propagated = false;  // layout + propagated==true（父链冒泡）
        bool saw_paint = false;  // kind==paint
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        for (const Json &e : wt["entries"]) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (e["kind"] == "layout" && e["propagated"] == false) {
                saw_root_cause = true;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (e["kind"] == "layout" && e["propagated"] == true) {
                saw_propagated = true;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (e["kind"] == "paint") {
                saw_paint = true;
            }
        }
        AURORA_TEST_CHECK(saw_root_cause);
        AURORA_TEST_CHECK(saw_propagated);
        AURORA_TEST_CHECK(saw_paint);
#else
        AURORA_TEST_CHECK(wt.contains("available"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 调试关闭分支：Json operator[]
        // 用于读取键，非下标索引，at() 语义不符
        AURORA_TEST_CHECK(!wt["available"].get<bool>());
#endif
    }
}