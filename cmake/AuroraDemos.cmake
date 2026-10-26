# ============================================================
# AuroraDemos.cmake — 示例 demo 目标（examples/demos/ 下每组件一个可运行窗口）
# ------------------------------------------------------------
# 从主 CMakeLists.txt 抽出的独立模块，避免主文件被 60+ 可执行目标的样板污染。
# 依赖（均在主文件更早就绪）：aurora 主库、aurora_setup_consumer_target（AuroraUtils）、aurora_inspector_server。
# ⚠️ 本模块须在主文件 include(AuroraTools) 之后 include：全部 demo 统一条件链接
# aurora_inspector_server（见下方 hook），该目标此时必须已存在。
# ============================================================

# 每个 demo_<组件>.cpp 独立成可执行程序，通过共享头 demos/demo_common.h 复用
# 调色板与窗口启动器（run_demo）。顶层 examples/*.cpp 的聚合/重复 demo 已清理，
# 统一收敛到 examples/demos/ 的 1:1 结构。
# demo 目标不进默认构建（EXCLUDE_FROM_ALL）：日常 `cmake --build build` 不再连带
# 编译链接全部 60+ demo；按名构建单个（--target demo_lazy_list）或聚合全建
# （--target demos）。关闭 AURORA_BUILD_DEMOS 则连目标都不定义。
option(AURORA_BUILD_DEMOS "Define the per-widget runnable demo targets (built on demand, not in ALL)" ON)
if (AURORA_BUILD_DEMOS)
    file(GLOB AURORA_DEMO_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos/*.cpp")
    add_custom_target(demos COMMENT "Building all Aurora demos")
    foreach (demo_src ${AURORA_DEMO_SOURCES})
        get_filename_component(demo_name ${demo_src} NAME_WE)
        get_filename_component(demo_dir ${demo_src} DIRECTORY)
        add_executable(${demo_name} EXCLUDE_FROM_ALL ${demo_src})
        # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志 + 私有 include。
        # demo 通过链接 aurora 自动获得其 PUBLIC 导出的 AURORA_ENABLE_DEBUG 等宏（Widget 布局依赖，须同值）。
        # include 含 examples/demos（共享头 demo_common.h）与 demo 自身所在目录（demos/ 下的私有头）。
        aurora_setup_consumer_target(${demo_name}
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos"
                "${demo_dir}")
        add_dependencies(demos ${demo_name})
    endforeach ()

    # demo_i18n 的外部词表走构建期生成（cmake/AuroraI18n.cmake 的唯一 in-tree 消费 leg，
    # 亦为其用法示范）：强类型 Id / 具名形参构造函数出自 examples/demos/i18n/*.json，
    # 译文不落源码（check_no_cjk_literals 口径：demo 词条全走外部数据）。
    if (TARGET demo_i18n)
        aurora_i18n_generate(TARGET demo_i18n
                NAMESPACE demo_i18n::msg
                BASE "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos/i18n/messages_en.json"
                LOCALES "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos/i18n/messages_zh-Hans-CN.json"
                OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/demo_i18n_i18n")
    endif ()

    aurora_log("Aurora demos enabled (on-demand: --target <demo_name> | --target demos)")
else ()
    aurora_log("Aurora demos disabled (set AURORA_BUILD_DEMOS=ON to define them)")
endif ()

# 接入 InspectorServer 远程检视的 demo：主文件已保证本模块在 include(AuroraTools) 之后引入，
# 此时 aurora_inspector_server 目标（AURORA_BUILD_INSPECTOR_SERVER=ON 时定义，跨平台）已存在。
# 其 PUBLIC 导出的 AURORA_BUILD_INSPECTOR_SERVER 宏随链接注入 demo，启用 demo 内 HTTP 远程检视代码；
# Release / 未开选项时该目标不存在 → 跳过，demo 内 InspectorServer 分支整编译剔除，零链接依赖。
# hook 覆盖**全部** demo（E2E 驱动任意 demo 的前提）：run_demo 为所有 demo 共用的启动器，
# 其 opt-in 启动块要求同一宏口径——只给个别 demo 链接会让共享头在不同 demo 下编译出不同形态。
# 运行期是否启动由环境变量 AURORA_INSPECTOR_PORT 决定（demo_common.h run_demo），未设置不启动。
if (AURORA_BUILD_DEMOS AND TARGET aurora_inspector_server)
    foreach (inspector_demo_src ${AURORA_DEMO_SOURCES})
        get_filename_component(inspector_demo ${inspector_demo_src} NAME_WE)
        target_link_libraries(${inspector_demo} PRIVATE aurora_inspector_server)
    endforeach ()
endif ()
