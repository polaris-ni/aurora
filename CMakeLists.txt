# ============================================================
# Aurora — AI-first modern C++ GUI library (v1.0.0-alpha.1)
# ------------------------------------------------------------
# 本文件只做「工程声明 + 核心库目标 + 模块编排」；具体逻辑按职责拆分在 cmake/ 下：
#   cmake/AuroraThirdParty.cmake      — FreeType/HarfBuzz 源码构建
#   cmake/AuroraBackends.cmake        — 后端剪裁开关（Headless/Win32/D3D11/GLFW/X11/Wayland/macOS/WASM）+ P3 优化宏
#   cmake/AuroraTools.cmake           — 工具/基准可执行（gen_api/MCP/CLI/LSP/bench_* 等）
#   cmake/AuroraTests.cmake           — CTest 注册式 runner（GLOB tests/*.cpp 链入单一 aurora_test_runner，AURORA_TEST() 自注册）
#   cmake/AuroraDemos.cmake           — 示例 demo 目标（examples/demos/ 下每组件一个可运行窗口；须在 AuroraTools 之后 include）
#   cmake/AuroraInstrumentation.cmake — 覆盖率/ASan 插桩（须在全部目标定义之后）
#   cmake/AuroraInstall.cmake         — 安装 + find_package(Aurora) 导出
# 全部 CMake 开关/宏/环境变量统一列于 codespec/BUILD_OPTIONS.md（唯一权威来源）。
# 注意：include() 不创建新作用域，各模块内 option/目标定义与写在本文件时完全等价。
# ============================================================
cmake_minimum_required(VERSION 3.20)

project(aurora
        VERSION 1.0.0
        DESCRIPTION "AI-first modern C++ cross-platform GUI library"
        LANGUAGES CXX C)

# 预发布后缀（semver pre-release）。project(VERSION) 仅接受纯数字，故 alpha 标识
# 单独存放：完整版本串 = ${PROJECT_VERSION}-${AURORA_VERSION_SUFFIX}（如 1.0.0-alpha.1），
# 由 include/aurora/core/version.h 经 AURORA_VERSION_STRING 暴露给程序。
set(AURORA_VERSION_SUFFIX "alpha.1" CACHE STRING
        "Pre-release suffix appended after PROJECT_VERSION (empty for stable releases)")

# ============================================================
# 统一配置期日志前缀：BUILD AURORA:
# 便于从海量 CMake 输出中过滤 Aurora 自身诊断信息（详见 cmake/ 下各模块调用）。
# ============================================================
function(aurora_log _msg)
    # STATUS 状态行：输出形态为 `-- BUILD AURORA: <msg>`。
    message(STATUS "BUILD AURORA: ${_msg}")
endfunction()
function(aurora_warn _msg)
    message(WARNING "BUILD AURORA: ${_msg}")
endfunction()
function(aurora_error _msg)
    # 复用 FATAL_ERROR，保证错误中断与退出码不变。
    message(FATAL_ERROR "BUILD AURORA: ${_msg}")
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json 供 clangd / IDE 索引（不影响构建结果）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 依赖扫描：Makefiles 生成器下启用编译器级依赖分析，依赖变更判定更准、减少误重编。
if (CMAKE_GENERATOR MATCHES "Makefiles")
    set(CMAKE_DEPENDS_USE_COMPILER ON)
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# 生成器建议：Makefiles 默认非并行，推荐 Ninja 以最大化编译吞吐（见 CMakePresets.json）。
if (CMAKE_GENERATOR MATCHES "Makefiles")
    find_program(AURORA_NINJA_HINT NAMES ninja ninja-build)
    if (AURORA_NINJA_HINT)
        aurora_log("Tip: Ninja available — use `-G Ninja` or `cmake --preset ninja` for parallel builds")
    endif ()
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---- ccache 编译缓存（加速重复编译）----
include(AuroraCcache)

# ---- 三方字体依赖（FreeType + HarfBuzz 源码构建）----
include(AuroraThirdParty)

# NOMINMAX：抑制 <windows.h> 的 min/max 宏，保证 std::min/std::max 在 Windows 上可用
# （preferences.cpp 等直接 include <windows.h> 并使用 std::max；第三方库同样需要，故保持全局定义）。
# 告警标志改为 aurora 目标级（见下方 add_library(aurora ...) 之后），不再全局注入，
# 故三方源码 include 顺序不再敏感。
add_compile_definitions(NOMINMAX)

# ---- 链接器：优先 LLVM lld（构建提速） ----
# 本项目 250+ 可执行目标全部静态链接 libaurora.a + freetype/harfbuzz，GNU ld 是
# 增量循环（改一个测试 → 编译+链接）的主要成本之一；lld 对 PE/COFF 链接通常快数倍。
# 找不到 ld.lld 或链接检查失败时静默回退 GNU ld（不影响构建正确性）；
# 显式关闭用 -DAURORA_ENABLE_LLD=OFF，非 PATH 安装用 -DAURORA_LLD_DIR=<LLVM bin 目录> 提示。
option(AURORA_ENABLE_LLD "Link with LLVM lld if available (much faster than GNU ld)" ON)
if (AURORA_ENABLE_LLD AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(AURORA_LLD_DIR "" CACHE PATH "ld.lld 所在目录提示（未在 PATH 时使用）")
    find_program(AURORA_LLD_EXECUTABLE NAMES ld.lld HINTS ${AURORA_LLD_DIR})
    if (AURORA_LLD_EXECUTABLE)
        get_filename_component(_aurora_lld_dir "${AURORA_LLD_EXECUTABLE}" DIRECTORY)
        include(CheckLinkerFlag)
        set(CMAKE_REQUIRED_LINK_OPTIONS "-B${_aurora_lld_dir}")
        check_linker_flag(CXX "-fuse-ld=lld" AURORA_LINKER_LLD_WORKS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        if (AURORA_LINKER_LLD_WORKS)
            add_link_options(-fuse-ld=lld "-B${_aurora_lld_dir}")
            aurora_log("Linker: lld (${AURORA_LLD_EXECUTABLE})")
        else ()
            aurora_log("Linker: GNU ld (lld found but link check failed)")
        endif ()
    else ()
        aurora_log("Linker: GNU ld (ld.lld not found; set AURORA_LLD_DIR to enable lld)")
    endif ()
endif ()

# ---- Library target (static) ----
file(GLOB_RECURSE AURORA_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/aurora/*.cpp")

# inspector_server.cpp 编入独立静态库 aurora_inspector_server（仅 AURORA_BUILD_INSPECTOR_SERVER=ON 时构建），
# 不编入核心 aurora 库，避免未开启开关时引入 Winsock2 依赖（见 cmake/AuroraTools.cmake）。
list(FILTER AURORA_SOURCES EXCLUDE REGEX "inspector_server\\.cpp$")

add_library(aurora STATIC ${AURORA_SOURCES})
# 版本常量注入：project(VERSION) + AURORA_VERSION_SUFFIX → version.h 的
# AURORA_VERSION_* / AURORA_VERSION_STRING（PUBLIC：消费者与测试同源取值）。
set(AURORA_VERSION_HAS_SUFFIX 0)
if (NOT AURORA_VERSION_SUFFIX STREQUAL "")
    set(AURORA_VERSION_HAS_SUFFIX 1)
endif ()
target_compile_definitions(aurora PUBLIC
        AURORA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        AURORA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        AURORA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        "AURORA_VERSION_SUFFIX_STR=\"${AURORA_VERSION_SUFFIX}\""
        AURORA_HAS_VERSION_SUFFIX=${AURORA_VERSION_HAS_SUFFIX})
target_include_directories(aurora
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        # vendored 第三方（nlohmann/json 由 aurora.h 传递包含，故需公开暴露）。
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        PRIVATE
        # 仅库实现可见的内部共享头（swizzle/keysym 等），不暴露给消费者。
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(aurora PUBLIC cxx_std_20)
# Warnings（mirrors CODING_STANDARDS.md §10）——aurora 自身目标级，不污染三方库与消费者目标。
# -Wno-missing-field-initializers：本库大量采用聚合 Props 的部分初始化（如 ButtonProps{...} 仅给
# 部分字段赋值），其余字段本就会被值初始化为零，该告警纯属噪音，故关闭。
target_compile_options(aurora PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
# FreeType：PUBLIC 链接使任意消费者（demo/测试/工具）静态链入，无需自行配置。
target_link_libraries(aurora PUBLIC freetype harfbuzz)

# ---- 图片编解码器外部库（libjpeg-turbo / libwebp / wuffs）----
# ⚠️ 必须在 add_library(aurora ...) 之后 include：本模块内部以 target_link_libraries(aurora ...)
# 把各 codec 的 OBJECT 库链入 aurora，若 aurora 目标尚未创建会报 "not built by this project"。
include(AuroraImageCodecs)

# 三方库 harfbuzz 从源码编译（src/harfbuzz.cc 合并源），其 hb-algs.hh:1146 的 memcmp 边界分析
# 在 -O2/-O3 内联展开时误报 -Wstringop-overread（"exceeds source size"）。该告警对三方代码
# 无意义，故整体对 harfbuzz target 关闭（不限于某 TU）；aurora 自有代码仍保留 -Wstringop-overread。
# 注：告警来自 harfbuzz 自身编译单元，与本项目是否 include 其头无关，故置于 target 级而非源文件级。
target_compile_options(harfbuzz PRIVATE "-Wno-stringop-overread")

# ---- Precompiled header (规格 §3.5 增量编译) ----
# 库自身 PCH：只聚合「稳定且不常变更」的重型头（标准库 + nlohmann/json），不纳入高频
# 变更的 aurora 自有头以保证库开发时的命中率（见 aurora_pch.h 收录原则）；PRIVATE 仅
# aurora 库编译一份 .gch。消费者（demo/测试/工具）另有含 aurora.h 的共享 PCH，见下方
# aurora_consumer_pch 目标（历史上 PUBLIC + 空 PCH：每目标一份空 .gch，纯开销零收益）。
target_precompile_headers(aurora PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/aurora/aurora_pch.h")

# 抑制 MSVC 对 std::fopen 等 POSIX 函数的弃用警告（render/png.h 使用 std::fopen）
if (MSVC)
    target_compile_definitions(aurora PUBLIC _CRT_SECURE_NO_WARNINGS)
endif ()

# ---- Debug 构建优化级别：aurora 库 + 第三方库用 -O3 取代默认 -O0（hybrid debug） ----
# 默认 CMake Debug 为 -g -O0，渲染热点（库内光栅内核 + 第三方文本光栅 freetype）在 -O0 下
# 比 Release(-O3) 慢数倍，导致 demo/测试在 Debug 下卡顿且难以排查真实性能问题。
# 实测 demo_google_play（真实 Win32 后端）帧率拆分：paint 占 ~85% 帧时间，其中文本光栅
# （freetype，第三方子项目，Debug 下默认 -O0 编成 libfreetyped.a）是主要热点。
# 仅给 aurora 库加 -O3 只能把 Debug 从 ~15 抬到 ~30（paint 仍被 -O0 的 freetype 拖死），
# 故对 aurora 库 + freetype/harfbuzz 等第三方库一并施加 -O3（保留 -g 调试信息）：
# 实测把 demo_google_play 的 Debug 帧率从 ~15 抬到接近 Release 的 ~50 FPS（与 build/ 对齐）。
# 折中方案（hybrid debug）：aurora 库与第三方库 -O3，demo/测试等消费者目标仍按 -O0 编译，
# 保证你能在自己的 demo 代码里正常单步/断点，而库内光栅热点与第三方文本光栅得到优化。
# 注：freetype 在 Debug 下的逻辑 target 仍为 `freetype`（输出名 libfreetyped.a，带 debug postfix），
#     此处直接作用于其逻辑 target 即可。harfbuzz 同理。
# 覆盖率/ASan 模式会强制 -O0（见 AuroraInstrumentation 的 _aurora_instrument_all_targets 注入），
# 那里以 target_compile_options 追加的 -O0 在命令行末尾生效，覆盖本处的 -O3，无冲突。
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT AURORA_ENABLE_COVERAGE AND NOT AURORA_ENABLE_ASAN)
    target_compile_options(aurora PRIVATE -O3)
    # 第三方库（freetype/harfbuzz）在 Debug 下默认 -O0 编成无优化版本（libfreetyped.a 等），
    # 其文本/shaping 光栅位于 paint 热路径，必须一并抬到 -O3 才能真正释放 Debug 帧率。
    # 这些 target 由 AuroraThirdParty.cmake 经 add_subdirectory 引入，此处直接作用于其逻辑 target。
    foreach (_tp freetype harfbuzz)
        if (TARGET ${_tp})
            target_compile_options(${_tp} PRIVATE -O3)
            # freetype/harfbuzz 在 CMAKE_BUILD_TYPE=Debug 下不会自动获得 NDEBUG，
            # 其内部 FT_ASSERT / 调试检查会在文本/shaping 光栅热路径上每帧执行，
            # 即便 -O3 也仍生效，导致 Debug 帧率被拖到 Release 的一半。
            # 这里显式补 NDEBUG 关闭断言，配合 -O3 真正释放 Debug 帧率。
            target_compile_definitions(${_tp} PRIVATE NDEBUG)
        endif ()
    endforeach ()
    aurora_log("Debug optimization: -O3 on aurora lib + third-party (freetype/harfbuzz) (hybrid debug; replaces default -O0)")
endif ()

# ---- 后端剪裁开关 + 优化宏（作用于上方 aurora 目标） ----
include(AuroraBackends)

# ---- 光栅内核 SIMD 双实现开关 ----
include(AuroraSimd)

# ---- 消费者共享 PCH（demo/测试/工具用） ----
# 消费者 TU 的最大解析成本是 aurora.h 伞头（全部 widget/render 公共头）。aurora 库自身
# 的 PCH 刻意不含库内头（保证库开发时命中率），但对消费者而言 aurora.h 变更本就会
# 触发其 TU 重编，把它整体预编译并共享**不增加任何失效面**，只省去 250+ TU 的重复解析：
# 锚定到一个空静态库目标编一份 .gch，全部消费者经 REUSE_FROM 复用。
# ⚠️ 插桩构建（覆盖率/ASan）强制关闭 PCH（见 AuroraInstrumentation）：此时须跳过锚定目标
# 与全部 REUSE_FROM，否则 CMake 会对「REUSE_FROM 一个 DISABLE_PRECOMPILE_HEADERS 锚点」
# 报 "ignoring" 警告。AURORA_PCH_ENABLED 为单一真相变量，供下方 demo/测试/工具复用。
set(AURORA_PCH_ENABLED ON)
if (AURORA_ENABLE_COVERAGE OR AURORA_ENABLE_ASAN)
    set(AURORA_PCH_ENABLED OFF)
endif ()
if (AURORA_PCH_ENABLED)
    file(WRITE "${CMAKE_BINARY_DIR}/aurora_consumer_pch.cpp"
            "// 自动生成：消费者共享 PCH 的锚定 TU（无内容，仅承载 .gch 编译）。\n")
    add_library(aurora_consumer_pch STATIC "${CMAKE_BINARY_DIR}/aurora_consumer_pch.cpp")
    target_link_libraries(aurora_consumer_pch PUBLIC aurora)
    # 显式钉 C++20：消费者经 aurora_setup_consumer_target 统一设 CXX_STANDARD 20，此处须一致，
    # 否则锚点用编译器默认标准（可能因全局 CMAKE_CXX_STANDARD 切换而偏离）→ 预定义宏
    # （如 __cpp_runtime_arrays，仅 C++17/20 定义）与消费 TU 不符，触发 -Winvalid-pch 回退。
    set_target_properties(aurora_consumer_pch PROPERTIES CXX_STANDARD 20)
    target_precompile_headers(aurora_consumer_pch PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/aurora/aurora.h")
endif ()

# ---- 消费者目标统一配置（demo/测试/工具复用）----
include(AuroraUtils)

# ---- 工具/基准 + 测试 ----
include(AuroraTools)
include(AuroraTests)

# ---- 示例 demo（examples/demos/ 下每组件一个可运行窗口；独立模块，须在 AuroraTools 之后 include）----
include(AuroraDemos)

# ---- 插桩（覆盖率 / ASan；须在全部目标定义之后，对所有目标统一注入）----
include(AuroraInstrumentation)

# ---- 安装 + find_package(Aurora) 导出（须在后端开关之后，导出宏取最终取值）----
include(AuroraInstall)

# ---- Clang-Tidy 门禁（按需 lint / lint-fix 聚合目标；须在全部目标定义之后 include）----
include(AuroraLint)

# ---- MinGW 可执行文件自包含运行期链接 ----
# 默认 MinGW 构建的 exe 动态依赖 libgcc_s_seh-1.dll / libwinpthread-1.dll / libstdc++-6.dll。
# 当启动环境 PATH 中混入版本不兼容的同名 DLL 时，exe 在加载期报
# 0xC0000139(STATUS_ENTRYPOINT_NOT_FOUND) 而崩溃。
# 静态链接这三个运行期库，使 exe 完全自包含、彻底摆脱 PATH DLL 版本错配；
# 仅对 MinGW 下的可执行目标生效，不影响静态库(aurora)与 Linux/macOS 构建。
# 注意：BUILDSYSTEM_TARGETS 的 DIRECTORY 属性在 configure 期即可用（GLOBAL 属性要到
# generate 期才填充），故此处用 DIRECTORY 遍历当前目录定义的所有目标。
if (MINGW)
    get_property(_aurora_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_t ${_aurora_all_targets})
        get_target_property(_t_type ${_t} TYPE)
        if (_t_type STREQUAL "EXECUTABLE")
            target_link_options(${_t} PRIVATE -static-libgcc -static-libstdc++)
            # 静态 libwinpthread.a 依赖 GCC 内置符号 __intrinsic_setjmpex，lld 无法
            # 解析该符号，故仅在使用 GNU ld 时静态链接 pthread；lld 下回退为动态链接。
            if (NOT AURORA_LINKER_LLD_WORKS)
                target_link_options(${_t} PRIVATE -Wl,-Bstatic -lpthread -Wl,-Bdynamic)
            else ()
                target_link_options(${_t} PRIVATE -lpthread)
            endif ()
        endif ()
    endforeach ()
endif ()
