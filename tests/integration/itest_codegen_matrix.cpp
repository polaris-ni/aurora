/// 测试类型: integration
/// 目标单元: include/aurora/widget/codegen.h
/// 测试说明: 属性面「全量类型 × 三种生成风格」的编译矩阵——生成物由 gen_codegen_matrix 按
///           aurora_api.json 的在册类型逐个发射，本文件把它整份 include 进来，
///           于是「产物可编译」由编译器本身判定，不再依赖字符串比对。
/// 覆盖说明: 判据是编译通过 + 每格都能构造出 Node；产物不可编译的类型登记在
///           tools/gen/codegen_matrix_exemptions.txt，其条目在此断言为显式在册（不允许静默跳过）

#include <iterator>
#include <string_view>

#include "aurora/aurora.h"
#include "codegen_matrix.inc"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_codegen_matrix {

AURORA_TEST_CASE(codegen_matrix_compiles_every_compilable_type) {
    // 矩阵本体在编译期判定：任一格产物不合法，本文件根本编不过。
    static_assert(std::size(k_codegen_matrix) > 0, "codegen matrix must not be empty");
    for (const auto &entry : k_codegen_matrix) {
        AURORA_TEST_CHECK_TRUE(entry.label != nullptr && !std::string_view{entry.label}.empty());
        AURORA_TEST_CHECK_TRUE(entry.build != nullptr);
        // 运行时再走一遍，确保产物构造出的确实是可持有的 Node（而非仅语法成立）。
        au::Node node = entry.build();
        (void)node;
    }
}

AURORA_TEST_CASE(codegen_matrix_exemptions_are_explicit) {
    // 豁免是登记项而非静默跳过：空表意味着「全部可编译」（那时应删掉清单文件引用），
    // 非空则每条都必须是具名类型，供人工对账。
    static_assert(std::size(k_codegen_matrix_exempt) > 0, "exemption table must be materialized");
    for (const auto &entry : k_codegen_matrix_exempt) {
        AURORA_TEST_CHECK_TRUE(entry.type != nullptr && !std::string_view{entry.type}.empty());
    }
}

}  // namespace aurora::test_cases::itest_codegen_matrix
