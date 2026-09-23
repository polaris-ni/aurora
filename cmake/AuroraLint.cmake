# ============================================================
# AuroraLint.cmake — Clang-Tidy 门禁（按需聚合目标，不进默认构建）
# ------------------------------------------------------------
# 提供两个目标：
#   lint      — 对 compile_commands.json 中全部**非 third_party**翻译单元跑 clang-tidy；
#               结果按 (file, line, check) 去重后统计，存在 warning 及以上即退出码 1。
#   lint-fix  — 同上但附加 --fix，就地应用 fix-it，且**不因告警失败**（便于人工审阅 diff）。
#
# 分片（AURORA_LINT_SHARD）：runner 侧 `--shard=i/n` 把已排序的 TU 清单切成 n 份、本目标只跑
# 第 i 份。存在的唯一理由是**墙钟**：CI runner 只有 4 vCPU，一遍 498 TU 实测 5172s（86 分钟），
# 而门禁要跑 DEBUG=OFF / ON 两遍——串跑 3 小时量级，分片把两遍 × n 片摊成矩阵作业，
# 总核时不变而墙钟近线性下降。⚠️ 分片不减覆盖面，也不改变判据：每片各自 red 即整门 red，
# 缺片等于缺覆盖面——故格式非法/索引越界在 configure 期 FATAL_ERROR，片内选中 0 个 TU 在
# runner 期 exit 2，两层都不允许「跑到了但没活儿」被读成「跑过且干净」。
# 各片 JSON 的 `shard` / `tu_total` 字段自述「本片是全量的哪一份」，聚合时据此核对片数齐没齐。
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

# 分片透传：值形如 "0/4"，本目录只跑第 0 片（共 4 片）。CI 矩阵靠它把一遍拆成多个作业；
# 日常本机留空即「一遍跑全量」，与拆分前的行为完全一致。
# ⚠️ 校验放在 configure 期而非脚本里：把 "0/04"、"4/4"、"off" 这类写错的值一路带到 runner，
#    症状是某个作业秒退或整片缺席，而门禁日志看着像绿的——覆盖面塌了没人报。
set(AURORA_LINT_SHARD "" CACHE STRING
        "Shard the 'lint' / 'lint-fix' runs as '<index>/<total>' (empty = lint every TU in one run)")
set(_lint_shard_args "")
if (AURORA_LINT_SHARD)
    # 正则里就禁掉前导零：CMake 的 `if(LESS)` 按十进制比，但 `math()` 会把 "08" 当八进制读而出错，
    # 而 `10#` 这种强制进位前缀 math() 根本不认（实测报 "cannot parse the expression"）。
    # 与其到处设数字陷阱，不如只收规范写法。
    if (NOT AURORA_LINT_SHARD MATCHES "^(0|[1-9][0-9]*)/(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "AURORA_LINT_SHARD must be '<index>/<total>' with plain decimal numbers "
                            "(e.g. 0/4), got '${AURORA_LINT_SHARD}'")
    endif ()
    set(_shard_i "${CMAKE_MATCH_1}")
    set(_shard_n "${CMAKE_MATCH_2}")
    if (_shard_n LESS 1 OR _shard_i GREATER_EQUAL _shard_n)
        message(FATAL_ERROR "AURORA_LINT_SHARD='${AURORA_LINT_SHARD}' is out of range: "
                            "total must be >= 1 and index in [0, total)")
    endif ()
    set(_lint_shard_args --shard "${_shard_i}/${_shard_n}")
    aurora_log("Clang-Tidy: sharded run ${_shard_i}/${_shard_n} (this build dir covers only that slice)")
endif ()

# `lint` 始终把结构化清单写到本目录的 lint-findings.json（tu_count / unique_findings /
# broken_tus / by_check / by_file / findings）。门禁 stdout 只有 top-N 文件表，CI 上据此
# 判因必然漏掉尾数；有了这份 JSON，聚合产物才能作为「按文件 + check 逐条列名」的依据。
# 分片跑法下这份清单只是全量的一片，故另带 shard / tu_total 两个自述字段（见文件头）。
add_custom_target(lint
        COMMAND ${PYTHON3_EXE} "${_lint_script}" --build-dir "${CMAKE_BINARY_DIR}" ${_lint_args} ${_lint_shard_args}
                --json-out "${CMAKE_BINARY_DIR}/lint-findings.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Clang-Tidy: linting non-third_party TUs (deduplicated; fails on any finding)")

add_custom_target(lint-fix
        COMMAND ${PYTHON3_EXE} "${_lint_script}" --build-dir "${CMAKE_BINARY_DIR}" ${_lint_args} ${_lint_shard_args} --fix
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Clang-Tidy: applying fix-its in place (review the diff before committing)")

aurora_log("Clang-Tidy: 'lint' / 'lint-fix' targets available (${AURORA_CLANG_TIDY_EXE})")
