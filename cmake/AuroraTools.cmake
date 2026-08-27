# ============================================================
# AuroraTools.cmake — 工具与基准可执行目标（tools/ 下，均 PRIVATE 链接 aurora）
# ------------------------------------------------------------
# aurora_add_tool(<name> <source> [注释见各调用处]) 统一样板：
# add_executable + link aurora + C++20。
# ============================================================

function(aurora_add_tool _name _src)
    add_executable(${_name} ${_src})
    # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志（见 AuroraUtils.cmake）。
    aurora_setup_consumer_target(${_name})
endfunction()

# 错误码生成器：解析 codespec/errors.toml -> error_codes.gen.h / ERROR_CATALOG.md /
# aurora_api.json 的 "error_codes" 段。独立可执行文件，**不链接 aurora**（避免鸡生蛋，
# 因为 aurora 自身包含生成的头）；仅依赖 third_party 的 nlohmann/json。
add_executable(gen_error_codes tools/gen_error_codes.cpp)
target_include_directories(gen_error_codes PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
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
aurora_add_tool(gen_api_tools tools/gen_api.cpp)

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
add_executable(gen_debug_api tools/gen_debug_api.cpp)
target_include_directories(gen_debug_api PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
set_target_properties(gen_debug_api PROPERTIES CXX_STANDARD 20)
add_custom_target(gen_debug_api_json
        COMMAND gen_debug_api "${CMAKE_SOURCE_DIR}/codespec/debug_api.toml" "${CMAKE_SOURCE_DIR}/aurora_api.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating aurora_api.json 'debug' section from debug_api.toml (gen_debug_api)"
        VERBATIM)
add_dependencies(gen_debug_api_json gen_debug_api)

# 工具：序列化 UI 树结构化检查器（独立 CLI 二进制，非 MCP/CLI 暴露）。
aurora_add_tool(au-lint tools/au-lint.cpp)

# MCP Server：stdio JSON-RPC 2.0，供 AI Agent 直接调用。
aurora_add_tool(aurora_mcp tools/aurora_mcp.cpp)

# CLI 工具：组件发现 / 校验 / 离屏渲染 / 代码生成。
aurora_add_tool(aurora_cli tools/aurora_cli.cpp)

# LSP 语言服务：stdio JSON-RPC 2.0，提供 completion/hover/diagnostics/codeAction。
aurora_add_tool(aurora_lsp tools/aurora_lsp.cpp)

# AI 兼容性批量验证工具：遍历 fixture 文件并执行 from_json → validate_ui → to_code 管线。
aurora_add_tool(ai_compat_test tools/ai_compat_test.cpp)

# 渲染基准：HeadlessSurface + Painter 多矩阵计时（非 CTest 断言，仅性能基线）。
aurora_add_tool(bench_render tools/bench_render.cpp)

# 滚动基准：ScrollBenchHarness 在 Headless 下跑确定性滚动序列，产出 §7 基线。
# 需要 examples/demos 的头（google_play_ui.h）——基线口径规定打在真实业务树上而非合成树；
# 该头 header-only 且随仓库分发，不引入额外构建依赖。
aurora_add_tool(bench_scroll tools/bench_scroll.cpp)
target_include_directories(bench_scroll PRIVATE "${CMAKE_SOURCE_DIR}/examples/demos")

# Win32 上屏诊断基准：拆分拖选帧的 paint / GDI blit 成本（非 CTest；无 Win32 后端时直接跳过）。
aurora_add_tool(bench_win32_present tools/bench_win32_present.cpp)

# 空闲 CPU 基准（CPU 性能专项阶段 A5）：验证事件驱动帧循环的静态界面 CPU 占比与
# 活跃帧 max_fps 节流（非 CTest；无 Win32 后端时直接跳过）。
aurora_add_tool(bench_idle_cpu tools/bench_idle_cpu.cpp)

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
