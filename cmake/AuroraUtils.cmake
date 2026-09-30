# ============================================================
# AuroraUtils.cmake — 公共辅助函数（消费者目标统一配置 + 工具链定位）
# ------------------------------------------------------------
# aurora_setup_consumer_target(<tgt> [extra private include dirs...])
# 统一处理所有「消费 aurora 库」的可执行目标（demo/测试/工具）的样板：
#   - PRIVATE 链接 aurora（自动获得其 PUBLIC 头目录 / feature 宏 / PCH 锚点）
#   - CXX_STANDARD 20
#   - 复用消费者共享 PCH（REUSE_FROM aurora_consumer_pch，受 AURORA_PCH_ENABLED 门控）
#   - 注入项目统一告警标志（与 aurora 自身一致）
#   - MinGW 下追加 -Wa,-mbig-obj（放宽 COFF 段数上限，见函数内注释）
#   - 可选额外 PRIVATE include 目录（ARGN）
# NOMINMAX 由顶层全局 add_compile_definitions 提供（第三方库同样需要），此处不再重复。
# 调用方须在本文件 include 之后、且 aurora_consumer_pch 锚点目标已定义之后调用。
#
# aurora_find_clang_format(<out_var>)
# 定位「真能读懂本仓 .clang-format」的 clang-format（生成链与排版门禁共用同一判据，见函数上方注释）。
# ============================================================

function(aurora_setup_consumer_target _tgt)
    # 链接 aurora 静态库（PUBLIC 传递头目录 / feature 宏 / 版本定义）。
    target_link_libraries(${_tgt} PRIVATE aurora)
    set_target_properties(${_tgt} PROPERTIES CXX_STANDARD 20)

    # 复用消费者共享 PCH（含 aurora.h，.gch 只编一份）；flags 不匹配时 GCC 安全回退为文本包含。
    # 插桩构建下 PCH 关闭，跳过 REUSE_FROM（见 AURORA_PCH_ENABLED）。
    if (AURORA_PCH_ENABLED)
        target_precompile_headers(${_tgt} REUSE_FROM aurora_consumer_pch)
    endif ()

    # 项目统一告警（mirrors CODING_STANDARDS.md §10）。
    # -Wno-missing-field-initializers：本库大量采用聚合 Props 的部分初始化，其余字段值初始化为零，
    # 该告警纯属噪音，故关闭。
    # MSVC 不识别 GCC 风格 -W*（D8021 硬错误），保持其默认 /W3 即可。
    if (NOT MSVC)
        target_compile_options(${_tgt} PRIVATE
                -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif ()

    # MinGW 的 COFF 目标文件默认段数上限（65535）会被超大消费者 TU 在 Debug（-g + 大量
    # 模板/内联实体的 header-only 控件）下击穿，汇编器报 "too many sections" / "file too big"
    #（实测 examples/app/google_play/demo_google_play.cpp 达 33614 段）。
    # -Wa,-mbig-obj 把上限放宽到 2^32 段，对象仍是标准 COFF，对链接器与其他平台透明。
    # 与 AURORA_ENABLE_COVERAGE（AuroraInstrumentation.cmake）同口径，此处覆盖 demo / 测试 / 工具
    # 全部消费者目标；MSVC / clang-cl 的汇编器无此限制，不加。
    if (MINGW)
        target_compile_options(${_tgt} PRIVATE -Wa,-mbig-obj)
    endif ()

    # 可选额外 PRIVATE include 目录（如 examples/demos、tests/）。
    if (ARGN)
        target_include_directories(${_tgt} PRIVATE ${ARGN})
    endif ()

    # 静态链接 GCC runtime（libgcc / libstdc++）与 winpthread，使开发工具自包含、双击即跑。
    # MinGW 默认动态链接 libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll，这些 DLL
    # 不在 exe 同目录、也不在普通终端（双击 / 裸 cmd / PowerShell）的 PATH 上，导致运行时报
    # 「无法定位程序输入点 _gthr_win32_self」。
    # 仅 MinGW 生效（GCC 或 clang 的 MinGW 目标，链接的是 GCC runtime 与 winpthread）。
    # clang-cl / MSVC 模式链接 MSVCRT 且 lld-link 无 winpthread.lib，命中会「could not open
    # 'winpthread.lib'」；MSVC 与其他平台亦忽略。故以 MINGW 为门禁而非「Clang + WIN32」。
    if (MINGW)
        target_link_options(${_tgt} PRIVATE
                -static-libgcc -static-libstdc++
                -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif ()
endfunction()

# ---------------------------------------------------------------------------
# AURORA_CLANG_FORMAT_STYLE — 本仓排版配置的**钉死引用形态**（`file:<绝对路径>`）。
#
# ⚠️ 任何调用 clang-format 的地方都必须用它，不得写裸 `--style=file`。裸形态的配置解析口径
#   取决于入参类型：给**具名文件**时从「该文件所在目录」逐级上溯，只有走标准输入才看工作目录。
#   生成链（AuroraTools 的 generate_error_codes）把产物先落在 `<build>/_gen_error_codes_stage/`
#   再拷回源码树，构建目录一旦位于仓库外（`cmake -B <仓外目录>`、WSL 侧的仓外构建树），裸形态就
#   寻不到配置、**静默退化成内置 LLVM 风格（2 空格 / 80 列）**，再把这份退化产物拷进版控文件。
#   实测代价：内容与 HEAD 一字未改，仅排版退化就让 check_doc_comments 报 26 条、format-check
#   报整文件 diff。钉死路径后，配置来源与构建目录位置解耦；探测与实调用同用这一形态，退化才不可能
#   「探测通过、产物却红」。Windows 与 WSL 两侧实测（clang-format 22.1.2 / Ubuntu 21.1.8）：
#   该形态下重新生成的 error_codes.gen.h 与 HEAD 逐字节相同。
set(AURORA_CLANG_FORMAT_STYLE "file:${AURORA_SOURCE_DIR}/.clang-format")

# ---------------------------------------------------------------------------
# aurora_find_clang_format(<out_var>)
#   按 AURORA_CLANG_FORMAT_CANDIDATES 的顺序挑出**第一个能读懂本仓 `.clang-format`** 的
#   clang-format，绝对路径写入 <out_var>；一个都不行则写空串（不 FATAL_ERROR）。
#
# 生成链（AuroraTools 的 generate_error_codes）与排版门禁（AuroraFormat 的 format/format-check）
# 共用本函数，两处因此必然落在同一个可执行文件上——避免「生成时按 A 版本折行、门禁按 B 版本
# 判红」这类口径漂移。
# ---------------------------------------------------------------------------
set(AURORA_CLANG_FORMAT_CANDIDATES "clang-format-22;clang-format-21;clang-format-20;clang-format"
        CACHE STRING "clang-format executables to probe, in order (first one that parses .clang-format wins)")

function(aurora_find_clang_format _out)
    set(_picked "")
    foreach(_cand IN LISTS AURORA_CLANG_FORMAT_CANDIDATES)
        unset(_exe)
        find_program(_exe NAMES "${_cand}")
        if (NOT _exe)
            continue()
        endif ()
        # 探测用钉死形态（见 AURORA_CLANG_FORMAT_STYLE）：既不依赖 cwd，也不依赖被检查文件的位置，
        # 与生成链 / 排版门禁的实调用完全同口径——否则会出现「探测恒通过、实调用却因寻不到配置而
        # 静默退化成内置 LLVM 风格」。
        execute_process(COMMAND "${_exe}" --dump-config "--style=${AURORA_CLANG_FORMAT_STYLE}"
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _probe_out
                ERROR_VARIABLE _probe_err)
        if (_rc EQUAL 0)
            set(_picked "${_exe}")
            break()
        endif ()
        # 把工具自己报的第一行带出来：只给退出码时，「版本读不懂配置」与「二进制压根跑不起来
        # （缺共享库 / 不是 clang-format）」在日志里长得一模一样，无法判因。
        string(STRIP "${_probe_err}${_probe_out}" _probe_msg)
        string(REGEX MATCH "[^\r\n]+" _probe_msg "${_probe_msg}")
        message(STATUS "clang-format: '${_cand}' rejected (exit ${_rc}) ${_probe_msg} -- try the next candidate")
    endforeach()
    set(${_out} "${_picked}" PARENT_SCOPE)
endfunction()
