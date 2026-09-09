/// 测试类型: integration
/// 目标单元: include/aurora/widget/widget.h
/// 测试说明: 逐个公共控件默认构造 + 语义冒烟——每个控件挂进 Column 并布局一帧
///           （render_to_logical_snapshot），控件清单以 include/aurora/widget/ 现有头为准；
///           另验证 WidgetRegistry 可重建全部已注册类型（Canvas/Repeater/Provider 除外）
/// 覆盖说明: 旧 tests/common/test_default_construct.h 已删除，本文件为其重写

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/canvas.h"
#include "aurora/widget/skeleton.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_default_construct {

namespace {

// 语义冒烟：把已挂好子件的根 Column 布局一帧，返回逻辑快照根类型。
// 注意 render_to_logical_snapshot 需要 Node& 非 const 引用（内部 mount+layout 会写节点 bounds）。
auto layout_frame(au::Node& root) -> au::Json { return au::render_to_logical_snapshot(root, 320, 240); }

}  // namespace

AURORA_TEST_CASE(basic_leaf_widgets_default_construct_and_layout) {
    // Widget 拷贝已删除：根 Column 必须经 Node 直接管在堆上，子件也逐个包 Node 挂入。
    au::Node root{au::Column{
        au::Node{au::Text{}},
        au::Node{au::Button{}},
        au::Node{au::Checkbox{}},
        au::Node{au::Chip{}},
        au::Node{au::Badge{}},
        au::Node{au::Skeleton{}},
        au::Node{au::ProgressIndicator{}},
        au::Node{au::Divider{}},
        au::Node{au::Placeholder{}},
        au::Node{au::Spacer{}},
        au::Node{au::RichText{}},
        au::Node{au::RichTextEdit{}},
        au::Node{au::Canvas{}},
    }};

    const au::Json snap = layout_frame(root);
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Column"});
    AURORA_TEST_CHECK_EQ(snap["children"].size(), 13U);
}

AURORA_TEST_CASE(layout_containers_default_construct_and_layout) {
    au::Node root{au::Column{
        au::Node{au::Column{}},
        au::Node{au::Row{}},
        au::Node{au::Stack{}},
        au::Node{au::Grid{}},
        au::Node{au::Scroll{}},
        au::Node{au::Splitter{}},
        au::Node{au::PageView{}},
        au::Node{au::OverlayHost{}},
        au::Node{au::Popup{}},
        au::Node{au::Drawer{}},
        au::Node{au::ToastHost{}},
        au::Node{au::ToolBar{}},
        au::Node{au::StatusBar{}},
        au::Node{au::TitleBar{}},
        au::Node{au::TabBar{}},
        au::Node{au::MenuBar{}},
        au::Node{au::Dropdown{}},
        au::Node{au::ExpansionPanel{}},
    }};

    const au::Json snap = layout_frame(root);
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Column"});
    AURORA_TEST_CHECK_EQ(snap["children"].size(), 18U);
}

AURORA_TEST_CASE(input_widgets_default_construct_and_layout) {
    au::Node root{au::Column{
        au::Node{au::TextInput{}},
        au::Node{au::Slider{}},
        au::Node{au::Switch{}},
        au::Node{au::RadioGroup{}},
        au::Node{au::SpinBox{}},
        au::Node{au::SegmentedControl{}},
        au::Node{au::Stepper{}},
        au::Node{au::DatePicker{}},
        au::Node{au::TimePicker{}},
        au::Node{au::ColorPicker{}},
    }};

    const au::Json snap = layout_frame(root);
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Column"});
    AURORA_TEST_CHECK_EQ(snap["children"].size(), 10U);
}

AURORA_TEST_CASE(data_media_widgets_default_construct_and_layout) {
    au::Node root{au::Column{
        au::Node{au::DataTable{}},
        au::Node{au::TreeView{}},
        au::Node{au::ListView{}},
        au::Node{au::Form{}},
        au::Node{au::FormField{}},
        au::Node{au::VideoPlayer{}},
        au::Node{au::PerfOverlay{}},
    }};

    const au::Json snap = layout_frame(root);
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Column"});
    AURORA_TEST_CHECK_EQ(snap["children"].size(), 7U);
}

AURORA_TEST_CASE(registry_rebuilds_every_registered_type) {
    au::serialization::register_core_widgets();
    const auto types = au::serialization::WidgetRegistry::instance().list_types();
    AURORA_TEST_REQUIRE_MSG(!types.empty(), "WidgetRegistry registers at least one widget type");

    // 已知类型但不可从静态 JSON 重建（持运行时回调/未注册 T），工厂给出友好错误而非崩溃。
    const std::vector<std::string> not_rebuildable = {"Canvas", "Repeater", "Provider"};

    for (const auto& type : types) {
        const bool expect_error = std::ranges::find(not_rebuildable, type) != not_rebuildable.end();
        const auto made = au::serialization::WidgetRegistry::instance().make(type, au::Json::object());
        if (expect_error) {
            AURORA_TEST_CHECK_MSG(!made, "registry rejects non-rebuildable type with friendly error: " + type);
        } else {
            AURORA_TEST_CHECK_MSG(made.ok(), "registry default-constructs registered type: " + type);
        }
    }
}

}  // namespace aurora::test_cases::itest_default_construct
