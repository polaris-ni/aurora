# ============================================================
# AuroraTests.cmake — 注册式测试 runner（CTest）
# ------------------------------------------------------------
# tests/unit 与 tests/integration 下全部用例 TU 链入单一可执行 aurora_test_runner：
#   - 全量构建从「每文件一个 exe、各自链接 libaurora」降为一次链接（极速构建的核心）；
#   - 用例由框架静态注册，main 由框架唯一提供（tests/framework/main.cpp）；
#     测试文件禁止自定义 main()。
#   - CTest 粒度：每条 add_test = runner --run=<stem>（文件级，进程隔离）。
#
# ⚠️ 空源集 guard：测试体系重写期间（「破旧」与「立骨」之间）tests/unit 与
#    tests/integration 可能为空。此时 add_executable 收到空源集会直接报 CMake 错误，
#    registry_integrity 依赖的 $<TARGET_FILE:aurora_test_runner> 亦无法解析。
#    故静态校验类用例先行注册，随后对空源集提前 return —— 只保留静态校验，
#    跳过 runner 相关的一切。
#
# 已移除的旧机制（重写清理，勿恢复）：
#   - 全局属性 _aurora_test_targets：全仓零消费者；AURORA_ENABLE_DEBUG 现由 aurora 的
#     PUBLIC 编译定义自动传播给所有消费者，测试 target 无需额外处理。
#   - RUN_SERIAL 硬编码注入：并行模型改为「进程隔离 + 资源虚拟化」，由框架在用例边界
#     隔离共享资源（tmpdir / cwd / 单例 / 剪贴板），不再依赖 CTest 串行。
#   - WORKING_DIRECTORY 白名单（原 14 条）：框架启动时统一把 cwd 切到仓库根，
#     相对路径经 tests/support/paths.h 解析。
# ============================================================

option(AURORA_BUILD_TESTS "Build Aurora tests" ON)
if (AURORA_BUILD_TESTS)
    enable_testing()
    # 框架源（registry / runner / main）恒参与构建；用例源可为空（重写中间态）。
    file(GLOB AURORA_TEST_FRAMEWORK_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/framework/*.cpp")
    file(GLOB AURORA_TEST_CASE_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/*.cpp")
    set(AURORA_TEST_SOURCES ${AURORA_TEST_FRAMEWORK_SOURCES} ${AURORA_TEST_CASE_SOURCES})

    # ---- 静态校验 / 门禁脚本（tools/check/*.py）注册为 CTest 用例 ----
    # 先于空源集 guard 注册：仓库结构类守护在重写全程持续生效，与测试源是否为空无关。
    # 跨平台 Python 解释器探测；找不到则不注册（不阻断 C++ 测试）。
    find_program(PYTHON3_EXE NAMES python3 python)
    if (PYTHON3_EXE)
        set(_check_dir "${CMAKE_SOURCE_DIR}/tools/check")
        # 校验 codespec 模块映射文档中的文件引用是否仍存在于仓库。
        add_test(NAME check_arch_module_map
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_arch_module_map.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 生成物依赖型门禁：须先构建生成器才能跑（CI 必须排在 build 之后）。
        # ⚠️ Emscripten 交叉构建下不注册：二者要调用**宿主可执行**的生成器，而 wasm 产物的
        #    gen_api_tools 是 .js（须 node 解释），gen_debug_api 同样不是原生 exe。让它们原生
        #    化意味着在 wasm job 里再整编一遍 aurora 库（gen_api_tools 链接 aurora），成本与
        #    linux/msvc 等原生 job 完全重复。故交叉构建下交由原生 job 守护，此处不注册。
        if (NOT EMSCRIPTEN)
            # 生成器 aurora_api.json 合并不截断回归（直接调用真实构建产物）。
            add_test(NAME check_gen_api_merge
                    COMMAND ${PYTHON3_EXE} "${_check_dir}/check_gen_api_merge.py" "${CMAKE_BINARY_DIR}"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            # aurora_api.json 与代码真实 API 的漂移守护（复用 gen_api_tools 提取，零三方漂移）。
            # 同 check_gen_api_merge 前提：须先构建 gen_api_tools，故 CI 必须排在 build 之后。
            add_test(NAME check_api_schema_sync
                    COMMAND ${PYTHON3_EXE} "${_check_dir}/check_api_schema_sync.py" "${CMAKE_BINARY_DIR}"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        endif ()
        # codespec 文档内部一致性守护（断链 / 失效锚点 / 章节号 / 反引号路径 / 特性表落点）。
        add_test(NAME check_codespec_xref
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_codespec_xref.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 代码注释 ↔ 文档一致性守护（架构/规格 § 引用 + 测试头部目标单元路径）。
        add_test(NAME check_code_doc_sync
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_code_doc_sync.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 版本一致性门禁（CHANGELOG.currentVersion 必须等于库版本；描述性口径不符仅告警）。
        add_test(NAME check_version_consistency
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_version_consistency.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 公共 API 命名一致性门禁（#2）：类型 PascalCase、属性/事件/函数 snake_case、事件 on_ 前缀。
        add_test(NAME check_naming_conventions
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_naming_conventions.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 零原生平台宏门禁（#14）：include/ src/ 预处理分支禁止 _WIN32/__linux__/__x86_64__ 等
        # 原生宏（platform.h 自身与 _WIN32_WINNT 等 SDK 旋钮豁免）；规范化宏密度仅报告。
        add_test(NAME check_platform_macros
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_platform_macros.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # 公共 API 体量/token 预算门禁（#24）：aurora_api.json 估算 token 数不得超预算上限。
        add_test(NAME check_api_budget
                COMMAND ${PYTHON3_EXE} "${_check_dir}/check_api_budget.py"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif ()

    # ---- 空源集 guard ----
    if (NOT AURORA_TEST_SOURCES)
        aurora_log("Tests: no test sources found (tests/unit|integration empty) - runner target skipped")
        return ()
    endif ()

    # ---- 分片（AURORA_TEST_SHARDS）：N=1 单 runner（默认，单次链接）；N>1 按 Suite 稳定散列拆 N 个 runner ----
    # 散列取自文件 stem（== Suite 名），同一 Suite 恒落同一分片；MD5 前 8 个十六进制位做桶号，
    # 稳定且与文件顺序无关。各分片 runner 均含完整框架源（各含唯一 main）。
    set(AURORA_TEST_SHARDS "1" CACHE STRING
            "Number of test runner shards; 1 = single runner, N>1 splits test sources across N runners by stable suite hash")
    if (NOT AURORA_TEST_SHARDS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "AURORA_TEST_SHARDS must be a positive integer (got '${AURORA_TEST_SHARDS}')")
    endif ()

    # runner 目标统一配置（分片共享）：链接 aurora + C++20 + 消费者 PCH + 告警；
    # tests/ 供框架头解析，examples/app/google_play 供 google_play_data/ui 数据层测试；
    # tools/include 复用 known_enums.h 等 SSOT，tests/support 为测试公共设施。
    function(_aurora_configure_runner tgt)
        aurora_setup_consumer_target(${tgt}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/app/google_play")
        target_include_directories(${tgt} PRIVATE
                "${CMAKE_SOURCE_DIR}/tools/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
        # 库侧差异，聚合后收敛到 runner 一处：
        #   shcore — dpi_awareness 用例（GetDpiForWindow 等）高 DPI 回归；inspector — 检视 server pimpl 实现；
        #   -ffp-contract=off — 标量黄金参考禁 FMA 收缩（目标级应用：仅禁收缩，语义安全）。
        if (WIN32)
            target_link_libraries(${tgt} PRIVATE shcore)
        endif ()
        if (TARGET aurora_inspector_server)
            target_link_libraries(${tgt} PRIVATE aurora_inspector_server)
        endif ()
        if (AURORA_ENABLE_SIMD)
            if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
                target_compile_options(${tgt} PRIVATE -ffp-contract=off)
            elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
                target_compile_options(${tgt} PRIVATE /fp:precise)
            endif ()
        endif ()
        if (EMSCRIPTEN)
            # NODERAWFS：Emscripten 默认 MEMFS，golden 基准 / 存储 / 临时文件等用例的
            # ifstream/ofstream 全落在内存盘，读不到仓库里的真实文件（表现为「文件不存在」
            # 而非断言失败）。NODERAWFS 让 node 直接透传宿主文件系统，与原生跑法语义一致。
            # 仅测试 runner 需要：库与其它 wasm 产物保持默认（浏览器中无宿主 FS 可透传）。
            #
            # ALLOW_MEMORY_GROWTH：NODERAWFS 产物的堆**默认不可增长**（实测：带
            #   -sNODERAWFS=1 -O3 -fexceptions 时分配 200MB 即 Aborted(OOM)，而不带
            #   NODERAWFS 的同源程序可增长）。测试 runner 需要它——itest_play_repository
            #   的程序化合成 catalog（192 项 × 图标/截图/横幅位图）远超初始堆。
            #
            # --post-js wasm_noderawfs_cwd.js：NODERAWFS 在 Windows 宿主下 FS.cwd() 返回
            #   `D:\...`（盘符 + 反斜杠），musl getcwd 判 ENOENT → fs::current_path() /
            #   fs::absolute(相对路径) 全线失效（详见该文件头部注释）。Linux/macOS 宿主本
            #   就是 POSIX 形态，shim 为恒等变换。
            target_link_options(${tgt} PRIVATE
                    -sNODERAWFS=1
                    -sALLOW_MEMORY_GROWTH=1
                    "--post-js=${CMAKE_SOURCE_DIR}/tests/support/wasm_noderawfs_cwd.js")
        endif ()
    endfunction()

    set(_runner_targets "")
    if (AURORA_TEST_SHARDS EQUAL 1)
        add_executable(aurora_test_runner ${AURORA_TEST_SOURCES})
        _aurora_configure_runner(aurora_test_runner)
        list(APPEND _runner_targets aurora_test_runner)

        # CTest 注册：每条用例 = runner --run=<stem>（进程隔离，文件级粒度）。
        foreach (tst ${AURORA_TEST_CASE_SOURCES})
            get_filename_component(tname ${tst} NAME_WE)
            add_test(NAME ${tname} COMMAND aurora_test_runner --run=${tname})
        endforeach ()
    else ()
        # 桶分配：stem → MD5 前 8 位 % N（稳定散列，与 GLOB 顺序无关）。
        math(EXPR _last_shard "${AURORA_TEST_SHARDS} - 1")
        foreach (k RANGE ${_last_shard})
            set(_shard_sources_${k} "")
            set(_shard_stems_${k} "")
        endforeach ()
        foreach (tst ${AURORA_TEST_CASE_SOURCES})
            get_filename_component(tname ${tst} NAME_WE)
            string(MD5 _md5 "${tname}")
            string(SUBSTRING "${_md5}" 0 8 _hex)
            math(EXPR bucket "0x${_hex} % ${AURORA_TEST_SHARDS}")
            list(APPEND _shard_sources_${bucket} ${tst})
            list(APPEND _shard_stems_${bucket} ${tname})
        endforeach ()
        foreach (k RANGE ${_last_shard})
            set(_tgt aurora_test_runner_s${k})
            add_executable(${_tgt} ${AURORA_TEST_FRAMEWORK_SOURCES} ${_shard_sources_${k}})
            _aurora_configure_runner(${_tgt})
            list(APPEND _runner_targets ${_tgt})
            # CTest 用例名带分片号（<stem>_s<k>）：文件级粒度下 stem 全局唯一，编号仅为可读性。
            foreach (tname ${_shard_stems_${k}})
                add_test(NAME ${tname}_s${k} COMMAND ${_tgt} --run=${tname})
            endforeach ()
        endforeach ()
        aurora_log("Tests: sharded runners = ${AURORA_TEST_SHARDS}")
    endif ()

    # ---- 框架自检：验证注册 / 断言 / 异常隔离 / 退出码协议 ----
    # 使用内建 synthetic 用例，不消费注册表，故不干扰 registry_integrity；分片时跑 0 号分片。
    list(GET _runner_targets 0 _first_runner)
    add_test(NAME framework_selftest COMMAND ${_first_runner} --selftest)

    # 完整性守护（TEST-R6，用例级）：runner --list 与测试源中的用例宏逐用例比对，
    # 含「TEST_P 漏 INSTANTIATE → 静默不运行」检测；比对逻辑见
    # tools/check/check_test_registry.py（无 python 时不注册，与其他 check_* 门禁一致）。
    # 分片时脚本对各 runner --list 取并集后比对（脚本 --runner 可重复传入）。
    if (PYTHON3_EXE)
        set(_registry_cmd COMMAND ${PYTHON3_EXE} "${CMAKE_SOURCE_DIR}/tools/check/check_test_registry.py")
        # 交叉构建（Emscripten）下 runner 是 .js，不能直接 exec：把 CTest 同款模拟器
        # （Emscripten.cmake 设为 node）显式交给脚本前置到命令行。
        if (EMSCRIPTEN AND CMAKE_CROSSCOMPILING_EMULATOR)
            list(APPEND _registry_cmd --launcher "${CMAKE_CROSSCOMPILING_EMULATOR}")
        endif ()
        foreach (_tgt ${_runner_targets})
            list(APPEND _registry_cmd --runner "$<TARGET_FILE:${_tgt}>")
        endforeach ()
        list(APPEND _registry_cmd
                --tests-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
                --tests-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
        add_test(NAME registry_integrity ${_registry_cmd}
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif ()
endif ()
