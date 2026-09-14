# ============================================================
# AuroraCcache.cmake — ccache 编译缓存集成
# ------------------------------------------------------------
# 提供 ccache 编译缓存支持，加速重复编译。
# 可通过 -DAURORA_ENABLE_CCACHE=OFF 禁用。
#
# ⚠️ 配置注入机制：CMake 的 set(ENV{...}) 仅在 configure 期生效，不会传递给
# ninja/VS 构建期子进程——凡 configure 期写入的环境变量对实际编译全部无效。
# 因此所有 ccache 配置统一经编译器启动器注入：
#     CMAKE_{C,CXX}_COMPILER_LAUNCHER = cmake -E env CCACHE_*=... ccache
# 「cmake -E env」在每个编译边构建期展开，环境精确作用于本项目的 ccache 调用，
# 不污染用户全局环境，也不依赖构建 shell 是否导出过变量；对 Ninja / Make 各生成器
# 与 GCC/Clang 各编译器适用。
#
# ⚠️ MSVC（cl）不接入：ccache 对 MSVC 的 PCH 旗标组合（/Yu + /FI + /Fp）支持不完整，
# 会把强制包含改写后丢失 PCH 边界匹配 → C1010（查找预编译头时遇到意外的文件结尾）。
# 且 VS 多配置生成器本就不实现 <LANG>_COMPILER_LAUNCHER（Ninja + cl 同样命中此坑），
# 故编译器为 MSVC 时整体跳过注入，靠 PCH 提速（PCH 在 MSVC 为正收益，见 BUILD_OPTIONS）。
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

    # 校验 ccache 可执行：--version 失败（损坏/占位）则视为不可用，安全回退。
    if (CCACHE_PROGRAM)
        execute_process(COMMAND "${CCACHE_PROGRAM}" --version
                RESULT_VARIABLE _aurora_ccache_ver_result
                OUTPUT_QUIET ERROR_QUIET)
        if (NOT _aurora_ccache_ver_result EQUAL 0)
            aurora_log("ccache: found but --version failed, caching disabled")
            unset(CCACHE_PROGRAM CACHE)  # find_program 落在 cache；unset(普通变量) 后 if() 仍会回退读 cache
        endif ()
    endif ()

    if (CCACHE_PROGRAM AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        aurora_log("ccache: MSVC (cl) compiler detected, caching disabled"
                " (ccache does not support MSVC /Yu+/FI PCH flags; PCH is kept as the accelerator)")
        unset(CCACHE_PROGRAM CACHE)  # 同上：必须清 cache 条目才能真正禁用
    endif ()

    if (CCACHE_PROGRAM)
        # 构建期注入的 ccache 配置（经 cmake -E env，见文件头说明）：
        #   SLOPPINESS —— PCH 场景必需：未设 pch_defines/time_macros 时，命令行带
        #     -include .../cmake_pch.hxx 的调用被 ccache 直接判 Uncacheable（消费者
        #     TU 全量裸编、缓存形同虚设）；include_file_mtime/ctime 让头文件时间戳
        #     变化而内容不变时仍可命中（preprocessor 模式按内容摘要，安全）。
        #   BASEDIR + NOHASHDIR —— 相对化绝对路径、忽略编译目录参与 hash：
        #     build/ 与 build-msvc 等多构建目录共享同一缓存的前提。
        #   COMPRESS/LEVEL —— 缓存产物压缩存储；注意 hardlink 与压缩互斥，故不启用。
        set(_ccache_env
                "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
                "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}"
                "CCACHE_NOHASHDIR=1"
                "CCACHE_COMPRESS=1"
                "CCACHE_COMPRESSLEVEL=6")

        # 缓存目录与容量（经启动器注入后构建期真正生效）。
        set(AURORA_CCACHE_DIR "" CACHE PATH "ccache 缓存目录（默认使用系统默认）")
        if (AURORA_CCACHE_DIR)
            list(APPEND _ccache_env "CCACHE_DIR=${AURORA_CCACHE_DIR}")
        endif ()
        set(AURORA_CCACHE_MAXSIZE "5G" CACHE STRING "ccache 最大缓存大小")
        if (AURORA_CCACHE_MAXSIZE)
            list(APPEND _ccache_env "CCACHE_MAXSIZE=${AURORA_CCACHE_MAXSIZE}")
        endif ()

        set(CMAKE_C_COMPILER_LAUNCHER
                "${CMAKE_COMMAND};-E;env;${_ccache_env};${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER
                "${CMAKE_COMMAND};-E;env;${_ccache_env};${CCACHE_PROGRAM}")

        aurora_log("ccache: enabled (${CCACHE_PROGRAM})")
        aurora_log("ccache: launcher env = ${_ccache_env}")
    else ()
        aurora_log("ccache: not found, compilation caching disabled")
        aurora_log("ccache: install ccache for faster rebuilds")
    endif ()
else ()
    aurora_log("ccache: disabled by user")
endif ()
