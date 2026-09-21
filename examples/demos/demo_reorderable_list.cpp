// 可拖拽重排列表 demo：按住条目上下拖动换位（跟手 + 其它项让位 + 松手 spring 落位）。
//
// 试法：按住任一卡片上下拖动——被拖卡片跟手并抬起（阴影），其余卡片实时让位；松手后弹簧落位，
// 数据顺序随之改写（下方文本框实时显示当前顺序）。拖到视口上下边缘会出现自动滚动。
// 条目自带点击回调，故拖拽限定在**右侧手柄带**（`set_drag_handle(true)`）：列表把该窄带留给自己，
// 避免子项的点击消费掉按下事件（这是 `ReorderableList` 的交互边界，见 specification/04-widget.md §3.4）。
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/widget/reorderable_list.h"
#include "aurora/widget/text.h"
#include "demo_common.h"

namespace {

/// 条目：带序号的卡片 + 右侧手柄提示（≡ 示意）。
auto task_row(const std::string &label, int index, au::Color accent) -> au::Node {
    auto row = std::make_unique<au::Row>();
    row->modifier.set(au::Modifier{}
                          .background(pal::AURORA_SURFACE, 6.0F)
                          .border(1.0F, accent)
                          .padding(au::EdgeInsets{.left = 12.0F, .top = 10.0F, .right = 12.0F, .bottom = 10.0F}));
    row->add(au::Node{std::make_shared<au::Text>(au::LocalizedString{"#" + std::to_string(index) + "  " + label})});
    row->set_gap(8.0F);

    auto handle = std::make_shared<au::Text>(au::LocalizedString{"≡"});
    handle->modifier.set(
        au::Modifier{}.padding(au::EdgeInsets{.left = 8.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F}));
    row->add(au::Node{std::move(handle)});

    std::shared_ptr<au::Widget> holder = std::move(row);
    return au::Node{std::move(holder)};
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径）
auto main() -> int {
    const std::vector<std::string> labels{"需求评审", "接口联调",     "写回归用例", "性能采样",
                                          "文档回写", "发布 alpha.5", "收集反馈",   "排下轮计划"};
    auto items = std::make_shared<au::State<std::vector<std::string>>>(labels);

    // 顺序镜像：重排回调里刷新显示（数据已由控件改写；此处只做展示）。
    auto order = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no reorder yet)"});

    auto list = std::make_shared<au::ReorderableList<std::string>>(
        items,
        [](const std::string &value, int index) -> au::Node {
            const au::Color accent = (index % 2 == 0) ? pal::AURORA_PRIMARY : pal::AURORA_ACCENT;
            return task_row(value, index, accent);
        },
        8.0F);
    list->set_drag_handle(true);  // 条目可点击 ⇒ 拖拽落在右侧手柄带
    list->set_auto_scroll_threshold(56.0F);
    list->set_on_reorder([items, order](int from, int to) -> void {
        const std::vector<std::string> &data = items->get();
        std::string text = "moved #" + std::to_string(from) + " → #" + std::to_string(to) + "：";
        for (std::size_t i = 0; i < data.size(); ++i) {
            text += (i == 0 ? "" : " / ");
            text += data[i];
        }
        order->set(au::LocalizedString{text});
    });

    // 视口：把列表放进固定高度的容器（列表按父约束取视口尺寸，故须给明确高度）。
    auto viewport = std::make_unique<au::Column>();
    viewport->modifier.set(au::Modifier{}.size(360.0F, 420.0F));
    viewport->add(au::Node{std::shared_ptr<au::Widget>(list)});
    std::shared_ptr<au::Widget> viewport_ptr = std::move(viewport);

    au::Node root = au::Column{
        GradientTitle{"Reorderable list"},
        au::Text{au::LocalizedString{"按住卡片上下拖动（右侧 ≡ 手柄带起拖）；松手后数据顺序即改写。"}},
        gap(8),
        au::Node{std::move(viewport_ptr)},
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{order}}},
    };
    return run_demo(Card{std::move(root)}, "Reorderable list · Aurora Demo", 560.0F, 620.0F);
}
