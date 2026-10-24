#pragma once

/// @brief 内置词条词表的公共只读面：`aurora::msg` 词条 Id 常量与规范名/英文兜底模板表。
/// @file i18n/builtin_messages.h
///
/// 数据源自 `codespec/i18n_builtin.json`（生成件见同名 `.gen.h`）；宿主按规范名登记译文即可
/// 覆盖控件内置文案（「库不预置译文」原则不变，见 specification/07-environment-modifier.md §6.2）。
/// 本头是薄封装，惯用包含入口；生成件勿直接引用。
///
/// @note Thread: thread-safe (pure data)
/// @note Side-effects: none
/// @note Rebuildable: no

#include "aurora/i18n/builtin_messages.gen.h"
