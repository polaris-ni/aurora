# ============================================================
# AuroraThirdParty.cmake — 三方字体依赖：FreeType + HarfBuzz（源码构建，third_party/ 下）
# ------------------------------------------------------------
# 从仓库内置源码编译链接，断网可构建、版本确定；不再经 FetchContent 网络拉取。
# 关闭非必要子依赖（bzip2/png/harfbuzz），仅保留核心 TTF/Type1 栅格与真 hinting；
# 保留 zlib/brotli 以正确解压含压缩表（.ttc/.ttf）的系统字体，避免缺字豆腐块。
# 静态链接（BUILD_SHARED_LIBS=OFF）以保证消费者无需额外 DLL。
# 顺序约束：必须先 freetype 后 harfbuzz——harfbuzz 在 `if (TARGET freetype)` 时自动开启
# HB_HAVE_FREETYPE（提供 hb-ft.h 并链接 freetype）。aurora 直接 PUBLIC 链接 harfbuzz 做文本
# shaping，故 freetype 自身保持 FT_DISABLE_HARFBUZZ=ON（standalone，避免额外别名耦合）。
# ⚠️ 本模块须在顶层注入 -Wall 等告警选项之前 include，使三方源码不受项目告警配置影响。
# ============================================================

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype)

# FreeType 在 WIN32 下无条件 enable_language(RC) 并把 src/base/ftver.rc（仅一个
# VERSIONINFO 版本资源，与静态链接无关）加入 freetype 目标。MinGW 的 windres 自带
# windows.h 可编过；但 LLVM/Clang 工具链下 CMake 会选 Windows SDK 的 rc.exe，它不会
# 自动带 SDK 的 INCLUDE 路径，导致 RC1015: cannot open include file 'windows.h'。
# 该资源对静态库无关紧要，直接剔除源文件，避免硬编码 SDK 路径。
if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MINGW)
    get_target_property(_ft_srcs freetype SOURCES)
    if (_ft_srcs)
        list(FILTER _ft_srcs EXCLUDE REGEX "ftver\\.rc$")
        set_target_properties(freetype PROPERTIES SOURCES "${_ft_srcs}")
    endif ()
endif ()

# 仅保留核心 harfbuzz 库（shape 仅需核心 + hb-ft），关闭 subset/raster/vector/gpu/utils 缩短构建。
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
# 作用域内收窄 configure 输出：harfbuzz 的 CMakeLists 会打印社区维护声明（message WARNING）
# 与特性检测 STATUS——三方配置噪音不进本项目 configure 日志；ERROR 及以上仍正常显示。
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/harfbuzz)
unset(CMAKE_MESSAGE_LOG_LEVEL)

# 三方源码静音：harfbuzz 自身代码（如 hb-algs.hh 的 memcmp 边界分析）的编译告警不属
# 本项目修复范围（上游官方构建是 Meson，CMake 构建为社区维护），统一 -w 关闭其全部告警。
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(harfbuzz PRIVATE -w)
endif ()

if (NOT TARGET freetype OR NOT TARGET harfbuzz)
    aurora_error("FreeType/HarfBuzz source build did not produce the 'freetype'/'harfbuzz' targets,"
            " check third_party/freetype and third_party/harfbuzz.")
endif ()
aurora_log("Aurora font engine: FreeType + HarfBuzz (source build under third_party/)")
