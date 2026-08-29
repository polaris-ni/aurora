# ============================================================
# AuroraCheckTestRegistry.cmake — 完整性守护脚本（registry_integrity 用）
# ------------------------------------------------------------
# 比对 runner --list 输出与配置期生成的预期清单。
# 新世界风险：新增 tests/*.cpp 忘写 AURORA_TEST() 时不会有任何链接错误，
# 用例静默不运行 —— 本脚本让这种漂移在 ctest 阶段立刻失败。
# ============================================================

execute_process(COMMAND "${RUNNER}" --list OUTPUT_VARIABLE _actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "aurora_test_runner --list failed (rc=${_rc})")
endif ()

file(READ "${EXPECT}" _expect)
# 归一化换行：runner 在 Windows 文本模式下向 stdout 逐行输出 "名\r\n"（AURORA_LOG_RAW 走 fprintf），
# 而配置期清单以 "\n" 写入。去除 \r 后再比较，避免仅因换行风格差异误报注册表漂移。
string(REPLACE "\r" "" _expect "${_expect}")
string(REPLACE "\r" "" _actual "${_actual}")
string(STRIP "${_expect}" _expect)
string(STRIP "${_actual}" _actual)

if (NOT _actual STREQUAL _expect)
    message(FATAL_ERROR
            "test registry mismatch: runner --list vs tests/*.cpp\n--- expected ---\n${_expect}\n--- actual ---\n${_actual}")
endif ()
