#pragma once

// ============================================================================
// error_codes.h — 薄封装层
// ----------------------------------------------------------------------------
// 真正的错误码枚举、元数据表与查表函数由 codespec/errors.toml 经
// tools/gen_error_codes.cpp 生成于 error_codes.gen.h。请勿在此手改声明。
// 修改错误码请编辑 errors.toml 后重新运行生成器（cmake --build build --target generate_error_codes）。
// ============================================================================
#include "aurora/core/error_codes.gen.h"

namespace aurora {} // namespace aurora
