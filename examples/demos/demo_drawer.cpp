// Drawer / ProgressDialog / PageView 控件 demo：抽屉侧栏 + 进度弹窗 + 翻页容器。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // PageView：三页可滑动翻页（左右拖拽超过 1/4 宽切页）
    std::vector<au::Node> pages;
    pages.emplace_back(au::Column{au::Text{"Page 1"}, au::Text{"Swipe left to flip page"}});
    pages.emplace_back(au::Column{au::Text{"Page 2"}});
    pages.emplace_back(au::Column{au::Text{"Page 3"}});
    au::PageView page_view{std::move(pages)};

    // 抽屉：左侧面板 + 主内容
    auto drawer =
        std::make_shared<au::Drawer>(au::Node{au::Column{
                                         GradientTitle{"Drawer / PageView"},
                                         gap(8),
                                         std::move(page_view),
                                     }},
                                     au::Node{au::Column{au::Text{"Sidebar menu"}, au::Text{"Click overlay to close"}}},
                                     au::DrawerSide::Left, 200.0F);

    au::Button open_btn{au::ButtonProps{.label = "Open drawer"}};
    open_btn.on_click = [drawer]() -> void { drawer->set_open(true); };

    au::Node root = au::Column{
        std::move(open_btn),
        gap(8),
        au::Node{std::shared_ptr<au::Widget>(drawer)},
    };
    return run_demo(std::move(root), "Drawer · Aurora Demo", 560.0F, 420.0F);
}