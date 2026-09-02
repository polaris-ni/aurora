// test_default_construct.cpp — 验证所有控件类型可默认构造。

#include "test_default_construct.h"

#include "test_harness.h"

namespace au = aurora;

// 运行时验证：实际构造每个控件
AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_default_construct ===\n");

    // 基础控件
    {
        au::Button w;
        AURORA_TEST_CHECK_MSG(true, "Button default constructible");
    }
    {
        au::Text w;
        AURORA_TEST_CHECK_MSG(true, "Text default constructible");
    }
    {
        au::Checkbox w;
        AURORA_TEST_CHECK_MSG(true, "Checkbox default constructible");
    }
    {
        au::Chip w;
        AURORA_TEST_CHECK_MSG(true, "Chip default constructible");
    }
    {
        au::Badge w;
        AURORA_TEST_CHECK_MSG(true, "Badge default constructible");
    }
    {
        au::Skeleton w;
        AURORA_TEST_CHECK_MSG(true, "Skeleton default constructible");
    }
    {
        au::ProgressIndicator w;
        AURORA_TEST_CHECK_MSG(true, "ProgressIndicator default constructible");
    }
    {
        au::Divider w;
        AURORA_TEST_CHECK_MSG(true, "Divider default constructible");
    }

    // 布局容器
    {
        au::Column w;
        AURORA_TEST_CHECK_MSG(true, "Column default constructible");
    }
    {
        au::Row w;
        AURORA_TEST_CHECK_MSG(true, "Row default constructible");
    }
    {
        au::Stack w;
        AURORA_TEST_CHECK_MSG(true, "Stack default constructible");
    }
    {
        au::Grid w;
        AURORA_TEST_CHECK_MSG(true, "Grid default constructible");
    }
    {
        au::Scroll w;
        AURORA_TEST_CHECK_MSG(true, "Scroll default constructible");
    }
    {
        au::Splitter w;
        AURORA_TEST_CHECK_MSG(true, "Splitter default constructible");
    }
    {
        au::ToolBar w;
        AURORA_TEST_CHECK_MSG(true, "ToolBar default constructible");
    }
    {
        au::StatusBar w;
        AURORA_TEST_CHECK_MSG(true, "StatusBar default constructible");
    }

    // 高级容器
    {
        au::TabBar w;
        AURORA_TEST_CHECK_MSG(true, "TabBar default constructible");
    }
    {
        au::Drawer w;
        AURORA_TEST_CHECK_MSG(true, "Drawer default constructible");
    }
    {
        au::PageView w;
        AURORA_TEST_CHECK_MSG(true, "PageView default constructible");
    }
    {
        au::ExpansionPanel w;
        AURORA_TEST_CHECK_MSG(true, "ExpansionPanel default constructible");
    }
    {
        au::Popup w;
        AURORA_TEST_CHECK_MSG(true, "Popup default constructible");
    }
    {
        au::OverlayHost w;
        AURORA_TEST_CHECK_MSG(true, "OverlayHost default constructible");
    }
    {
        au::ToastHost w;
        AURORA_TEST_CHECK_MSG(true, "ToastHost default constructible");
    }
    {
        au::Dialog w;
        AURORA_TEST_CHECK_MSG(true, "Dialog default constructible");
    }
    {
        au::MenuBar w;
        AURORA_TEST_CHECK_MSG(true, "MenuBar default constructible");
    }
    {
        au::Dropdown w;
        AURORA_TEST_CHECK_MSG(true, "Dropdown default constructible");
    }
    {
        au::GridView w;
        AURORA_TEST_CHECK_MSG(true, "GridView default constructible");
    }
    {
        au::LazyList w;
        AURORA_TEST_CHECK_MSG(true, "LazyList default constructible");
    }
    {
        au::DataTable w;
        AURORA_TEST_CHECK_MSG(true, "DataTable default constructible");
    }
    {
        au::TreeView w;
        AURORA_TEST_CHECK_MSG(true, "TreeView default constructible");
    }
    {
        au::ListView w;
        AURORA_TEST_CHECK_MSG(true, "ListView default constructible");
    }
    {
        au::Form w;
        AURORA_TEST_CHECK_MSG(true, "Form default constructible");
    }
    {
        au::FormField w;
        AURORA_TEST_CHECK_MSG(true, "FormField default constructible");
    }
    {
        au::InspectorPanel w;
        AURORA_TEST_CHECK_MSG(true, "InspectorPanel default constructible");
    }
    {
        au::LayoutBuilder w;
        AURORA_TEST_CHECK_MSG(true, "LayoutBuilder default constructible");
    }

    // 叶控件 / 输入
    {
        au::Slider w;
        AURORA_TEST_CHECK_MSG(true, "Slider default constructible");
    }
    {
        au::Switch w;
        AURORA_TEST_CHECK_MSG(true, "Switch default constructible");
    }
    {
        au::TextInput w;
        AURORA_TEST_CHECK_MSG(true, "TextInput default constructible");
    }
    {
        au::RadioGroup w;
        AURORA_TEST_CHECK_MSG(true, "RadioGroup default constructible");
    }
    {
        au::SpinBox w;
        AURORA_TEST_CHECK_MSG(true, "SpinBox default constructible");
    }
    {
        au::SegmentedControl w;
        AURORA_TEST_CHECK_MSG(true, "SegmentedControl default constructible");
    }
    {
        au::Stepper w;
        AURORA_TEST_CHECK_MSG(true, "Stepper default constructible");
    }
    {
        au::DatePicker w;
        AURORA_TEST_CHECK_MSG(true, "DatePicker default constructible");
    }
    {
        au::TimePicker w;
        AURORA_TEST_CHECK_MSG(true, "TimePicker default constructible");
    }
    {
        au::ColorPicker w;
        AURORA_TEST_CHECK_MSG(true, "ColorPicker default constructible");
    }
    {
        au::RichText w;
        AURORA_TEST_CHECK_MSG(true, "RichText default constructible");
    }
    {
        au::RichTextEdit w;
        AURORA_TEST_CHECK_MSG(true, "RichTextEdit default constructible");
    }
    {
        au::ImageView w;
        AURORA_TEST_CHECK_MSG(true, "ImageView default constructible");
    }
    {
        au::Canvas w;
        AURORA_TEST_CHECK_MSG(true, "Canvas default constructible");
    }
    {
        au::Placeholder w;
        AURORA_TEST_CHECK_MSG(true, "Placeholder default constructible");
    }

    // 控制流 Widget
    {
        au::Show w;
        AURORA_TEST_CHECK_MSG(true, "Show default constructible");
    }
    {
        au::Lifecycle w;
        AURORA_TEST_CHECK_MSG(true, "Lifecycle default constructible");
    }
    {
        au::Timer w;
        AURORA_TEST_CHECK_MSG(true, "Timer default constructible");
    }

    // 媒体
    {
        au::VideoPlayer w;
        AURORA_TEST_CHECK_MSG(true, "VideoPlayer default constructible");
    }
    {
        au::VideoControls w;
        AURORA_TEST_CHECK_MSG(true, "VideoControls default constructible");
    }

    // Spacer
    {
        au::Spacer w;
        AURORA_TEST_CHECK_MSG(true, "Spacer default constructible");
    }
}
