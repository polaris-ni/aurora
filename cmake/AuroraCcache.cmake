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
# 缓存关闭不影响构建正确性。各 ccache 配置默认由框架经 env 注入（含 PCH 必需的
# SLOPPINESS），用户侧仅暴露单一变量 AURORA_CCACHE_ARGS 做追加/覆盖：
#   -DAURORA_CCACHE_ARGS="--max-size=20G --max-files=1000000"
# 该变量追加在框架默认之后；ccache CLI 选项优先于同名 CCACHE_* 环境变量，覆盖直观可控。
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
        # ---- 框架默认（经 env 注入，保 PCH 可缓存 + 多构建目录共享）----
        # 这些不是用户可读变量；要改任意一项，用 AURORA_CCACHE_ARGS（见下）。
        # ccache CLI 选项优先级高于同名 CCACHE_* 环境变量（CLI > env > config > builtin），
        # 故用户通过 ARGS 显式覆盖时直观可控。
        set(_aurora_ccache_env
                "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
                "CCACHE_BASEDIR=${AURORA_SOURCE_DIR}"
                "CCACHE_NOHASHDIR=1"
                "CCACHE_COMPRESS=1"
                "CCACHE_COMPRESSLEVEL=6"
                "CCACHE_MAXSIZE=10G")

        # ---- 单一用户覆盖点：AURORA_CCACHE_ARGS ----
        # 追加式 ccache 命令行选项，嵌在框架默认之后。用于覆盖任意默认或设置本框架未
        # 单列的高级开关，例如：
        #   -DAURORA_CCACHE_ARGS="--max-size=20G --max-files=1000000 --cache-dir=/path/to/ccache"
        # 注：CLI 选项会写入用户缓存的 ccache.conf 并持久生效；上方 CCACHE_* 环境注入不持久。
        # 若需完全接管配置，用 -DAURORA_ENABLE_CCACHE=OFF 关闭后自行在构建环境配置 ccache。
        set(AURORA_CCACHE_ARGS "" CACHE STRING
                "Extra ccache CLI options appended after Aurora defaults, e.g. --max-size=20G --max-files=1000000")

        # 启动器：cmake -E env <框架默认 CCACHE_*> ccache [<用户 ARGS>] <编译器>
        set(_aurora_ccache_launcher "${CMAKE_COMMAND};-E;env;${_aurora_ccache_env};${CCACHE_PROGRAM}")
        if (AURORA_CCACHE_ARGS)
            separate_arguments(_aurora_ccache_user_args UNIX_COMMAND "${AURORA_CCACHE_ARGS}")
            list(APPEND _aurora_ccache_launcher ${_aurora_ccache_user_args})
            aurora_log("ccache: extra user args appended (AURORA_CCACHE_ARGS) = ${AURORA_CCACHE_ARGS}")
        endif ()

        set(CMAKE_C_COMPILER_LAUNCHER "${_aurora_ccache_launcher}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_aurora_ccache_launcher}")

        aurora_log("ccache: enabled (${CCACHE_PROGRAM})")
        aurora_log("ccache: launcher env = ${_aurora_ccache_env}")
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
