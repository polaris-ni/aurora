#pragma once

// ============================================================
// 测试框架统一入口（tests/framework/aurora_test.h）
// ------------------------------------------------------------
// 测试 TU 只需包含本头：注册宏、断言宏、runner 设施一并就绪。
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。
// ============================================================

#include "assertions.h"
#include "death_test.h"
#include "isolation.h"
#include "matchers.h"
#include "parameterized.h"
#include "test_fixture.h"
#include "test_registry.h"
#include "test_runner.h"
#include "test_types.h"
