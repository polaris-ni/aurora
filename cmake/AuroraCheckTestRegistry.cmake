# ============================================================
# AuroraCheckTestRegistry.cmake — 完整性守护脚本（registry_integrity 用）
# ------------------------------------------------------------
# 比对逻辑已升级为**用例级**（TEST-R6），实际比对由 Python 脚本承担：
#   tools/check/check_test_registry.py
# 它运行 runner --list --format=cases，与从测试源静态提取的用例宏
# （AURORA_TEST_CASE / _F / _P / _TYPED_TEST + INSTANTIATE / TYPED_TEST_SUITE）
# 逐用例比对，能抓住「写了 AURORA_TEST_P 但漏 INSTANTIATE → 用例静默不运行」
# 这类套件级比对抓不到的漂移。
#
# 依赖：python（与其他 check_* 门禁一致；无 python 时本条 ctest 用例不注册，
# 与静态校验类门禁同一取舍）。
# ============================================================

execute_process(
        COMMAND "${PYTHON3_EXE}" "${CMAKE_SOURCE_DIR}/tools/check/check_test_registry.py"
        --runner "${RUNNER}"
        --tests-dir "${CMAKE_SOURCE_DIR}/tests/unit"
        --tests-dir "${CMAKE_SOURCE_DIR}/tests/integration"
        RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "test registry case-level mismatch: see [FAIL] lines above")
endif ()
