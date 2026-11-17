# CHG-084 上屏像素转换的 AVX2 显式内核与运行时分发

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-084 |
| 提出日期 | 2026-11-17 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `src/aurora/window/swizzle.h`（三处像素转换：R/B 交换、预乘 + R/B 交换、反序交换）；`tests/unit/utest_window_swizzle.cpp`。**公共 API 与 `aurora_api.json` 无差异**（内部头，仅库实现可见） |

## 动机

1. **上屏转换按整窗像素量级跑，但只有编译器自动向量化的基线循环。** Win32 常驻 BGRA DIB 与 Wayland `wl_shm` 上屏各自逐行调用 `swizzle_rgba_to_bgra` / `swizzle_rgba_premul_to_bgra`，2560×1440 一帧合计约 1470 万字节读写。x86-64 基线只有 SSE2；`-O3` 虽能自动向量化，但宽度停在 16 字节。

2. **实测差异明显可测。** 同机（GCC 16.1.0，`-O3`，2560×1440 全幅，300 次均值）：
   - R/B 交换：基线 1.23 ms → AVX2 内核 0.91 ms（约 -26%）；
   - 预乘 + R/B 交换：基线 2.86 ms → AVX2 内核 1.19 ms（约 -58%）。
   预乘项差距更大，因为标量口径的 `c*a/255` 在 16 位通道上要拆包重组，AVX2 的 32 位乘可整字处理。

3. **此项此前被显式排除，需独立立项。** 上一轮热路径收敛明确「不做 `swizzle` SIMD，先看编译产物」；编译产物与实测已证明有收益，故单独立案，边界只含转换内核与其分发，不触及上屏路径的其它环节。

## 变更内容

1. `swizzle.h` 的既有实现原地改名为 `*_scalar`，作为逐字节黄金参考（逻辑一字不改）。

2. 新增 AVX2 内核（`target("avx2")` 逐函数升档，仅 x86 + GCC/Clang 编译）：
   - R/B 交换用 `pshufb` 字节置换（`[2,1,0,3]`），等价于标量的位交换；
   - 预乘用 32 位 `mullo`，与标量同式 `(c*a+255)>>8`、同截断（乘积上限 65025，不溢出）；
   - 不足 8 像素的尾部回落标量参考。

3. 三个对外名字（`swizzle_rgba_to_bgra` / `swizzle_bgra_to_rgba` / `swizzle_rgba_premul_to_bgra`）改为分发入口：像素数 ≥ 8 且运行时探测到 AVX2（`__builtin_cpu_supports`，含 OS XSAVE 检查，首次调用后缓存）走内核，否则走标量参考。MSVC 无非 x86 逐函数目标属性，保持基线循环；非 x86 构建同样只走标量参考。

4. 调用点零改动（三个名字不变），像素输出逐字节不变。

## 验收判据

1. 逐字节一致：新增用例以同一随机缓冲比对「标量参考 / AVX2 内核 / 分发入口」，覆盖长度 1、7、8、9、1031（尾部与整组边界），差异字节数必须为 0。

2. 退让语义：未编译 AVX2 内核或运行时无 AVX2 时，用例以 `AURORA_TEST_SKIP` 退让，`registry_integrity` 不漂移。

3. 既有语义不回退：`utest_window_swizzle` 既有三例（alpha=0 归零、alpha=255 与旧 XRGB 直转恒等、半透明预乘值域）保持全绿。

4. 门禁：`ctest --preset ninja-test` 全绿（含 `check_platform_macros` 与 `format-check`），改动文件 clang-tidy 无告警，`aurora_api.json` 无差异。

5. 性能：以上述同机同构建口径记录改动前后单帧转换耗时，只接受实测改善；不新增也不收紧 `perf_gates.json` 阈值。

## 回写落点

- `codespec/specification/06-app-platform.md`：上屏性能契约补「像素转换按 AVX2 运行时分发，逐字节一致，无 AVX2 回落基线循环」。
- `codespec/ARCHITECTURE.md`：渲染与窗口章节的 CPU swizzle 描述同步为双实现口径。

## 实施记录

- `swizzle.h`：三处标量实现改名 `*_scalar` 并补全注释；新增 `detail::swap_rb_avx2` / `detail::premul_bgra_avx2`（`target("avx2")`，尾部回落标量）与 `detail::swizzle_avx2_available`（一次性缓存）；三个对外名字改分发入口，`count < 8` 直接走标量参考。
- 运行时探测按工具链分流（与 `painter_simd.inl` 的 `detect_simd_level` 同一口径）：GCC / 非 clang-cl 的 Clang 用 `__builtin_cpu_supports("avx2")`；clang-cl 改用 CPUID leaf 7 EBX bit5 + OSXSAVE / XCR0 校验——该 ABI 下内建会引用 `__cpu_model`，而 MSVC 链接运行时不提供该符号（`lld-link: undefined symbol: __cpu_model`，由 windows-llvm 构建暴露）。
- 实测（GCC 16.1.0，`-O3`，2560×1440，300 次均值）：R/B 交换 1.23 → 0.91 ms；预乘 2.86 → 1.19 ms。两路输出与标量参考逐字节相同。
- 用例：`utest_window_swizzle` 新增标量 / AVX2 / 分发三路逐字节比对（长度 1、7、8、9、1031），并保留原有三例。
