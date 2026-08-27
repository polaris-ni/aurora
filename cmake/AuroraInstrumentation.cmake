# ============================================================
# AuroraInstrumentation.cmake — 插桩开关：覆盖率 / ASan+UBSan / 渲染性能埋点
# ------------------------------------------------------------
# 覆盖率与 ASan 互斥（都改写代码生成），共用两段辅助逻辑：
#   _aurora_strip_optimization_flags() — 清除 Release 默认 -O3/-Os/-DNDEBUG
#   _aurora_instrument_all_targets(<flags...>) — 对所有已定义可编译目标注入插桩
# 渲染性能埋点（AURORA_ENABLE_PROFILING / AURORA_ENABLE_TRACING）只加编译定义，
# 不改代码生成，与上述两者互不冲突。
# ⚠️ 本模块须在所有目标（库/demo/工具/测试）定义之后 include，否则遍历不到后定义目标。
# ============================================================

# 清除 Release 默认注入的 -O3/-Os/-DNDEBUG（插桩需要干净 -O0 与可断言环境）。
macro(_aurora_strip_optimization_flags)
    foreach (_v CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG
            CMAKE_C_FLAGS CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_DEBUG)
        string(REGEX REPLACE "-O[0-9s]" "" ${_v} "${${_v}}")
        string(REPLACE "-DNDEBUG" "" ${_v} "${${_v}}")
    endforeach ()
endmacro()

# 对当前目录所有已定义目标（静态库 aurora + 测试/demo/工具）统一注入插桩编译/链接选项。
# 必须覆盖测试目标：大量 widget 是 header-only，仅在测试 TU 中编译，若测试 exe 不插桩，
# 这些 widget 的覆盖率将完全缺失；共同插桩也避免 ASan annotate_string 取值不一致
# （=1 vs =0）导致 lld-link /failifmismatch 链接失败。
function(_aurora_instrument_all_targets)
    get_property(_tgts DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_tgt ${_tgts})
        # 跳过不可编译目标（add_custom_target 产生的 UTILITY 目标，如 aurora_api_json），
        # 否则 target_compile_options / target_link_options 会报 non-compilable target 错误。
        get_target_property(_type ${_tgt} TYPE)
        if (_type STREQUAL "UTILITY" OR _type STREQUAL "INTERFACE")
            continue()
        endif ()
        # PCH 与覆盖率行映射 / sanitizer 插桩常冲突：逐目标关闭。
        set_target_properties(${_tgt} PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
        target_compile_options(${_tgt} PRIVATE ${ARGV})
        target_link_options(${_tgt} PRIVATE ${ARGV})
    endforeach ()
endfunction()

# ---- 代码覆盖率（终端摘要，不生成 HTML） ----
# 按编译器分流（同一开关，工具链自适应）：
#   GCC   → gcov（--coverage，.gcda/.gcno）
#   Clang → LLVM 原生 source-based（-fprofile-instr-generate -fcoverage-mapping，.profraw）
# ⚠️ Clang 下禁止注入 --coverage：clang 的 gcov 兼容运行时（llvm_gcda_*）在 Windows
# 进程退出刷写 .gcda 时稳定崩溃（0xC0000005，关闭窗口/测试退出即触发）。
option(AURORA_ENABLE_COVERAGE "Build with coverage instrumentation (GCC gcov / Clang source-based)" OFF)
if (AURORA_ENABLE_COVERAGE)
    _aurora_strip_optimization_flags()
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        _aurora_instrument_all_targets(-fprofile-instr-generate -fcoverage-mapping -O0 -g)
    else ()
        _aurora_instrument_all_targets(--coverage -O0 -g)
    endif ()

    # 覆盖率终端摘要：Linux/macOS 走 gcov + tools/coverage_report.sh；
    # Windows 走 tools/coverage_report.ps1；Clang 走 LLVM 原生
    # source-based（llvm-profdata merge + llvm-cov report，tools/coverage_report_llvm.ps1）。
    # 用法：cmake -S . -B build -DAURORA_ENABLE_COVERAGE=ON ... && cmake --build build --target coverage
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # LLVM_PROFILE_FILE 用 %p（pid）模板：ctest 并行多进程各写独立 .profraw，不互相覆盖。
        get_filename_component(_aurora_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_target(coverage
                COMMAND ${CMAKE_COMMAND} -E env "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profraw/aurora-%p.profraw"
                ${CMAKE_CTEST_COMMAND} --output-on-failure
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/coverage_report_llvm.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -SrcRoot "${CMAKE_SOURCE_DIR}"
                -LlvmBin "${_aurora_llvm_bin}"
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                COMMENT "Running ctest then aggregating llvm-cov line coverage (terminal summary, no HTML)")
    else ()
        if (WIN32)
            add_custom_target(coverage
                    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/tools/coverage_report.ps1"
                    -BuildDir "${CMAKE_BINARY_DIR}"
                    -SrcRoot "${CMAKE_SOURCE_DIR}"
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                    COMMENT "Running ctest then aggregating gcov line coverage (terminal summary, no HTML)")
        else ()
            add_custom_target(coverage
                    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                    COMMAND bash "${CMAKE_SOURCE_DIR}/tools/coverage_report.sh"
                    "${CMAKE_BINARY_DIR}"
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                    COMMENT "Running ctest then aggregating gcov line coverage via coverage_report.sh (terminal summary, no HTML)")
        endif ()
    endif ()
endif ()

# ---- 内存错误检测（AddressSanitizer + UndefinedBehaviorSanitizer） ----
# 开启后注入 -fsanitize=address,undefined 插桩，运行时检测越界、use-after-free、
# 整数溢出、空指针解引用等。与 AURORA_ENABLE_COVERAGE 互斥（二者都改写代码生成）。
option(AURORA_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if (AURORA_ENABLE_ASAN)
    if (AURORA_ENABLE_COVERAGE)
        aurora_error("AURORA_ENABLE_COVERAGE and AURORA_ENABLE_ASAN are mutually exclusive. "
                "Choose one: turn off coverage or turn off ASan.")
    endif ()
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if (MINGW)
            aurora_warn("MinGW GCC AddressSanitizer support may be incomplete "
                    "(requires libasan DLL at runtime). If tests fail to start, "
                    "copy libasan.dll from MinGW bin/ or use clang-cl instead.")
        endif ()
        _aurora_strip_optimization_flags()
        set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
        _aurora_instrument_all_targets(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0)
        aurora_log("ASan/UBSan enabled (${CMAKE_CXX_COMPILER_ID}): -fsanitize=address,undefined")
    else ()
        aurora_warn("AURORA_ENABLE_ASAN=ON but compiler (${CMAKE_CXX_COMPILER_ID}) does not "
                "support -fsanitize. ASan is only available with GCC or Clang.")
    endif ()
endif ()

# ---- 渲染性能埋点 ----
# 分级策略：帧级指标（FrameStats/PerfOverlay）常开、无开关；
# 作用域计时（AURORA_PROFILE_SCOPE）与绘制计数（AURORA_PROFILE_COUNT）由本宏控制，
# 关闭时埋点宏展开为 ((void)0)，编译期彻底归零，发布态天然零开销。
#
# AURORA_ENABLE_PROFILING 是**三态**开关（AUTO / ON / OFF），默认 AUTO：
#   AUTO → Debug、RelWithDebInfo 定义宏；Release、MinSizeRel 不定义。
#          用生成式表达式而非 CMAKE_BUILD_TYPE 判断，使多配置生成器（MSVC/Xcode）
#          与单配置生成器（Ninja/Make）行为完全一致。
#   ON   → 恒定义（用于在 Release 下采集确定性计数类指标）
#   OFF  → 恒不定义（用于采集时间类门槛数据，排除观察者效应）
# ⚠️ 埋点宏只影响宏展开，不改变任何类/结构体的内存布局，故库与消费者取值不一致亦安全。
set(AURORA_ENABLE_PROFILING "AUTO" CACHE STRING
        "Fine-grained render profiling instrumentation: AUTO (Debug/RelWithDebInfo only) | ON | OFF")
set_property(CACHE AURORA_ENABLE_PROFILING PROPERTY STRINGS AUTO ON OFF)

string(TOUPPER "${AURORA_ENABLE_PROFILING}" _aurora_profiling_mode)
set(AURORA_PROFILING_FORCED_ON OFF)   # 供 AuroraInstall.cmake 决定是否随安装导出该宏
if (_aurora_profiling_mode STREQUAL "AUTO")
    target_compile_definitions(aurora PUBLIC
            $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:AURORA_ENABLE_PROFILING>)
    aurora_log("Render profiling: AUTO (ON for Debug/RelWithDebInfo, OFF for Release/MinSizeRel)")
elseif (AURORA_ENABLE_PROFILING)
    target_compile_definitions(aurora PUBLIC AURORA_ENABLE_PROFILING)
    set(AURORA_PROFILING_FORCED_ON ON)
    aurora_log("Render profiling: ON (forced for all configurations)")
else ()
    aurora_log("Render profiling: OFF (forced for all configurations)")
endif ()

# Chrome Trace Event 时间线导出：产出文件且开销显著，**两种构建下一律默认 OFF**。
# 打开即隐含强制打开 PROFILING（TraceWriter 消费 Profiler 的 zone 样本，否则无数据可写）。
option(AURORA_ENABLE_TRACING "Emit Chrome Trace Event timeline files (implies AURORA_ENABLE_PROFILING=ON)" OFF)
if (AURORA_ENABLE_TRACING)
    target_compile_definitions(aurora PUBLIC AURORA_ENABLE_TRACING)
    if (NOT AURORA_PROFILING_FORCED_ON)
        target_compile_definitions(aurora PUBLIC AURORA_ENABLE_PROFILING)
        set(AURORA_PROFILING_FORCED_ON ON)
        aurora_log("Render tracing: ON (AURORA_ENABLE_PROFILING forced ON as a prerequisite)")
    else ()
        aurora_log("Render tracing: ON")
    endif ()
endif ()

# ---- 真实后端 DEBUG 能力（DEBUG_BACKEND_PLAN.md） ----
# 三态开关（AUTO / ON / OFF），默认 AUTO，分级策略对齐 AURORA_ENABLE_PROFILING：
#   AUTO → Debug、RelWithDebInfo 注入宏；Release、MinSizeRel 不注入。
#   ON   → 恒注入（用于在 Release 下强制开启调试能力）。
#   OFF  → 恒不注入。
# 导出为 PUBLIC（而非 PRIVATE）：Widget 类在宏下新增数据成员（m_debug_paint_frame），会改变类 ABI 布局，
# 因此消费端（demo / tests / 宿主应用）必须与库使用同一宏取值，否则构造与读成员偏移错位 → ODR / 访问冲突。
# 自由函数式调试 API 本身仍按宏在 .cpp 裁切、关闭时返回 disabled（与宏取值无关，仅影响能否进入成功路径）。
set(AURORA_ENABLE_DEBUG "AUTO" CACHE STRING
        "Real-backend debug capabilities (framebuffer/OS-window screenshot, widget tree, perf snapshot, debug paint flags): AUTO (Debug/RelWithDebInfo only) | ON | OFF")
set_property(CACHE AURORA_ENABLE_DEBUG PROPERTY STRINGS AUTO ON OFF)

string(TOUPPER "${AURORA_ENABLE_DEBUG}" _aurora_debug_mode)
if (_aurora_debug_mode STREQUAL "AUTO")
    set(_aurora_debug_genex "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:AURORA_ENABLE_DEBUG>")
    target_compile_definitions(aurora PUBLIC ${_aurora_debug_genex})
    aurora_log("Backend debug: AUTO (ON for Debug/RelWithDebInfo, OFF for Release/MinSizeRel)")
elseif (AURORA_ENABLE_DEBUG)
    set(_aurora_debug_genex "AURORA_ENABLE_DEBUG")
    target_compile_definitions(aurora PUBLIC AURORA_ENABLE_DEBUG)
    aurora_log("Backend debug: ON (forced for all configurations)")
else ()
    set(_aurora_debug_genex "")
    aurora_log("Backend debug: OFF (forced for all configurations)")
endif ()

# 注：AURORA_ENABLE_DEBUG 现已作为 aurora 的 PUBLIC 编译定义导出，所有链接 aurora 的消费者
# （test / demo / 宿主应用）都会自动获得与库完全一致的宏取值，故无需再为测试目标单独注入。
