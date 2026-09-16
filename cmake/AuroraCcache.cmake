# ============================================================
# AuroraCcache.cmake — ccache 编译缓存集成
# ------------------------------------------------------------
# 提供 ccache 编译缓存支持，加速重复编译。
# 可通过 -DAURORA_ENABLE_CCACHE=OFF 禁用。
#
# ⚠️ 配置注入机制：CMake 的 set(ENV{...}) 仅在 configure 期生效，不会传递给
# ninja/VS 构建期子进程——凡 configure 期写入的环境变量对实际编译全部无效。
# 因此所有 ccache 配置统一经编译器启动器注入：
#     CMAKE_{C,CXX}_COMPILER_LAUNCHER = cmake -E env CCACHE_*=... ccache [<用户选项>]
# 「cmake -E env」在每个编译边构建期展开，环境精确作用于本项目的 ccache 调用，
# 不污染用户全局环境，也不依赖构建 shell 是否导出过变量；对 Ninja / Make 各生成器
# 与 GCC/Clang 各编译器适用。
#
# ⚠️ MSVC（cl）不接入：ccache 对 MSVC 的 PCH 旗标组合（/Yu + /FI + /Fp）支持不完整，
# 会把强制包含改写后丢失 PCH 边界匹配 → C1010（查找预编译头时遇到意外的文件结尾）。
# 且 VS 多配置生成器本就不实现 <LANG>_COMPILER_LAUNCHER（Ninja + cl 同样命中此坑），
# 故编译器为 MSVC 时整体跳过注入，靠 PCH 提速（PCH 在 MSVC 为正收益，见 BUILD_OPTIONS）。
#
# ⚠️ Aurora 作为三方库不主动安装 ccache：仅在 PATH 中查找；未找到则提示用户自行安装，
# 缓存关闭不影响构建正确性。用户可通过 -DAURORA_CCACHE_OPTIONS="..." 注入任意 ccache
# 命令行选项——一旦设置，即直接使用用户输入，不再注入 Aurora 默认 CCACHE_* 配置。
# ============================================================

option(AURORA_ENABLE_CCACHE "Use ccache for compilation caching" ON)

if (AURORA_ENABLE_CCACHE)
    # Aurora 不主动安装 ccache：仅在 PATH 中查找，不扫描 winget 等安装目录。
    find_program(CCACHE_PROGRAM ccache)

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
        # 用户自定义 ccache 命令行选项：若设置 AURORA_CCACHE_OPTIONS，则直接使用用户输入，
        # 不再注入 Aurora 默认的 CCACHE_* 环境配置（用户自行承担完整配置责任）。
        #   cmake -S . -B build -DAURORA_CCACHE_OPTIONS="--max-size=5G --sloppiness=pch_defines,time_macros"
        # 注意：用户选项会完全取代默认；若需 PCH 缓存命中，须自行包含
        #   --sloppiness=pch_defines,time_macros,include_file_mtime,include_file_ctime
        set(AURORA_CCACHE_OPTIONS "" CACHE STRING
                "Extra ccache CLI options passed verbatim (e.g. --max-size=5G); if set, overrides Aurora defaults")

        if (AURORA_CCACHE_OPTIONS)
            # 按 shell 语义拆分用户选项（支持引号包裹的带空格参数），直接透传给 ccache。
            separate_arguments(_aurora_ccache_user_opts UNIX_COMMAND "${AURORA_CCACHE_OPTIONS}")
            set(_aurora_ccache_launcher "${CCACHE_PROGRAM}" ${_aurora_ccache_user_opts})
            aurora_log("ccache: using user-provided AURORA_CCACHE_OPTIONS = ${AURORA_CCACHE_OPTIONS}")
        else ()
            # Aurora 默认配置（经 cmake -E env 注入构建期生效）：
            #   SLOPPINESS —— PCH 场景必需：未设 pch_defines/time_macros 时，命令行带
            #     -include .../cmake_pch.hxx 的调用被 ccache 直接判 Uncacheable（消费者
            #     TU 全量裸编、缓存形同虚设）；include_file_mtime/ctime 让头文件时间戳
            #     变化而内容不变时仍可命中（preprocessor 模式按内容摘要，安全）。
            #   BASEDIR + NOHASHDIR —— 相对化绝对路径、忽略编译目录参与 hash：
            #     build/ 与 build-msvc 等多构建目录共享同一缓存的前提。
            #   COMPRESS/LEVEL —— 缓存产物压缩存储；注意 hardlink 与压缩互斥，故不启用。
            set(_aurora_ccache_env
                    "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
                    "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}"
                    "CCACHE_NOHASHDIR=1"
                    "CCACHE_COMPRESS=1"
                    "CCACHE_COMPRESSLEVEL=6")

            # 缓存目录与容量（经启动器注入后构建期真正生效）。
            set(AURORA_CCACHE_DIR "" CACHE PATH "ccache 缓存目录（默认使用系统默认）")
            if (AURORA_CCACHE_DIR)
                list(APPEND _aurora_ccache_env "CCACHE_DIR=${AURORA_CCACHE_DIR}")
            endif ()
            set(AURORA_CCACHE_MAXSIZE "5G" CACHE STRING "ccache 最大缓存大小")
            if (AURORA_CCACHE_MAXSIZE)
                list(APPEND _aurora_ccache_env "CCACHE_MAXSIZE=${AURORA_CCACHE_MAXSIZE}")
            endif ()

            set(_aurora_ccache_launcher
                    "${CMAKE_COMMAND};-E;env;${_aurora_ccache_env};${CCACHE_PROGRAM}")
            aurora_log("ccache: launcher env = ${_aurora_ccache_env}")
        endif ()

        set(CMAKE_C_COMPILER_LAUNCHER "${_aurora_ccache_launcher}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_aurora_ccache_launcher}")

        aurora_log("ccache: enabled (${CCACHE_PROGRAM})")
    else ()
        # 未找到 ccache：Aurora 作为三方库不主动安装，仅提示用户自行安装；
        # 缓存关闭不影响构建正确性与可用性。
        aurora_log("ccache: not found in PATH, compilation caching disabled")
        aurora_log("ccache: install ccache to speed up rebuilds (caching off does not affect build correctness):")
        aurora_log("ccache:   Windows : winget install ccache")
        aurora_log("ccache:   Linux   : apt install ccache   (or your distro's package manager)")
        aurora_log("ccache:   macOS   : brew install ccache")
    endif ()
else ()
    aurora_log("ccache: disabled by user")
endif ()
