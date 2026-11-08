// tests/fuzz/fuzz_effectiveness.cpp — 消毒链路有效性自证目标。
//
// 本文件故意含一处 UBSan 有符号溢出与一处 ASan 越界读，仅用于在夜间作业开头验证
// 「插桩 + UBSAN_OPTIONS=halt_on_error + 崩溃输入落盘」整条链路确实接上了：喂入
// corpus/fuzz_effectiveness 下的种子必须产生崩溃输入。若它不转红，其余五个目标的
// 「零发现」就毫无意义，作业必须整体判失败。
//
// 严禁把本目标链进产品库、常规测试 runner 或任何非模糊测试构建；它只在
// AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，且不参与正式的覆盖短跑。
#include <climits>
#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    if (size >= 1 && data[0] == 0xABU) {
        volatile int value = INT_MAX;
        value = value + 1;  // UB: signed integer overflow
        return value;
    }
    if (size >= 2 && data[1] == 0xCDU) {
        volatile int array[2] = {1, 2};
        return array[100];  // OOB read
    }
    return 0;
}
