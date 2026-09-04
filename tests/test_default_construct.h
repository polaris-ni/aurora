// test_default_construct.h — 编译期验证所有控件类型可默认构造。
// 新增控件时，在此文件添加对应的 CHECK_DEFAULT_CONSTRUCT 行即可。

#pragma once

#include <type_traits>

#include "aurora/aurora.h"
#include "aurora/widget/dialog.h"

namespace au = aurora;

// 编译期验证宏：每个控件类型必须可默认构造
#define CHECK_DEFAULT_CONSTRUCT(T) \
    static_assert(std::is_default_constructible_v<T>, #T " must be default constructible");

// ---- 基础控件 ----
CHECK_DEFAULT_CONSTRUCT(au::Button)
CHECK_DEFAULT_CONSTRUCT(au::Text)
CHECK_DEFAULT_CONSTRUCT(au::Checkbox)
CHECK_DEFAULT_CONSTRUCT(au::Chip)
CHECK_DEFAULT_CONSTRUCT(au::Badge)
CHECK_DEFAULT_CONSTRUCT(au::Skeleton)
CHECK_DEFAULT_CONSTRUCT(au::ProgressIndicator)
CHECK_DEFAULT_CONSTRUCT(au::Divider)

// ---- 布局容器 ----
CHECK_DEFAULT_CONSTRUCT(au::Column)
CHECK_DEFAULT_CONSTRUCT(au::Row)
CHECK_DEFAULT_CONSTRUCT(au::Stack)
CHECK_DEFAULT_CONSTRUCT(au::Grid)
CHECK_DEFAULT_CONSTRUCT(au::Scroll)
CHECK_DEFAULT_CONSTRUCT(au::Splitter)
CHECK_DEFAULT_CONSTRUCT(au::ToolBar)
CHECK_DEFAULT_CONSTRUCT(au::StatusBar)

// ---- 高级容器 ----
CHECK_DEFAULT_CONSTRUCT(au::TabBar)
CHECK_DEFAULT_CONSTRUCT(au::Drawer)
CHECK_DEFAULT_CONSTRUCT(au::PageView)
CHECK_DEFAULT_CONSTRUCT(au::ExpansionPanel)
CHECK_DEFAULT_CONSTRUCT(au::Popup)
CHECK_DEFAULT_CONSTRUCT(au::OverlayHost)
CHECK_DEFAULT_CONSTRUCT(au::ToastHost)
CHECK_DEFAULT_CONSTRUCT(au::Dialog)
CHECK_DEFAULT_CONSTRUCT(au::MenuBar)
CHECK_DEFAULT_CONSTRUCT(au::Dropdown)
CHECK_DEFAULT_CONSTRUCT(au::GridView)
CHECK_DEFAULT_CONSTRUCT(au::LazyList)
CHECK_DEFAULT_CONSTRUCT(au::DataTable)
CHECK_DEFAULT_CONSTRUCT(au::TreeView)
CHECK_DEFAULT_CONSTRUCT(au::ListView)
CHECK_DEFAULT_CONSTRUCT(au::Form)
CHECK_DEFAULT_CONSTRUCT(au::FormField)
CHECK_DEFAULT_CONSTRUCT(au::InspectorPanel)
CHECK_DEFAULT_CONSTRUCT(au::LayoutBuilder)

// ---- 叶控件 / 输入 ----
CHECK_DEFAULT_CONSTRUCT(au::Slider)
CHECK_DEFAULT_CONSTRUCT(au::Switch)
CHECK_DEFAULT_CONSTRUCT(au::TextInput)
CHECK_DEFAULT_CONSTRUCT(au::RadioGroup)
CHECK_DEFAULT_CONSTRUCT(au::SpinBox)
CHECK_DEFAULT_CONSTRUCT(au::SegmentedControl)
CHECK_DEFAULT_CONSTRUCT(au::Stepper)
CHECK_DEFAULT_CONSTRUCT(au::DatePicker)
CHECK_DEFAULT_CONSTRUCT(au::TimePicker)
CHECK_DEFAULT_CONSTRUCT(au::ColorPicker)
CHECK_DEFAULT_CONSTRUCT(au::RichText)
CHECK_DEFAULT_CONSTRUCT(au::RichTextEdit)
CHECK_DEFAULT_CONSTRUCT(au::ImageView)
CHECK_DEFAULT_CONSTRUCT(au::Canvas)
CHECK_DEFAULT_CONSTRUCT(au::Placeholder)

// ---- 控制流 Widget ----
CHECK_DEFAULT_CONSTRUCT(au::Show)
CHECK_DEFAULT_CONSTRUCT(au::Lifecycle)
CHECK_DEFAULT_CONSTRUCT(au::Timer)

// ---- 媒体 ----
CHECK_DEFAULT_CONSTRUCT(au::VideoPlayer)
CHECK_DEFAULT_CONSTRUCT(au::VideoControls)

// ---- Spacer（explicit 但参数有默认值） ----
CHECK_DEFAULT_CONSTRUCT(au::Spacer)
