# ============================================================
# AuroraFuzz.cmake — 覆盖引导模糊测试目标（tests/fuzz/，默认 OFF + EXCLUDE_FROM_ALL）
# ------------------------------------------------------------
# 只在 AURORA_BUILD_FUZZ=ON 且 C++ 编译器为 Clang 时定义目标；libFuzzer 无 GCC/MSVC
# 运行时，Emscripten 与 AppleClang 亦不提供，故必须配独立构建目录，不能挂进默认构建或
# CTest。全部目标 EXCLUDE_FROM_ALL，`cmake --build build` 不会连带构建它们。
#
# 开启（独立目录，显式关掉另两套插桩）：
#   cmake -S . -B build-fuzz -G Ninja \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#     -DAURORA_BUILD_FUZZ=ON -DAURORA_BUILD_TESTS=OFF \
#     -DAURORA_ENABLE_ASAN=OFF -DAURORA_ENABLE_COVERAGE=OFF \
#     -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
#   cmake --build build-fuzz --target aurora_fuzz
#
# 每个目标只编译被测路径所需的少数翻译单元，**不链接 aurora 整库**：模糊测试面全是
# 纯解析路径，拉进平台后端与第三方解码器只会扩大插桩面、引入未插桩库噪声。两处链接桩
# 是这条边界的显式声明，见 tests/fuzz/ 下对应源文件头注释。
#
# 编译与链接同参注入 fuzzer,address,undefined；UBSan 的中断开关由运行时环境变量负责，
# 判定与有效性自证在 tools/check/ 下的判定脚本里，不内联进任何工作流脚本块。
# 全部开关/宏/环境变量统一列于 codespec/BUILD_OPTIONS.md（唯一权威来源）。
# ============================================================

option(AURORA_BUILD_FUZZ "Define the libFuzzer targets under tests/fuzz/ (EXCLUDE_FROM_ALL)" OFF)

if (NOT AURORA_BUILD_FUZZ)
    return()
endif ()

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    aurora_error("AURORA_BUILD_FUZZ requires CMAKE_CXX_COMPILER=clang++ (libFuzzer is Clang-only); got '${CMAKE_CXX_COMPILER_ID}'.")
endif ()

# SanitizerCoverage 与 --coverage / -fprofile-instr-generate 会互相干扰；地址消毒的
# 逐目标注入也会与本模块重复。二者必须在这份独立配置里显式关闭。
if (AURORA_ENABLE_ASAN OR AURORA_ENABLE_COVERAGE)
    aurora_error("AURORA_BUILD_FUZZ conflicts with AURORA_ENABLE_ASAN / AURORA_ENABLE_COVERAGE; use a dedicated build directory with both OFF.")
endif ()

set(_aurora_fuzz_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz")
set(_aurora_fuzz_src "${CMAKE_CURRENT_SOURCE_DIR}/src/aurora")

function(_aurora_add_fuzz_target _name)
    add_executable(${_name} EXCLUDE_FROM_ALL ${ARGN})
    set_target_properties(${_name} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            DISABLE_PRECOMPILE_HEADERS ON)
    target_include_directories(${_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # -O1 覆盖 Release 的 -O3；-g 与 -fno-omit-frame-pointer 保证消毒报告可读。
    target_compile_options(${_name} PRIVATE
            -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
    target_link_options(${_name} PRIVATE
            -fsanitize=fuzzer,address,undefined)
endfunction()

set(_aurora_fuzz_targets "")

_aurora_add_fuzz_target(fuzz_uri_list
        "${_aurora_fuzz_dir}/fuzz_uri_list.cpp"
        "${_aurora_fuzz_src}/app/uri_list.cpp")
list(APPEND _aurora_fuzz_targets fuzz_uri_list)

_aurora_add_fuzz_target(fuzz_json
        "${_aurora_fuzz_dir}/fuzz_json.cpp"
        "${_aurora_fuzz_src}/core/json/parse.cpp"
        "${_aurora_fuzz_src}/core/json/sax.cpp"
        "${_aurora_fuzz_src}/core/json/value.cpp"
        "${_aurora_fuzz_src}/core/json/dump.cpp"
        "${_aurora_fuzz_src}/core/json/pointer.cpp"
        "${_aurora_fuzz_src}/core/result.cpp"
        "${_aurora_fuzz_src}/core/utf8.cpp")
list(APPEND _aurora_fuzz_targets fuzz_json)

_aurora_add_fuzz_target(fuzz_http_wire
        "${_aurora_fuzz_dir}/fuzz_http_wire.cpp"
        "${_aurora_fuzz_src}/net/detail/http_wire.cpp"
        "${_aurora_fuzz_src}/core/url.cpp"
        "${_aurora_fuzz_src}/core/result.cpp"
        "${_aurora_fuzz_src}/core/utf8.cpp")
list(APPEND _aurora_fuzz_targets fuzz_http_wire)

_aurora_add_fuzz_target(fuzz_ws_frame
        "${_aurora_fuzz_dir}/fuzz_ws_frame.cpp"
        "${_aurora_fuzz_src}/net/detail/ws_frame.cpp"
        "${_aurora_fuzz_src}/core/result.cpp"
        "${_aurora_fuzz_src}/core/utf8.cpp")
list(APPEND _aurora_fuzz_targets fuzz_ws_frame)

_aurora_add_fuzz_target(fuzz_bmp
        "${_aurora_fuzz_dir}/fuzz_bmp.cpp"
        "${_aurora_fuzz_dir}/fuzz_image_stubs.cpp"
        "${_aurora_fuzz_src}/core/image.cpp"
        "${_aurora_fuzz_src}/core/result.cpp")
list(APPEND _aurora_fuzz_targets fuzz_bmp)

# 消毒链路有效性自证：故意含缺陷，只由判定脚本在作业开头带固定种子运行。
_aurora_add_fuzz_target(fuzz_effectiveness
        "${_aurora_fuzz_dir}/fuzz_effectiveness.cpp")
list(APPEND _aurora_fuzz_targets fuzz_effectiveness)

add_custom_target(aurora_fuzz DEPENDS ${_aurora_fuzz_targets})
