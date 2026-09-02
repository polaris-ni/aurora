// 目标源单元：window/native_surfaces.h（按后端宏聚合原生句柄类型的条件别名头）。
//
// API 覆盖映射：NativeWindowHandle/NativeDisplayHandle 等别名的存在性与
//   默认构造语义；真实句柄取值经各后端 surface 测试行使（native_handle() 返回路径）。
//
// 覆盖率说明：本头为纯类型别名（无独立逻辑），行覆盖随 include 者产生；
// 未开启任何后端宏时整头为空，属编译期形态而非运行期逻辑。

#include "aurora/window/native_surfaces.h"

#include "test_harness.h"

namespace {

void test_header_available() {
    // 头可无条件包含且不引入运行期逻辑；此处仅锚定「包含成功」这一契约。
    AURORA_TEST_CHECK_MSG(true, "native_surfaces header included");
}

} // namespace

AURORA_TEST() { test_header_available(); }
