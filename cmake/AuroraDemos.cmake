# ============================================================
# AuroraDemos.cmake — 示例 demo 目标（examples/demos/ 下每组件一个可运行窗口；examples/app/ 下为应用级演示，如 google_play）
# ------------------------------------------------------------
# 从主 CMakeLists.txt 抽出的独立模块，避免主文件被 60+ 可执行目标的样板污染。
# 依赖（均在主文件更早就绪）：aurora 主库、aurora_setup_consumer_target（AuroraUtils）、
# aurora_inspector_server（AuroraTools，仅 AURORA_BUILD_INSPECTOR_SERVER=ON 时定义）。
# ⚠️ 本模块须在主文件 include(AuroraTools) 之后 include：demo_google_play 的 InspectorServer
# 远程检视接线需要 aurora_inspector_server 目标已存在（见下方 hook）。
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
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/app/google_play/*.cpp")
    add_custom_target(demos COMMENT "Building all Aurora demos")
    foreach (demo_src ${AURORA_DEMO_SOURCES})
        get_filename_component(demo_name ${demo_src} NAME_WE)
        get_filename_component(demo_dir ${demo_src} DIRECTORY)
        add_executable(${demo_name} EXCLUDE_FROM_ALL ${demo_src})
        # 统一配置：链接 aurora + C++20 + 复用消费者 PCH + 告警标志 + 私有 include。
        # demo 通过链接 aurora 自动获得其 PUBLIC 导出的 AURORA_ENABLE_DEBUG 等宏（Widget 布局依赖，须同值）。
        # include 同时含 examples/demos（共享头 demo_common.h）与 demo 自身所在目录
        #（如 app/google_play 下的 google_play_ui.h / google_play_data.h）。
        aurora_setup_consumer_target(${demo_name}
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/demos"
                "${demo_dir}")
        add_dependencies(demos ${demo_name})
    endforeach ()
    aurora_log("Aurora demos enabled (on-demand: --target <demo_name> | --target demos)")
else ()
    aurora_log("Aurora demos disabled (set AURORA_BUILD_DEMOS=ON to define them)")
endif ()

# demo_google_play 接入 InspectorServer 远程检视：主文件已保证本模块在 include(AuroraTools) 之后引入，
# 此时 aurora_inspector_server 目标（AURORA_BUILD_INSPECTOR_SERVER=ON 时定义，跨平台）已存在。
# 其 PUBLIC 导出的 AURORA_INSPECTOR_SERVER_ENABLED 宏随链接注入 demo，启用 demo 内 HTTP 远程检视代码；
# Release / 未开选项时该目标不存在 → 跳过，demo 内 InspectorServer 分支整编译剔除，零链接依赖。
if (AURORA_BUILD_DEMOS AND TARGET demo_google_play AND TARGET aurora_inspector_server)
    target_link_libraries(demo_google_play PRIVATE aurora_inspector_server)
endif ()

# Google Play 数据层测试（tests/ 下的 google_play_data / google_play_ui / test_play_repository）
# 需要包含 examples/app/google_play 下的头文件（google_play_data.h、google_play_ui.h）。这些测试目标由更先
# include 的 AuroraTests 定义，此处按 TARGET 存在性注入 demo 私有 include 路径，使数据层测试可编译
# demo 组件；不门控于 AURORA_BUILD_DEMOS（demo 头文件始终存在，测试编译仍需该路径），
# AURORA_BUILD_TESTS=OFF 时目标不存在 → 跳过，零耦合污染主 tests 模块。
foreach (_gp google_play_data google_play_ui test_play_repository)
    if (TARGET ${_gp})
        target_include_directories(${_gp} PRIVATE "${CMAKE_SOURCE_DIR}/examples/app/google_play")
    endif ()
endforeach ()
