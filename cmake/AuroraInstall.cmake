# ============================================================
# AuroraInstall.cmake — 安装 + find_package(Aurora) 支持
# ------------------------------------------------------------
# 采用手写 AuroraConfig.cmake（不依赖 third_party 自带的 export 集），
# 以避免整图导出冲突与 ZLIB::ZLIB 等跨工程引用失效。消费者只需：
#   find_package(Aurora REQUIRED)
#   target_link_libraries(app PRIVATE Aurora::aurora)
# ⚠️ 本模块须在后端开关模块之后 include（导出的 feature 宏取各开关最终取值）。
# ============================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1) 收集需随安装导出的 PUBLIC 编译定义（feature 宏 + 全局宏），与编译期 aurora 目标一致。
#    feature 宏名由 AuroraBackends.cmake 累积到 AURORA_FEATURE_DEFINES（仅含已开启者），
#    直接遍历即可，避免手动列表与开关定义脱节。
set(AURORA_EXPORTED_DEFINES "")
foreach (_d ${AURORA_FEATURE_DEFINES})
    list(APPEND AURORA_EXPORTED_DEFINES ${_d})
endforeach ()
list(APPEND AURORA_EXPORTED_DEFINES NOMINMAX)

# 性能埋点宏只在**显式强制 ON** 时随安装导出：AUTO 模式依赖构建类型的生成式表达式，
# 无法静态导出，而安装产物按发布态语义应为关闭。埋点宏不改变任何类型的内存布局，
# 故库与消费者取值不一致亦不构成 ODR/ABI 问题（见 AuroraInstrumentation.cmake）。
if (AURORA_PROFILING_FORCED_ON)
    list(APPEND AURORA_EXPORTED_DEFINES AURORA_ENABLE_PROFILING)
endif ()
if (AURORA_ENABLE_TRACING)
    list(APPEND AURORA_EXPORTED_DEFINES AURORA_ENABLE_TRACING)
endif ()

# 2) 安装 aurora 静态库与公共头（aurora.h 传递包含 third_party/nlohmann/json.hpp）。
install(TARGETS aurora ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/aurora DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY third_party/nlohmann DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# 3) 安装 harfbuzz 静态库与公共头（freetype 自带 install 规则已安装自身；二者均被 aurora PUBLIC 链接）。
install(TARGETS harfbuzz ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY third_party/harfbuzz/src/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/harfbuzz
        FILES_MATCHING PATTERN "hb.h" PATTERN "hb-*.h")

# 4) 生成并安装包配置文件。
set(_aurora_cfg_out "${CMAKE_CURRENT_BINARY_DIR}/cmake")
configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuroraConfig.cmake.in"
        "${_aurora_cfg_out}/AuroraConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Aurora
        PATH_VARS CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR)
write_basic_package_version_file(
        "${_aurora_cfg_out}/AuroraConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
install(FILES
        "${_aurora_cfg_out}/AuroraConfig.cmake"
        "${_aurora_cfg_out}/AuroraConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Aurora)
