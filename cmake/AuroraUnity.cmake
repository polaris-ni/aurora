# ============================================================
# AuroraUnity.cmake — Unity（jumbo）构建开关
# ------------------------------------------------------------
# 把目标的多个源文件合并为少量 unity TU（生成 #include 原文件的批处理源），
# 摊薄「每个 TU 各解析一遍 aurora.h 伞头（315 个公共头）」的重复成本，
# 是全量 / 冷构建场景的编译加速旋钮。
#
# ⚠️ 选型理由（与 ccache 的相互关系，实测数据见 BUILD_OPTIONS.md §4.8）：
#   unity 用「单文件迭代变慢」换「全量构建变快」——改动一个文件会令其所在
#   整个批次（AURORA_UNITY_BATCH_SIZE 个文件）一起重编，且批次内容变了
#   ccache 亦不命中。故缺省 OFF，由使用者按场景显式开启（大重构、CI 全量）。
#
# ⚠️ 插桩构建（覆盖率 / ASan）强制关闭：覆盖率行归属会全部落到生成的 unity
#   文件上，per-file 归因失效；ASan 的符号归因同样劣化。与主文件 PCH 门控
#   （AURORA_PCH_ENABLED）同口径——同样依赖 -D 传入时 cache 项已存在。
#
# ⚠️ clang-tidy 门禁构建（-DAURORA_ENABLE_CLANG_TIDY=ON，CI lint 作业形态）
#   强制关闭：lint 从 compile_commands.json 取 TU 清单，unity 下清单变成构建
#   目录里的生成文件（内容 = 被合并的全部源），按路径的排除名单、分片与
#   --git-diff 挑选全部失真。lint 需要真实 TU 路径，故该配置下不合并。
#
# 生效目标：aurora 库、aurora_test_runner（含分片）、aurora_add_tool 族与
# aurora_json；单文件目标（各 demo / gen_* 生成器）合并无收益，自动为空操作。
# 覆盖率 / ASan / clang-tidy 之外的消费者（demo / 工具）经
# aurora_setup_consumer_target 统一接入，无需逐个目标接线。
# ============================================================

option(AURORA_UNITY_BUILD
        "Combine target sources into unity (jumbo) batches to speed up full builds; slows single-file iteration" OFF)

set(AURORA_UNITY_BATCH_SIZE "16" CACHE STRING
        "Maximum sources per unity TU when AURORA_UNITY_BUILD=ON (larger = faster full builds, slower single-file rebuilds)")
if (NOT AURORA_UNITY_BATCH_SIZE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "AURORA_UNITY_BATCH_SIZE must be a positive integer (got '${AURORA_UNITY_BATCH_SIZE}')")
endif ()

# aurora_apply_unity(<tgt>)：对单个目标应用 unity 属性。
# 空操作条件：开关关闭 / 插桩构建 / clang-tidy 门禁构建。
function(aurora_apply_unity _tgt)
    if (NOT AURORA_UNITY_BUILD)
        return()
    endif ()
    # 插桩关闭口径与主文件 AURORA_PCH_ENABLED 一致：依赖 -D 传入时 cache 项
    # 在 include 本模块前已存在（option() 定义在 AuroraInstrumentation，最后 include）。
    if (AURORA_ENABLE_COVERAGE OR AURORA_ENABLE_ASAN)
        return()
    endif ()
    # clang-tidy 门禁构建：lint 从 compile_commands.json 取 TU 清单，unity 下清单
    # 变成构建目录里的生成文件（内容 = 被合并的全部源），按路径的排除名单、分片与
    # --git-diff 挑选全部失真；CI lint 作业经 -DAURORA_ENABLE_CLANG_TIDY=ON 传入，
    # 需要真实 TU 路径，故其构建目录不合并。
    # ⚠️ 极性判定（勿改）：该 option 默认 ON，其 cache 项在**首次 configure 后即存在**，
    # 若按「clang-tidy 当前值」直接判定，普通构建目录的任何一次 reconfigure
    # （glob verify / cmake 文件变更）都会把 unity 悄悄关掉——首次 configure 时
    # option() 尚未运行、项不存在故放行，之后恒存在故拦截。改为「clang-tidy 开
    # **且** 用户未显式开 unity 才跳过」：AURORA_UNITY_BUILD=ON 由 -D 持久进 cache，
    # 用户意图跨 reconfigure 稳定；显式同时开两者时以用户意图为准（lint 将看到
    # unity TU，属已知取舍，见 BUILD_OPTIONS.md §4.8）。
    if (AURORA_ENABLE_CLANG_TIDY AND NOT AURORA_UNITY_BUILD)
        return()
    endif ()
    set_target_properties(${_tgt} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE ${AURORA_UNITY_BATCH_SIZE})
endfunction()

# aurora_unity_exclude(<路径模式>...)：把匹配到的源文件标记为「不进 unity 批次」
# （SKIP_UNITY_BUILD_INCLUSION，CMake 4.4 起对普通源文件生效）——合并进同一 TU
# 会 ODR 重定义的文件独立编译，与关闭 unity 时逐字节相同，其余 TU 照常合并。
# 用法传目录（递归）或单文件；开关关闭时为空操作。
function(aurora_unity_exclude)
    if (NOT AURORA_UNITY_BUILD)
        return()
    endif ()
    foreach (_pat ${ARGN})
        file(GLOB_RECURSE _excl CONFIGURE_DEPENDS "${_pat}")
        if (_excl)
            set_source_files_properties(${_excl} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
            list(LENGTH _excl _n)
            aurora_log("Unity: excluded ${_n} file(s) from batching (ODR): ${_pat}")
        endif ()
    endforeach ()
endfunction()
