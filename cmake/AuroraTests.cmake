# ============================================================
# AuroraTests.cmake — 注册式测试 runner（CTest）
# ------------------------------------------------------------
# tests/ 下全部用例 TU 链入单一可执行 aurora_test_runner：
#   - 全量构建从「每文件一个 exe、各自链接 libaurora」降为一次链接（极速构建的核心）；
#   - 用例经 AURORA_TEST() 静态注册（用例名 = 文件名 stem，按源文件注入 AURORA_TEST_NAME），
#     main 由 tests/au_test_main.cpp 唯一提供；测试文件禁止再自定义 main()。
#   - CTest 粒度不变：每条 add_test = runner --run=<stem>（进程隔离与旧逐 exe 等价）。
# 新增 tests/*.cpp 由 GLOB CONFIGURE_DEPENDS 自动收集（必要时碰一下 CMakeLists.txt）。
# 完整性守护 registry_integrity：新文件漏写 AURORA_TEST() 时旧世界是链接错误、
# 新世界会静默不运行，故以 runner --list 与配置期 GLOB 清单比对兜底。
# ============================================================

option(AURORA_BUILD_TESTS "Build Aurora tests" ON)
if (AURORA_BUILD_TESTS)
    enable_testing()
    file(GLOB AURORA_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    list(REMOVE_ITEM AURORA_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_test_main.cpp")

    # 用例名 = 文件名 stem：按源文件注入 AURORA_TEST_NAME（AURORA_TEST() 注册与 --run 匹配均依赖此名）。
    set(_expect_list "")
    foreach (tst ${AURORA_TEST_SOURCES})
        get_filename_component(tname ${tst} NAME_WE)
        set_source_files_properties(${tst} PROPERTIES
                COMPILE_DEFINITIONS "AURORA_TEST_NAME=\"${tname}\"")
        string(APPEND _expect_list "${tname}\n")
    endforeach ()

    add_executable(aurora_test_runner
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_test_main.cpp"
            ${AURORA_TEST_SOURCES})
    # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志；
    # tests/ 供 test_harness.h 解析，examples/app/google_play 供 google_play_data/ui 数据层测试。
    aurora_setup_consumer_target(aurora_test_runner
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/app/google_play")
    # 暴露工具链共享头（tools/include）：部分测试需复用 known_enums.h / au_lint_core.h 等
    # 单一来源，避免与工具实现漂移（known_enums 取值守护、aurora_lint 核心逻辑单测）。
    target_include_directories(aurora_test_runner PRIVATE "${CMAKE_SOURCE_DIR}/tools/include")
    # 标记测试目标，供 AuroraInstrumentation 注入 AURORA_ENABLE_DEBUG（与库体编译分支对齐），
    # 使 DEBUG 成功路径可在测试中编译/调用；宏保持 PRIVATE，不导出给外部消费者 ABI。
    set_property(GLOBAL APPEND PROPERTY _aurora_test_targets aurora_test_runner)

    # 旧模块逐 exe 的链接差异，聚合后收敛到 runner 一处：
    #   shcore — dpi_awareness 用例（GetDpiForWindow 等）高 DPI 回归；inspector — 检视 server pimpl 实现；
    #   -ffp-contract=off — test_simd_parity 的标量黄金参考禁 FMA 收缩（目标级应用：仅禁收缩，语义安全）。
    if (WIN32)
        target_link_libraries(aurora_test_runner PRIVATE shcore)
    endif ()
    if (TARGET aurora_inspector_server)
        target_link_libraries(aurora_test_runner PRIVATE aurora_inspector_server)
    endif ()
    if (AURORA_ENABLE_SIMD)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(aurora_test_runner PRIVATE -ffp-contract=off)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_compile_options(aurora_test_runner PRIVATE /fp:precise)
        endif ()
    endif ()

    # CTest 注册：每条用例 = runner --run=<stem>（进程隔离，与旧逐 exe 等价）。
    foreach (tst ${AURORA_TEST_SOURCES})
        get_filename_component(tname ${tst} NAME_WE)
        add_test(NAME ${tname} COMMAND aurora_test_runner --run=${tname})
    endforeach ()

    # 需以仓库根为工作目录的用例（读 tests/golden、tests/fixtures、仓库根 aurora_api.json，
    # 或检视用 PNG 写到 build/ 下）。
    foreach (_t test_offscreen test_ai_compat test_nav_win test_custom_surface test_window_options
            test_bottom_nav_bar test_grid_view test_lazy_row test_api_json_integrity
            test_image_view)
        set_tests_properties(${_t} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endforeach ()

    # 需独占运行的用例：
    #   - test_perf_display_list：相对性能计时断言，并行满载调度抖动会偶发误报；
    #   - test_text / test_clipboard：二者读写同一份系统剪贴板，-j 并发时互相覆盖导致
    #     Clipboard::get_text() 偶发读到对端数据（旧逐 exe 套件亦有的共享资源竞态）。
    # 串行独占可消除该竞态，代价仅数条用例错峰。
    foreach (_serial_t test_perf_display_list test_text test_clipboard)
        set_tests_properties(${_serial_t} PROPERTIES RUN_SERIAL ON)
    endforeach ()

    # 完整性守护：runner --list 输出必须与配置期 GLOB 清单逐行一致。
    set(_expect_file "${CMAKE_CURRENT_BINARY_DIR}/tests_expected.txt")
    file(WRITE "${_expect_file}" "${_expect_list}")
    add_test(NAME registry_integrity
            COMMAND ${CMAKE_COMMAND}
                    -DRUNNER=$<TARGET_FILE:aurora_test_runner>
                    -DEXPECT=${_expect_file}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuroraCheckTestRegistry.cmake")

    # ---- 静态校验 / 门禁脚本（tools/check/*.py）注册为 CTest 用例 ----
    # 跨平台 Python 解释器探测；找不到则不注册（不阻断 C++ 测试）。
    find_program(PYTHON3_EXE NAMES python3 python)
    if (PYTHON3_EXE)
        set(_check_dir "${CMAKE_SOURCE_DIR}/tools/check")
        # 校验 codespec 模块映射文档中的文件引用是否仍存在于仓库。
        add_test(NAME check_arch_module_map
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_arch_module_map.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 生成器 aurora_api.json 合并不截断回归（直接调用真实构建产物）。
        add_test(NAME check_gen_api_merge
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_gen_api_merge.py" "${CMAKE_BINARY_DIR}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 版本一致性门禁（CHANGELOG.currentVersion 必须等于库版本；描述性口径不符仅告警）。
        add_test(NAME check_version_consistency
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_version_consistency.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif ()
endif ()
