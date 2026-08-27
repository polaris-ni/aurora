# ============================================================
# AuroraBackends.cmake — 后端代码剪裁（feature 宏 + CMake 开关）
# ------------------------------------------------------------
# 每个内置后端可经 CMake 开关整体剔除：关闭后对应 Surface 子类、工厂重载与重型平台头
# （<windows.h> / GLFW / OpenGL）被预处理器剔除，链接产物不再含该后端。自定义 Surface
# 注入路径（Application(Scene,unique_ptr<Surface>) 等）始终可用，故「只用自定义 backend」
# 可不编译任何内置后端。宏由 aurora 目标以 PUBLIC 编译定义传播给所有消费者。
# 全部开关/宏/环境变量统一列于 codespec/BUILD_OPTIONS.md（唯一权威来源）。
# ============================================================

# 累积已开启的 feature 宏名，供 AuroraInstall 统一导出（消除手动重复列表）。
set(AURORA_FEATURE_DEFINES "")

# ---- Headless（无头内存/PNG 后端，默认 ON） ----
option(AURORA_BACKEND_HEADLESS "Build Headless (memory/PNG) Surface backend" ON)
if (AURORA_BACKEND_HEADLESS)
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_HEADLESS)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_HEADLESS)
endif ()

# ---- Win32/GDI（Windows 默认 ON，否则 OFF） ----
if (WIN32)
    option(AURORA_BACKEND_WIN32 "Build Win32/GDI Surface backend" ON)
else ()
    option(AURORA_BACKEND_WIN32 "Build Win32/GDI Surface backend" OFF)
endif ()
if (AURORA_BACKEND_WIN32)
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_WIN32)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_WIN32)
    if (WIN32)
        # user32/gdi32：窗口与 GDI；shell32/ole32：Shell_NotifyIcon 与 COM 文件对话框；
        # uuid：CLSID/IID 常量（IFileOpenDialog 等）。
        target_link_libraries(aurora PUBLIC user32 gdi32 shell32 ole32 uuid)
    endif ()
endif ()

# ---- D3D11（GPU 增量上屏后端，需 d3d11/dxgi/d3dcompiler；默认 OFF） ----
option(AURORA_BACKEND_D3D11 "Build D3D11 (GPU incremental present) Surface backend" OFF)
if (AURORA_BACKEND_D3D11)
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_D3D11)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_D3D11)
    if (WIN32)
        target_link_libraries(aurora PUBLIC d3d11 dxgi d3dcompiler)
    endif ()
endif ()

# ---- GLFW + OpenGL（跨平台真实窗口，默认 OFF） ----
# 依赖来源：仓库内置 third_party/glfw（GLFW 3.5.1）源码构建，与 FreeType/HarfBuzz 同口径
# ——源码进仓库、断网可构建、版本确定；仅保留核心库（关 examples/tests/docs/install），
# 静态链接无 DLL 依赖。源码缺失即 FATAL（无外部安装根回退，避免二进制发行版路径漂移）。
option(AURORA_BACKEND_GLFW "Build GLFW platform Surface backend" OFF)
if (AURORA_BACKEND_GLFW)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/CMakeLists.txt")
        aurora_error("AURORA_BACKEND_GLFW=ON but third_party/glfw sources are missing"
                " (expected CMakeLists.txt and include/GLFW/glfw3.h).")
    endif ()
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # EXCLUDE_FROM_ALL：仅当 aurora 链接时连带构建，不进 `cmake --build build` 默认目标。
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw
            ${CMAKE_BINARY_DIR}/third_party/glfw EXCLUDE_FROM_ALL)
    if (NOT TARGET glfw)
        aurora_error("GLFW source build did not produce the 'glfw' target; check third_party/glfw.")
    endif ()
    # 三方源码显式压制告警（与 Wayland 生成胶水 -w、harfbuzz 告警豁免同口径）；
    # 顶层 -Wall 已改为 aurora 目标级，不再全局注入，此处 -w 仅为双保险。
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(glfw PRIVATE "-w")
    elseif (MSVC)
        target_compile_options(glfw PRIVATE "/w")
    endif ()
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_GLFW)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_GLFW)
    # glfw 目标自带 PUBLIC include/ 路径，随链接传递给消费者，无需手动加头目录。
    target_link_libraries(aurora PUBLIC glfw opengl32)
    aurora_log("GLFW backend enabled (source build under third_party/glfw)")
endif ()

# ---- X11 / Wayland / macOS / WASM ----
# X11/Wayland 用于 Linux 桌面、macOS 用于 Apple、WASM 用于 Emscripten 工具链；
# 默认构建（含本机 Windows/MinGW）不受影响，仍仅 Headless 必开。
option(AURORA_BACKEND_X11 "Enable X11 (Xlib) backend for Linux desktop (requires libX11)" OFF)
option(AURORA_BACKEND_WAYLAND "Enable native Wayland backend for Linux desktop (requires wayland-client + xkbcommon)" OFF)
option(AURORA_BACKEND_MACOS "Enable macOS (Cocoa/AppKit) backend (Apple only)" OFF)
option(AURORA_BACKEND_WASM "Enable WebAssembly (Emscripten) backend (Emscripten toolchain only)" OFF)

if (AURORA_BACKEND_X11)
    if (NOT (UNIX AND NOT APPLE))
        aurora_error("AURORA_BACKEND_X11 is only supported on Linux/Unix (non-Apple) platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_X11=OFF.")
    endif ()
    find_package(X11 REQUIRED)
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_X11)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_X11)
    target_include_directories(aurora PUBLIC ${X11_INCLUDE_DIR})
    target_link_libraries(aurora PUBLIC ${X11_LIBRARIES})
    aurora_log("X11 backend enabled: X11_LIBRARIES=${X11_LIBRARIES}")
endif ()

if (AURORA_BACKEND_WAYLAND)
    if (NOT (UNIX AND NOT APPLE))
        aurora_error("AURORA_BACKEND_WAYLAND is only supported on Linux/Unix (non-Apple) platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_WAYLAND=OFF.")
    endif ()
    # 依赖：wayland-client（线协议）+ xkbcommon（键盘 keymap）+ wayland-protocols（xdg-shell XML）
    # + wayland-scanner（协议 XML → C 胶水；生成物落在 build 目录，不入仓）。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
    pkg_get_variable(AURORA_WL_PROTO_DIR wayland-protocols pkgdatadir)
    pkg_get_variable(AURORA_WL_SCANNER wayland-scanner wayland_scanner)
    if (NOT AURORA_WL_SCANNER)
        find_program(AURORA_WL_SCANNER wayland-scanner REQUIRED)
    endif ()
    set(AURORA_WL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-gen")
    file(MAKE_DIRECTORY "${AURORA_WL_GEN_DIR}")
    # xdg-shell（窗口/toplevel，stable）与 xdg-decoration（服务端装饰，unstable v1；KDE 有/GNOME 无）。
    set(AURORA_WL_PROTOS
            "${AURORA_WL_PROTO_DIR}/stable/xdg-shell/xdg-shell.xml|xdg-shell"
            "${AURORA_WL_PROTO_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml|xdg-decoration-unstable-v1")
    set(AURORA_WL_GEN_SRCS "")
    foreach (_entry IN LISTS AURORA_WL_PROTOS)
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _xml)
        list(GET _pair 1 _name)
        set(_hdr "${AURORA_WL_GEN_DIR}/${_name}-client-protocol.h")
        set(_src "${AURORA_WL_GEN_DIR}/${_name}-protocol.c")
        add_custom_command(OUTPUT "${_hdr}" "${_src}"
                COMMAND "${AURORA_WL_SCANNER}" client-header "${_xml}" "${_hdr}"
                COMMAND "${AURORA_WL_SCANNER}" private-code "${_xml}" "${_src}"
                DEPENDS "${_xml}" VERBATIM
                COMMENT "wayland-scanner: ${_name}")
        list(APPEND AURORA_WL_GEN_SRCS "${_src}")
    endforeach ()
    # 生成的 C 胶水非本项目代码：屏蔽 -Wall/-Wpedantic 告警（不改动其内容）。
    set_source_files_properties(${AURORA_WL_GEN_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
    target_sources(aurora PRIVATE ${AURORA_WL_GEN_SRCS})
    target_include_directories(aurora PRIVATE "${AURORA_WL_GEN_DIR}" ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_WAYLAND)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_WAYLAND)
    target_link_libraries(aurora PUBLIC ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
    aurora_log("Wayland backend enabled: protocols=${AURORA_WL_PROTO_DIR} scanner=${AURORA_WL_SCANNER}")
endif ()

if (AURORA_BACKEND_MACOS)
    if (NOT APPLE)
        aurora_error("AURORA_BACKEND_MACOS is only supported on Apple platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_MACOS=OFF.")
    endif ()
    enable_language(OBJCXX)
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_MACOS)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_MACOS)
    target_link_libraries(aurora PUBLIC "-framework Cocoa" "-framework AppKit")
    # macos_surface.cpp 为 Objective-C++（Cocoa），需显式指定语言；文件存在才设置，避免 glob 缺失告警。
    set(_macos_src "${CMAKE_CURRENT_SOURCE_DIR}/src/aurora/window/macos_surface.cpp")
    if (EXISTS "${_macos_src}")
        set_source_files_properties("${_macos_src}" PROPERTIES LANGUAGE OBJCXX)
    endif ()
    aurora_log("macOS backend enabled (Cocoa/AppKit).")
endif ()

if (AURORA_BACKEND_WASM)
    if (NOT EMSCRIPTEN)
        aurora_error("AURORA_BACKEND_WASM is only supported with the Emscripten toolchain;"
                " configure with emcmake cmake, and disable with -DAURORA_BACKEND_WASM=OFF in non-Emscripten environments.")
    endif ()
    target_compile_definitions(aurora PUBLIC AURORA_BACKEND_WASM)
    list(APPEND AURORA_FEATURE_DEFINES AURORA_BACKEND_WASM)
    aurora_log("WASM backend enabled (Emscripten).")
endif ()

# ---- 架构级优化开关（性能，独立退化，默认开启） ----
# 三项互不依赖的渲染/布局优化；宏由 aurora 目标以 PUBLIC 编译定义传播给所有消费者。
# 关闭任一开关即回退到原始实现路径（等价无优化）。
option(AURORA_LAYOUT_CACHE "Enable layout constraint cache (skip redundant subtree layout)" ON)
option(AURORA_OCCLUSION_CULLING "Enable occlusion culling (skip children outside clip region)" ON)
option(AURORA_DISPLAY_LIST "Enable display list record/replay (skip unchanged subtree paint)" ON)
foreach (_opt AURORA_LAYOUT_CACHE AURORA_OCCLUSION_CULLING AURORA_DISPLAY_LIST)
    if (${_opt})
        target_compile_definitions(aurora PUBLIC ${_opt})
        list(APPEND AURORA_FEATURE_DEFINES ${_opt})
    endif ()
endforeach ()
