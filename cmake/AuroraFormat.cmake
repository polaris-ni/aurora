# ============================================================
# AuroraFormat.cmake — clang-format 门禁（按需聚合目标，不进默认构建）
# ------------------------------------------------------------
# 提供两个目标：
#   format-check — 只读校验 first-party 源码是否符合仓库根 .clang-format；
#                  有任何一处不一致即退出码 1（CI 用）。
#   format       — 就地重写（等价于 clang-format -i），供人工整理后 git diff 审阅。
#
# 与 clang-tidy 门禁的分工：
#   lint        — 语义 / 检查项；
#   format-*    — 纯排版。两者互不重叠，也都不进默认构建，避免拖慢日常编译。
#
# 为什么要有这个门禁：2026-09-21 之前仓库**没有任何** clang-format 调用点，排版完全靠手，
#   导致配置（PointerAlignment: Left）与代码库实际写法（右对齐，约 8.4 : 1）长期背离，
#   累积到 488/823 文件、约 1.5 万行不一致也没被发现。门禁存在的意义就是让这类漂移
#   在**引入的那一刻**暴露，而不是攒到需要一次性大改。
#
# ⚠️ 依赖 tools/check/run_clang_format.py。脚本内的 third_party 排除 + 绝对路径调用形态
#    是正确性的一部分，勿在外层自行传文件列表绕过。
# ⚠️ 可执行文件由 aurora_find_clang_format 选出并显式传给脚本（--clang-format），不靠脚本自己
#    在 PATH 上撞运气：本仓 `.clang-format` 用了 v20+ 才认的枚举取值（`BinPackParameters: BinPack`），
#    发行版旧版会 `error: invalid boolean` 后整条命令失败；与生成链（AuroraTools 的
#    generate_error_codes）取同一个判据，两处不会落在不同版本上。
# ============================================================

option(AURORA_ENABLE_CLANG_FORMAT "Provide the 'format' / 'format-check' aggregate targets (clang-format)" ON)
if (NOT AURORA_ENABLE_CLANG_FORMAT)
    aurora_log("clang-format: disabled (AURORA_ENABLE_CLANG_FORMAT=OFF)")
    return ()
endif ()

aurora_find_clang_format(AURORA_CLANG_FORMAT_BIN)
find_program(PYTHON3_EXE NAMES python3 python)

if (NOT AURORA_CLANG_FORMAT_BIN)
    aurora_warn("AURORA_ENABLE_CLANG_FORMAT=ON but no clang-format on PATH parses the repo .clang-format "
                "(probed: ${AURORA_CLANG_FORMAT_CANDIDATES}); 'format' targets skipped. "
                "Install clang-format >= 20, or point AURORA_CLANG_FORMAT_CANDIDATES at one.")
    return ()
endif ()
if (NOT PYTHON3_EXE)
    aurora_warn("AURORA_ENABLE_CLANG_FORMAT=ON but no python interpreter found; 'format' targets skipped.")
    return ()
endif ()

set(_format_script "${CMAKE_SOURCE_DIR}/tools/check/run_clang_format.py")
if (NOT EXISTS "${_format_script}")
    aurora_warn("clang-format: runner script missing (${_format_script}); 'format' targets skipped.")
    return ()
endif ()

add_custom_target(format-check
        COMMAND ${PYTHON3_EXE} "${_format_script}" --jobs 8 --clang-format "${AURORA_CLANG_FORMAT_BIN}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-format: verifying first-party sources against .clang-format (fails on any divergence)")

add_custom_target(format
        COMMAND ${PYTHON3_EXE} "${_format_script}" --fix --jobs 8 --clang-format "${AURORA_CLANG_FORMAT_BIN}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-format: rewriting first-party sources in place (review 'git diff' before committing)")

aurora_log("clang-format: 'format' / 'format-check' targets available (${AURORA_CLANG_FORMAT_BIN})")
