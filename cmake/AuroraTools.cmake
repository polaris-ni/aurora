# ============================================================
# AuroraTools.cmake — 工具与基准可执行目标（tools/ 下，均 PRIVATE 链接 aurora）
# ------------------------------------------------------------
# aurora_add_tool(<name> <source> [注释见各调用处]) 统一样板：
# add_executable + link aurora + C++20 + 暴露 tools/include 共享头。
#
# 目录约定（重构后 tools/ 按职责分子目录，详见 AGENTS.md §2）：
#   gen/      构建期生成器（gen_error_codes / gen_debug_api 不链接 aurora）
#   servers/  常驻与命令行服务端（MCP / LSP / CLI / au-lint）
#   bench/    性能基准（共享头 bench_common.h 与本目录同级）
#   check/    静态校验与门禁脚本（.py / .ps1）
#   coverage/ 覆盖率聚合脚本
#   include/  跨工具共享 C++ 头（由本函数统一注入搜索路径）
# ============================================================

function(aurora_add_tool _name _src)
    add_executable(${_name} ${_src})
    # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志（见 AuroraUtils.cmake）。
    aurora_setup_consumer_target(${_name})
    # 共享头搜索路径：tools/include 下的 known_enums.h / jsonrpc_io.h / api_schema.h
    # 等被多个工具复用，统一注入避免逐个目标手写。
    target_include_directories(${_name} PRIVATE "${CMAKE_SOURCE_DIR}/tools/include")
endfunction()

# 错误码生成器：解析 codespec/errors.toml -> error_codes.gen.h / ERROR_CATALOG.md /
# aurora_api.json 的 "error_codes" 段。独立可执行文件，**不链接 aurora**（避免鸡生蛋，
# 因为 aurora 自身包含生成的头）；仅依赖 third_party 的 nlohmann/json。
add_executable(gen_error_codes tools/gen/gen_error_codes.cpp)
target_include_directories(gen_error_codes PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        # toml_lines.h / api_json_merge.h 为「零 aurora 依赖」共享头，供本生成器复用。
        ${CMAKE_SOURCE_DIR}/tools/include)
set_target_properties(gen_error_codes PROPERTIES CXX_STANDARD 20)

# 从 errors.toml 重新生成头/目录/API（仅在 errors.toml 变更时触发）。
add_custom_command(
        OUTPUT ${CMAKE_SOURCE_DIR}/include/aurora/core/error_codes.gen.h
        ${CMAKE_SOURCE_DIR}/codespec/ERROR_CATALOG.md
        ${CMAKE_SOURCE_DIR}/aurora_api.json
        COMMAND $<TARGET_FILE:gen_error_codes>
        "${CMAKE_SOURCE_DIR}/codespec/errors.toml"
        "${CMAKE_SOURCE_DIR}/include/aurora/core/error_codes.gen.h"
        "${CMAKE_SOURCE_DIR}/codespec/ERROR_CATALOG.md"
        "${CMAKE_SOURCE_DIR}/aurora_api.json"
        DEPENDS $<TARGET_FILE:gen_error_codes> ${CMAKE_SOURCE_DIR}/codespec/errors.toml
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating error_codes.gen.h / ERROR_CATALOG.md / aurora_api.json from errors.toml"
        VERBATIM)
add_custom_target(generate_error_codes DEPENDS
        ${CMAKE_SOURCE_DIR}/include/aurora/core/error_codes.gen.h
        ${CMAKE_SOURCE_DIR}/codespec/ERROR_CATALOG.md
        ${CMAKE_SOURCE_DIR}/aurora_api.json)
add_dependencies(aurora generate_error_codes)

# 工具：反射 aurora 公共 API 生成 aurora_api.json。
aurora_add_tool(gen_api_tools tools/gen/gen_api.cpp)

# 可复现生成：运行 gen_api_tools 直接写 aurora_api.json（跨平台，无需 shell 重定向）。
# 运行：cmake --build build --target aurora_api_json
add_custom_target(aurora_api_json
        COMMAND gen_api_tools "${CMAKE_SOURCE_DIR}/aurora_api.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating aurora_api.json from current public API (gen_api_tools)"
        VERBATIM)
add_dependencies(aurora_api_json gen_api_tools)

# 配套：调试能力 API 生成器。解析 codespec/debug_api.toml -> aurora_api.json 的 "debug" 段。
# 独立可执行文件，**不链接 aurora**（与 gen_error_codes 同构），仅依赖 third_party 的 nlohmann/json。
# merge-only：保留其它段，只写 debug 段。运行：cmake --build build --target gen_debug_api_json
add_executable(gen_debug_api tools/gen/gen_debug_api.cpp)
target_include_directories(gen_debug_api PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/tools/include)
set_target_properties(gen_debug_api PROPERTIES CXX_STANDARD 20)
add_custom_target(gen_debug_api_json
        COMMAND gen_debug_api "${CMAKE_SOURCE_DIR}/codespec/debug_api.toml" "${CMAKE_SOURCE_DIR}/aurora_api.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating aurora_api.json 'debug' section from debug_api.toml (gen_debug_api)"
        VERBATIM)
add_dependencies(gen_debug_api_json gen_debug_api)

# 工具：序列化 UI 树结构化检查器（独立 CLI 二进制，非 MCP/CLI 暴露）。
aurora_add_tool(au-lint tools/servers/au-lint.cpp)

# MCP Server：stdio JSON-RPC 2.0，供 AI Agent 直接调用。
aurora_add_tool(aurora_mcp tools/servers/aurora_mcp.cpp)

# CLI 工具：组件发现 / 校验 / 离屏渲染 / 代码生成。
aurora_add_tool(aurora_cli tools/servers/aurora_cli.cpp)

# LSP 语言服务：stdio JSON-RPC 2.0，提供 completion/hover/diagnostics/codeAction。
aurora_add_tool(aurora_lsp tools/servers/aurora_lsp.cpp)

# 注：原 tools/ai_compat_test（AI 兼容性批量验证可执行）已移除 —— 其 fixture 管线
# （from_json → validate_ui → to_code）由 tests/test_ai_compat.cpp 完整覆盖，且后者
# 改为目录遍历后是前者的超集（另含纯内存用例）。保留两份属重复实现。

# 渲染基准：HeadlessSurface + Painter 多矩阵计时（非 CTest 断言，仅性能基线）。
aurora_add_tool(bench_render tools/bench/bench_render.cpp)

# 滚动基准：ScrollBenchHarness 在 Headless 下跑确定性滚动序列，产出 §7 基线。
# 需要 examples/app/google_play 的头（google_play_ui.h）——基线口径规定打在真实业务树上而非合成树；
# 该头 header-only 且随仓库分发，不引入额外构建依赖。
aurora_add_tool(bench_scroll tools/bench/bench_scroll.cpp)
target_include_directories(bench_scroll PRIVATE "${CMAKE_SOURCE_DIR}/examples/app/google_play")

# Win32 上屏诊断基准：拆分拖选帧的 paint / GDI blit 成本（非 CTest；无 Win32 后端时直接跳过）。
aurora_add_tool(bench_win32_present tools/bench/bench_win32_present.cpp)

# 空闲 CPU 基准：验证事件驱动帧循环的静态界面 CPU 占比与
# 活跃帧 max_fps 节流（非 CTest；无 Win32 后端时直接跳过）。
aurora_add_tool(bench_idle_cpu tools/bench/bench_idle_cpu.cpp)

# Phase 4 本机时间类门槛校验（check_perf_gates.ps1）：门槛已外置为 tools/check/perf_gates.json。
# 仅 Windows 有 bench 上屏基准可执行；pwsh 缺失时跳过（不阻断构建/CI）。
# 说明：时间类门槛受环境抖动影响，不进 CTest；本目标供本机趋势对比，可选运行。
if (WIN32)
    find_program(PWSH_EXE NAMES pwsh powershell)
    if (PWSH_EXE)
        add_custom_target(perf_gates
                COMMAND ${PWSH_EXE}
                        "${CMAKE_SOURCE_DIR}/tools/check/check_perf_gates.ps1"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                COMMENT "Phase 4 本机时间类门槛校验（不进 CI，仅本机趋势对比）"
                VERBATIM)
    endif ()
endif ()

# Inspector 远程 HTTP 接口（可选，跨平台）：localhost-only HTTP 服务器，
# 暴露 REST 端点供外部 Inspector 工具访问 widget 树。
# inspector_server.cpp 编入独立静态库 aurora_inspector_server（不编入核心 aurora 库），
# 避免未开启开关时引入平台 socket 依赖（Windows: ws2_32；POSIX: pthread）。
option(AURORA_BUILD_INSPECTOR_SERVER "Build Inspector remote HTTP server" OFF)
if (AURORA_BUILD_INSPECTOR_SERVER)
    add_library(aurora_inspector_server STATIC
            src/aurora/inspector/inspector_server.cpp
    )
    if (WIN32)
        target_link_libraries(aurora_inspector_server PUBLIC aurora ws2_32)
    else ()
        find_package(Threads REQUIRED)
        target_link_libraries(aurora_inspector_server PUBLIC aurora Threads::Threads)
    endif ()
    target_include_directories(aurora_inspector_server PUBLIC include)
    target_compile_definitions(aurora_inspector_server PUBLIC AURORA_INSPECTOR_SERVER_ENABLED)
    set_target_properties(aurora_inspector_server PROPERTIES CXX_STANDARD 20)
    aurora_log("Inspector HTTP server enabled (aurora_inspector_server static lib)")
endif ()
