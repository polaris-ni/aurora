// 滚动位置保存/恢复 demo：两个独立列表各有 restore_key，位置跨「控件重建」与「进程重启」保留。
//
// 试法：滚动任一列表 → 点 `save now`（或直接关掉窗口）→ 重新运行本 demo，两个列表各自回到
// 上次的位置；`reset left` 演示清除某个键。持久化落在平台配置目录的
// `demo_scroll_restore.json`（`Preferences` 文件模式），故与真实应用一致地跨进程生效。
#include <functional>
#include <memory>
#include <string>

#include "aurora/app/scroll_storage.h"
#include "aurora/preferences/preferences.h"
#include "demo_common.h"

namespace {

/// 固定尺寸宿主容器：虚拟列表按父约束取视口尺寸，故须给它一个明确高度才会滚动。
auto fixed(float w, float h, const au::Node &child) -> au::Node {
    auto box = std::make_unique<au::Column>();
    box->modifier.set(au::Modifier{}.size(w, h).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
    box->add(child);
    std::shared_ptr<au::Widget> holder = std::move(box);
    return au::Node{std::move(holder)};
}

/// 带 `restore_key` 的滚动列表：同 key 的新实例（重建 / 重启）会恢复上次位置。
auto feed(std::shared_ptr<au::LazyList> &out, const char *key, int count, au::Color accent) -> au::Node {
    auto list = std::make_shared<au::LazyList>(
        count,
        [accent](int i) -> au::Node {
            auto row = std::make_unique<au::Row>();
            row->modifier.set(au::Modifier{}.padding(8.0F).background(pal::AURORA_SURFACE).border(1.0F, accent));
            row->add(au::Node{std::make_shared<au::Text>(au::LocalizedString{"item " + std::to_string(i)})});
            std::shared_ptr<au::Widget> holder = std::move(row);
            return au::Node{std::move(holder)};
        },
        40.0F);
    list->set_restore_key(key);
    out = list;
    return au::Node{std::shared_ptr<au::Widget>(list)};
}

/// 一个小按钮（demo 内联用）。
auto button(const char *label, std::function<void()> on_click) -> au::Node {
    auto btn = std::make_shared<au::Button>(label);
    btn->set_on_click(std::move(on_click));
    return au::Node{std::shared_ptr<au::Widget>(btn)};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    // 文件模式持久化：本 demo 的 key 只落在自己的配置文件里。
    au::preferences::Preferences &prefs = au::preferences::Preferences::instance_at(
        "demo_scroll_restore", au::preferences::Preferences::default_config_dir() / "demo_scroll_restore.json");
    au::ScrollStorage &storage = au::ScrollStorage::instance();
    storage.attach(prefs);

    std::shared_ptr<au::LazyList> left_list;
    std::shared_ptr<au::LazyList> right_list;
    au::Node left = fixed(280.0F, 240.0F, feed(left_list, "demo.left", 60, pal::AURORA_PRIMARY));
    au::Node right = fixed(280.0F, 240.0F, feed(right_list, "demo.right", 60, pal::AURORA_ACCENT));

    au::Row columns;
    columns.add(left);
    columns.add(right);
    columns.set_gap(12.0F);

    au::Row actions;
    actions.add(button("reset left", [left_list]() -> void {
        left_list->set_scroll_offset(0.0F);
        // 先归零再删键：顺序反了会被 setter 写回覆盖。
        au::ScrollStorage::instance().clear("demo.left");
    }));
    actions.add(button("save now", [&prefs]() -> void {
        au::ScrollStorage::instance().sync();  // 内存 → Preferences 内存
        (void)prefs.flush();  // Preferences 内存 → 磁盘
    }));
    actions.set_gap(8.0F);

    au::Node root = au::Column{
        GradientTitle{"Scroll position restore"},
        au::Text{au::LocalizedString{"Scroll either list, then `save now` (or close the window) and re-run this demo: "
                                     "each list returns to its own position. Keys are independent."}},
        gap(8),
        std::move(columns),
        gap(12),
        std::move(actions),
    };
    const int rc = run_demo(Card{std::move(root)}, "Scroll restore · Aurora Demo", 640.0F, 430.0F);

    // 退出前落盘（运行中亦可随时按 `save now`）。
    au::ScrollStorage::instance().sync();
    (void)prefs.flush();
    au::ScrollStorage::instance().detach();
    return rc;
}
