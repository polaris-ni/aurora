// test_lifecycle.cpp — 覆盖声明式挂载/卸载控件 `au::Lifecycle`。
// 关注点：on_mount 在子树挂载后恰好一次；on_unmount 在控件销毁时触发；
// Show 隐藏保留子树存活（不触发 on_unmount，对齐 Flutter Visibility）；空 unmount 不崩溃。

#include <cstdio>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/lifecycle.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Lifecycle;
using aurora::Node;
using aurora::Show;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_lifecycle ===\n");

    // 1) 基础：挂载后恰好触发一次 on_mount；销毁 Node 触发 on_unmount。
    {
        int mount_calls = 0;
        int unmount_calls = 0;
        bool ctx_usable = false;
        auto root = Node{ Lifecycle(
            Node{ Text{ "hi" } },
            [&](const BuildContext &ctx) -> void {
                ++mount_calls;
                ctx_usable = true;      // BuildContext 引用有效，可读取其字段
                (void)ctx.scale_factor; // 不崩溃即证明上下文可用
            },
            [&]() -> void { ++unmount_calls; }) };
        auto r = render_to_png(root, 200, 100, "lifecycle_out.png");
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(mount_calls == 1);
        AURORA_TEST_CHECK(unmount_calls == 0);
        AURORA_TEST_CHECK(ctx_usable);

        // mount 幂等：二次渲染不应再次触发 on_mount。
        auto r2 = render_to_png(root, 200, 100, "lifecycle_out.png");
        (void)r2;
        AURORA_TEST_CHECK(mount_calls == 1);

        // 销毁 Node → 析构触发 on_unmount（覆盖 Repeater 缩容 / Navigator pop 场景）。
        root = Node{}; // NOLINT
        AURORA_TEST_CHECK(unmount_calls == 1);
    }

    // 2) Show 隐藏：子树保留存活，不触发 on_unmount；整体销毁才卸载。
    {
        int mount_calls = 0;
        int unmount_calls = 0;
        auto root = Node{ Show(false, Node{ Lifecycle(
                                          Node{ Text{ "x" } }, [&](const BuildContext &) -> void { ++mount_calls; },
                                          [&]() -> void { ++unmount_calls; }) }) };
        auto r = render_to_png(root, 200, 100, "lifecycle_show_out.png");
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(mount_calls == 1);   // Show.on_mount 仍会挂载子节点
        AURORA_TEST_CHECK(unmount_calls == 0); // 仅隐藏，未卸载（存活语义）
        root = Node{};                         // NOLINT
        AURORA_TEST_CHECK(unmount_calls == 1); // 整体销毁才卸载
    }

    // 3) 空 on_unmount 回调不崩溃。
    {
        int mount_calls = 0;
        auto root = Node{ Lifecycle(Node{ Text{ "y" } }, [&](const BuildContext &) -> void { ++mount_calls; }) };
        auto r = render_to_png(root, 100, 100, "lifecycle_empty_out.png");
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(mount_calls == 1);
        root = Node{}; // 空 unmount 不崩溃 // NOLINT
        AURORA_TEST_CHECK(mount_calls == 1);
    }
}
