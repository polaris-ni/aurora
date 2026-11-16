# ============================================================
# AuroraSimd.cmake — 光栅内核 SIMD 双实现（恒开，无开关）
# ------------------------------------------------------------
# 标量参考 + SSE2/AVX2 SIMD 快路径恒编入：运行期 CPUID 分发决定走哪条路径，
# 关闭档不改变任何语义输出（utest_simd_parity 逐位一致门禁）。
# 无条件给 aurora 目标加 -ffp-contract=off，杜绝 FMA 融合，确保 packed float
# 与标量 float 逐位相同——它是逐位一致的前提，不允许随任何开关消失。
# ⚠️ 必须在 add_library(aurora ...) 之后 include。
# ============================================================

# -ffp-contract=off：禁止 a*b+c 融合为 FMA，保证 SIMD 浮点序列与标量逐位一致。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(aurora PRIVATE -ffp-contract=off)
elseif(MSVC)
    # MSVC 等价：/fp:precise 禁用浮点收缩（FMA 融合），保证逐位一致。
    target_compile_options(aurora PRIVATE /fp:precise)
endif()
aurora_log("Raster SIMD: always on (scalar reference + SSE2 baseline + AVX2 runtime dispatch; -ffp-contract=off)")
