# ============================================================
# AuroraCcache.cmake — ccache 编译缓存集成
# ------------------------------------------------------------
# 提供 ccache 编译缓存支持，加速重复编译。
# 可通过 -DAURORA_ENABLE_CCACHE=OFF 禁用。
# ============================================================

option(AURORA_ENABLE_CCACHE "Use ccache for compilation caching" ON)

if (AURORA_ENABLE_CCACHE)
    # 首先尝试在PATH中查找ccache
    find_program(CCACHE_PROGRAM ccache)

    # 如果未找到，尝试常见的winget安装路径
    if (NOT CCACHE_PROGRAM)
        set(_ccache_winget_dir "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages")
        if (EXISTS "${_ccache_winget_dir}")
            file(GLOB _ccache_candidates "${_ccache_winget_dir}/Ccache.Ccache_*/ccache-*/ccache.exe")
            if (_ccache_candidates)
                list(GET _ccache_candidates 0 CCACHE_PROGRAM)
                aurora_log("ccache: found via winget (${CCACHE_PROGRAM})")
            endif ()
        endif ()
    endif ()

    if (CCACHE_PROGRAM)
        # 校验 ccache 可执行：--version 失败（损坏/占位）则视为不可用，安全回退。
        execute_process(COMMAND "${CCACHE_PROGRAM}" --version
                RESULT_VARIABLE _aurora_ccache_ver_result
                OUTPUT_QUIET ERROR_QUIET)
        if (NOT _aurora_ccache_ver_result EQUAL 0)
            aurora_log("ccache: found but --version failed, caching disabled")
            unset(CCACHE_PROGRAM)
        endif ()
    endif ()

    if (CCACHE_PROGRAM)
        # 设置 ccache 作为编译器启动器
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")

        # 配置 ccache 环境变量
        set(ENV{CCACHE_COMPRESS} "1")
        set(ENV{CCACHE_COMPRESSLEVEL} "6")
        set(ENV{CCACHE_HARDLINK} "1")

        # 设置缓存目录（可自定义）
        set(AURORA_CCACHE_DIR "" CACHE PATH "ccache 缓存目录（默认使用系统默认）")
        if (AURORA_CCACHE_DIR)
            set(ENV{CCACHE_DIR} "${AURORA_CCACHE_DIR}")
        endif ()

        # 设置缓存大小限制（默认 2GB）
        set(AURORA_CCACHE_MAXSIZE "2G" CACHE STRING "ccache 最大缓存大小")
        if (AURORA_CCACHE_MAXSIZE)
            set(ENV{CCACHE_MAXSIZE} "${AURORA_CCACHE_MAXSIZE}")
        endif ()

        aurora_log("ccache: enabled (${CCACHE_PROGRAM})")
        aurora_log("ccache: compression enabled (level 6)")
        aurora_log("ccache: hardlink enabled")
        if (AURORA_CCACHE_DIR)
            aurora_log("ccache: cache directory = ${AURORA_CCACHE_DIR}")
        endif ()
        aurora_log("ccache: max size = ${AURORA_CCACHE_MAXSIZE}")
    else ()
        aurora_log("ccache: not found, compilation caching disabled")
        aurora_log("ccache: install ccache for faster rebuilds")
    endif ()
else ()
    aurora_log("ccache: disabled by user")
endif ()
