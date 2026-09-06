// default_construct.h — 编译期验证所有控件类型可默认构造（公共 fixture）。
//
// 迁自 tests/test_default_construct.h：去除 namespace au 别名，统一全限定 aurora::。
// 新增控件时，在此文件添加对应的 CHECK_DEFAULT_CONSTRUCT 行即可。

#pragma once

#include <type_traits>

#include "aurora/aurora.h"
#include "aurora/widget/dialog.h"

// 编译期验证宏：每个控件类型必须可默认构造
#define CHECK_DEFAULT_CONSTRUCT(T) \
    static_assert(std::is_default_constructible_v<T>, #T " must be default constructible");

// ---- 基础控件 ----
CHECK_DEFAULT_CONSTRUCT(aurora::Button)
CHECK_DEFAULT_CONSTRUCT(aurora::Text)
CHECK_DEFAULT_CONSTRUCT(aurora::Checkbox)
CHECK_DEFAULT_CONSTRUCT(aurora::Chip)
CHECK_DEFAULT_CONSTRUCT(aurora::Badge)
CHECK_DEFAULT_CONSTRUCT(aurora::Skeleton)
CHECK_DEFAULT_CONSTRUCT(aurora::ProgressIndicator)
CHECK_DEFAULT_CONSTRUCT(aurora::Divider)

// ---- 布局容器 ----
CHECK_DEFAULT_CONSTRUCT(aurora::Column)
CHECK_DEFAULT_CONSTRUCT(aurora::Row)
CHECK_DEFAULT_CONSTRUCT(aurora::Stack)
CHECK_DEFAULT_CONSTRUCT(aurora::Grid)
CHECK_DEFAULT_CONSTRUCT(aurora::Scroll)
CHECK_DEFAULT_CONSTRUCT(aurora::Splitter)
CHECK_DEFAULT_CONSTRUCT(aurora::ToolBar)
CHECK_DEFAULT_CONSTRUCT(aurora::StatusBar)

// ---- 高级容器 ----
CHECK_DEFAULT_CONSTRUCT(aurora::TabBar)
CHECK_DEFAULT_CONSTRUCT(aurora::Drawer)
CHECK_DEFAULT_CONSTRUCT(aurora::PageView)
CHECK_DEFAULT_CONSTRUCT(aurora::ExpansionPanel)
CHECK_DEFAULT_CONSTRUCT(aurora::Popup)
CHECK_DEFAULT_CONSTRUCT(aurora::OverlayHost)
CHECK_DEFAULT_CONSTRUCT(aurora::ToastHost)
CHECK_DEFAULT_CONSTRUCT(aurora::Dialog)
CHECK_DEFAULT_CONSTRUCT(aurora::MenuBar)
CHECK_DEFAULT_CONSTRUCT(aurora::Dropdown)
CHECK_DEFAULT_CONSTRUCT(aurora::GridView)
CHECK_DEFAULT_CONSTRUCT(aurora::LazyList)
CHECK_DEFAULT_CONSTRUCT(aurora::DataTable)
CHECK_DEFAULT_CONSTRUCT(aurora::TreeView)
CHECK_DEFAULT_CONSTRUCT(aurora::ListView)
CHECK_DEFAULT_CONSTRUCT(aurora::Form)
CHECK_DEFAULT_CONSTRUCT(aurora::FormField)
CHECK_DEFAULT_CONSTRUCT(aurora::InspectorPanel)
CHECK_DEFAULT_CONSTRUCT(aurora::LayoutBuilder)

// ---- 叶控件 / 输入 ----
CHECK_DEFAULT_CONSTRUCT(aurora::Slider)
CHECK_DEFAULT_CONSTRUCT(aurora::Switch)
CHECK_DEFAULT_CONSTRUCT(aurora::TextInput)
CHECK_DEFAULT_CONSTRUCT(aurora::RadioGroup)
CHECK_DEFAULT_CONSTRUCT(aurora::SpinBox)
CHECK_DEFAULT_CONSTRUCT(aurora::SegmentedControl)
CHECK_DEFAULT_CONSTRUCT(aurora::Stepper)
CHECK_DEFAULT_CONSTRUCT(aurora::DatePicker)
CHECK_DEFAULT_CONSTRUCT(aurora::TimePicker)
CHECK_DEFAULT_CONSTRUCT(aurora::ColorPicker)
CHECK_DEFAULT_CONSTRUCT(aurora::RichText)
CHECK_DEFAULT_CONSTRUCT(aurora::RichTextEdit)
CHECK_DEFAULT_CONSTRUCT(aurora::ImageView)
CHECK_DEFAULT_CONSTRUCT(aurora::Canvas)
CHECK_DEFAULT_CONSTRUCT(aurora::Placeholder)

// ---- 控制流 Widget ----
CHECK_DEFAULT_CONSTRUCT(aurora::Show)
CHECK_DEFAULT_CONSTRUCT(aurora::Lifecycle)
CHECK_DEFAULT_CONSTRUCT(aurora::Timer)

// ---- 媒体 ----
CHECK_DEFAULT_CONSTRUCT(aurora::VideoPlayer)
CHECK_DEFAULT_CONSTRUCT(aurora::VideoControls)

// ---- Spacer（explicit 但参数有默认值） ----
CHECK_DEFAULT_CONSTRUCT(aurora::Spacer)
