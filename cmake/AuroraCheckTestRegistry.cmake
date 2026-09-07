# ============================================================
# AuroraCheckTestRegistry.cmake — 完整性守护脚本（registry_integrity 用）
# ------------------------------------------------------------
# 比对 runner --list 输出与配置期生成的预期清单。
# 新世界风险：新增 tests/unit/*.cpp 或 tests/integration/*.cpp 忘写 AURORA_TEST() 时不会有任何链接错误，
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

# 归一化顺序：runner --list 按用例名排序输出，而配置期清单按 file(GLOB) 的目录顺序生成
# （tests/integration 排在 tests/unit 之前）。用例在两目录间迁移时，两侧顺序必然不同，
# 但注册集合本身并未漂移。本守护只关心「集合相等」——是否有源文件漏写 AURORA_TEST()、
# 或注册了不存在的用例 —— 故两侧各自排序后再比较，避免纯顺序差异造成误报。
string(REPLACE "\n" ";" _expect_list "${_expect}")
string(REPLACE "\n" ";" _actual_list "${_actual}")
list(SORT _expect_list)
list(SORT _actual_list)

if (NOT _actual_list STREQUAL _expect_list)
    message(FATAL_ERROR
            "test registry mismatch: runner --list vs tests/unit|integration/*.cpp\n--- expected ---\n${_expect}\n--- actual ---\n${_actual}")
endif ()
