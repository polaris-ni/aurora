# ============================================================
# AuroraSimd.cmake — 光栅内核 SIMD 双实现开关
# ------------------------------------------------------------
# AURORA_ENABLE_SIMD 默认 ON：编译标量参考 + SSE2/AVX2 SIMD 快路径。
# OFF 时只有标量路径（与现有像素逐位一致）。
# 开启时给 aurora 目标加 -ffp-contract=off，杜绝 FMA 融合，确保 packed float
# 与标量 float 逐位相同。
# ⚠️ 必须在 add_library(aurora ...) 之后 include。
# ============================================================

option(AURORA_ENABLE_SIMD "Enable SIMD raster kernel dual-implementation (scalar reference + SSE2/AVX2 fast path)" ON)

if(AURORA_ENABLE_SIMD)
    target_compile_definitions(aurora PUBLIC AURORA_ENABLE_SIMD)
    # -ffp-contract=off：禁止 a*b+c 融合为 FMA，保证 SIMD 浮点序列与标量逐位一致。
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(aurora PRIVATE -ffp-contract=off)
    elseif(MSVC)
        # MSVC 等价：/fp:precise 禁用浮点收缩（FMA 融合），保证逐位一致。
        target_compile_options(aurora PRIVATE /fp:precise)
    endif()
    aurora_log("Raster SIMD: ON (scalar reference + SSE2 baseline + AVX2 runtime dispatch; -ffp-contract=off)")
else()
    aurora_log("Raster SIMD: OFF (scalar path only)")
endif()
