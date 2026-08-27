# ============================================================
# AuroraTests.cmake — 测试目标（CTest）
# ------------------------------------------------------------
# 各测试源文件自带 main()，故每个文件独立成可执行；合并会因多定义 main 冲突。
# 新增 tests/*.cpp 由 GLOB CONFIGURE_DEPENDS 自动收集（必要时碰一下 CMakeLists.txt）。
# ============================================================

option(AURORA_BUILD_TESTS "Build Aurora tests" ON)
if (AURORA_BUILD_TESTS)
    enable_testing()
    file(GLOB AURORA_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

    # 全部 tests/*.cpp 均参与构建；后端/平台专属用例（test_*_win32 / test_x11_surface /
    # test_wayland_surface / test_d3d11_present 等）在各自 main() 内按 feature 宏
    # （AURORA_BACKEND_* 等）self-skip：宏未开启时直接 TEST_END() 空通过，此处无需门控。
    foreach (tst ${AURORA_TEST_SOURCES})
        get_filename_component(tname ${tst} NAME_WE)
        add_executable(${tname} ${tst})
        # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志；
        # tests/ 加入包含路径（aurora/test_helpers.h 自动补全 test_harness.h 解析需要）。
        aurora_setup_consumer_target(${tname}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        # 标记测试目标，供 AuroraInstrumentation 在同配置注入 AURORA_ENABLE_DEBUG（与库体编译分支对齐），
        # 使 DEBUG 成功路径可在测试中编译/调用；宏保持 PRIVATE，不导出给外部消费者 ABI。
        set_property(GLOBAL APPEND PROPERTY _aurora_test_targets ${tname})
        add_test(NAME ${tname} COMMAND ${tname})
    endforeach ()

    # dpi_awareness_test 需要 shcore（GetDpiForWindow 等）做高 DPI 回归校验。
    if (TARGET dpi_awareness_test)
        target_link_libraries(dpi_awareness_test PRIVATE shcore)
    endif ()

    # 需以仓库根为工作目录的测试（ctest 默认 CWD=build，相对路径会解析失败）：
    #   test_offscreen       — tests/golden/ 像素级基准（原 test_golden 并入）
    #   test_ai_compat      — tests/fixtures/ai_compat/ fixture
    #   test_nav_win        — 检视用 PNG 写到 build/win_*.png（否则变成 build/build/...）
    #   test_custom_surface / test_window_options — 检视用 PNG 写到 build/*.png
    #   test_bottom_nav_bar / test_grid_view（原 test_grid_view_clip 并入）/ test_lazy_row
    #                       — 检视用 PNG 写到 build/*.png（ctest 默认 CWD=build 会变成 build/build/...）
    #   test_api_json_integrity — 读取仓库根 aurora_api.json（相对路径）
    #   test_image_view     — 读取 tests/golden/ 像素基准（相对路径）
    foreach (_t test_offscreen test_ai_compat test_nav_win test_custom_surface test_window_options
            test_bottom_nav_bar test_grid_view test_lazy_row test_api_json_integrity
            test_image_view)
        if (TARGET ${_t})
            set_tests_properties(${_t} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        endif ()
    endforeach ()

    # test_inspector_server / test_inspector_robustness 需链接 aurora_inspector_server（pimpl 实现）
    if (TARGET aurora_inspector_server)
        foreach (_insp_t test_inspector_server test_inspector_robustness)
            if (TARGET ${_insp_t})
                target_link_libraries(${_insp_t} PRIVATE aurora_inspector_server)
            endif ()
        endforeach ()
    endif ()

    # 计时断言测试（相对性能断言，如 replay 不慢于 re-record ×1.5）：并行满载时调度
    # 抖动会造成偶发误报，串行独占运行以保证 `ctest -j N` 稳定。
    if (TARGET test_perf_display_list)
        set_tests_properties(test_perf_display_list PROPERTIES RUN_SERIAL ON)
    endif ()

    # SIMD 逐位一致校验：本 TU 必须与 SIMD 路径同年编译为 -ffp-contract=off，
    # 否则其内联标量黄金参考会被 FMA 融合，与显式 mul+add 的 SIMD 路径差位，误报 parity 失败。
    # AURORA_ENABLE_SIMD 在 AuroraSimd.cmake（本文件之前）已定义；MSVC 用 /fp:precise 等价禁用收缩。
    if (AURORA_ENABLE_SIMD AND TARGET test_simd_parity)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(test_simd_parity PRIVATE -ffp-contract=off)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_compile_options(test_simd_parity PRIVATE /fp:precise)
        endif ()
    endif ()
endif ()
