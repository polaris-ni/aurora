# ============================================================
# AuroraLint.cmake — Clang-Tidy 门禁（按需聚合目标，不进默认构建）
# ------------------------------------------------------------
# 提供两个目标：
#   lint      — 对 compile_commands.json 中全部**非 third_party**翻译单元跑 clang-tidy；
#               结果按 (file, line, check) 去重后统计，存在 warning 及以上即退出码 1。
#   lint-fix  — 同上但附加 --fix，就地应用 fix-it，且**不因告警失败**（便于人工审阅 diff）。
#
# 为什么不直接用 CMAKE_CXX_CLANG_TIDY 随构建执行：
#   1. 该变量必须在目标定义**之前**设置才生效，与本项目「模块在最后 include」的编排冲突；
#   2. 会让每次编译额外跑一遍 clang-tidy，日常开发构建被拖慢一个数量级；
#   3. 头文件诊断会在每个包含它的 TU 中重复上报，原始条数无法直接作为门禁计数。
# 因此改为独立目标 + tools/check/run_clang_tidy.py（并行 / 去重 / 排除 third_party）。
#
# ⚠️ 依赖 CMAKE_EXPORT_COMPILE_COMMANDS（clang-tidy -p 需要编译数据库）。未显式指定时
#    本模块会自动打开——它只影响生成期产物，不改变任何编译结果。
#
# ⚠️ Emscripten 配置下自动附加 --emscripten：em++ 写的编译库需先重写成 native clang-tidy
#    可消费的形态（wasm 三元组 + sysroot、弃 PCH），否则浏览器专属 TU 一律编译不过、
#    门禁静默报 0 告警。覆盖面的口径差异见 codespec/BUILD_OPTIONS.md §4.5。
#
# ⚠️ 本模块须在 tools 目录可访问时 include（脚本路径基于 CMAKE_SOURCE_DIR，与目标无关，
#    故放在最后 include 亦可安全 return() 跳过）。
# ============================================================

option(AURORA_ENABLE_CLANG_TIDY "Provide the 'lint' / 'lint-fix' aggregate targets (clang-tidy)" ON)
if (NOT AURORA_ENABLE_CLANG_TIDY)
    aurora_log("Clang-Tidy: disabled (AURORA_ENABLE_CLANG_TIDY=OFF)")
    return ()
endif ()

find_program(AURORA_CLANG_TIDY_EXE NAMES clang-tidy)
find_program(PYTHON3_EXE NAMES python3 python)

if (NOT AURORA_CLANG_TIDY_EXE)
    aurora_warn("AURORA_ENABLE_CLANG_TIDY=ON but clang-tidy was not found on PATH; 'lint' target skipped.")
    return ()
endif ()
if (NOT PYTHON3_EXE)
    aurora_warn("AURORA_ENABLE_CLANG_TIDY=ON but no python interpreter found; 'lint' target skipped.")
    return ()
endif ()

set(_lint_script "${CMAKE_SOURCE_DIR}/tools/check/run_clang_tidy.py")
if (NOT EXISTS "${_lint_script}")
    aurora_warn("Clang-Tidy: runner script missing (${_lint_script}); 'lint' target skipped.")
    return ()
endif ()

if (NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

# Emscripten 配置下，本目录的编译库由 em++ 驱动写出（三元组/垫片宏/PCH 都在驱动内部注入），
# clang-tidy 不能直接消费：加 --emscripten 让 runner 先把它重写成 native 可跑的形态。
# 这样 `cmake --build build-wasm --target lint` 才真的覆盖浏览器专属 TU 与
# `#ifdef AURORA_BACKEND_WASM` 分支——否则 wasm 侧的 lint 目标会扫到一批编译不过的 TU 而报 0 告警。
set(_lint_args "")
if (EMSCRIPTEN)
    list(APPEND _lint_args --emscripten)
endif ()

# `lint` 始终把结构化清单写到本目录的 lint-findings.json（tu_count / unique_findings /
# broken_tus / by_check / by_file / findings）。门禁 stdout 只有 top-N 文件表，CI 上据此
# 判因必然漏掉尾数；有了这份 JSON，聚合产物才能作为「按文件 + check 逐条列名」的依据。
add_custom_target(lint
        COMMAND ${PYTHON3_EXE} "${_lint_script}" --build-dir "${CMAKE_BINARY_DIR}" ${_lint_args}
                --json-out "${CMAKE_BINARY_DIR}/lint-findings.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Clang-Tidy: linting non-third_party TUs (deduplicated; fails on any finding)")

add_custom_target(lint-fix
        COMMAND ${PYTHON3_EXE} "${_lint_script}" --build-dir "${CMAKE_BINARY_DIR}" ${_lint_args} --fix
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Clang-Tidy: applying fix-its in place (review the diff before committing)")

aurora_log("Clang-Tidy: 'lint' / 'lint-fix' targets available (${AURORA_CLANG_TIDY_EXE})")
