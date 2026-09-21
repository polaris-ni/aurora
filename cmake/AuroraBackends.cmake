# ============================================================
# AuroraBackends.cmake — 后端代码剪裁（feature 宏 + CMake 开关）
# ------------------------------------------------------------
# 每个内置 Surface 图形后端可经 AURORA_BACKEND_* 开关整体剔除：关闭后
# 对应 Surface 子类、工厂重载与重型平台头（<windows.h> / GLFW / OpenGL）被预处理器
# 剔除，链接产物不再含该后端。音频设备后端开关（AURORA_ENABLE_AUDIO / AURORA_ENABLE_AUDIO_WASAPI /
# AURORA_ENABLE_AUDIO_ALSA，ENABLE 组）亦定义于本文件末段。自定义注入路径（自定义 Surface 经
# Application(Scene,unique_ptr<Surface>)、自定义 AudioDeviceBackend 经 AudioContext 构造注入）
# 始终可用，故「只用自定义 backend」可不编译任何内置后端。feature 宏由 aurora 目标以
# PUBLIC 编译定义传播给所有消费者。
# 全部开关/宏/环境变量统一列于 codespec/BUILD_OPTIONS.md（唯一权威来源）。
# ============================================================

# 累积已开启的 feature 宏名，供 AuroraInstall 统一导出（消除手动重复列表）。
# 初始化位于 AuroraFeatures.cmake（早于本模块 include，且编解码宏等更早调用点也覆盖）。

# ---- Headless（无头内存/PNG 后端，默认 ON） ----
option(AURORA_BACKEND_HEADLESS "Build Headless (memory/PNG) Surface backend" ON)
if (AURORA_BACKEND_HEADLESS)
    aurora_define_feature(AURORA_BACKEND_HEADLESS EXPORT)
endif ()

# ---- Win32/GDI（Windows 默认 ON，否则 OFF） ----
if (WIN32)
    option(AURORA_BACKEND_WIN32 "Build Win32/GDI Surface backend" ON)
else ()
    option(AURORA_BACKEND_WIN32 "Build Win32/GDI Surface backend" OFF)
endif ()
if (AURORA_BACKEND_WIN32)
    aurora_define_feature(AURORA_BACKEND_WIN32 EXPORT)
    if (WIN32)
        # user32/gdi32：窗口与 GDI；shell32/ole32：Shell_NotifyIcon 与 COM 文件对话框；
        # uuid：CLSID/IID 常量（IFileOpenDialog 等）；imm32：IMM32 组合输入桥
        # （ImmGetContext / ImmGetCompositionStringW / ImmSetCandidateWindow，见 window/detail/win32_ime.*）。
        target_link_libraries(aurora PUBLIC user32 gdi32 shell32 ole32 uuid imm32)
    endif ()
endif ()

# ---- D3D11（GPU 增量上屏后端，需 d3d11/dxgi/d3dcompiler；默认 OFF） ----
option(AURORA_BACKEND_D3D11 "Build D3D11 (GPU incremental present) Surface backend" OFF)
if (AURORA_BACKEND_D3D11)
    aurora_define_feature(AURORA_BACKEND_D3D11 EXPORT)
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
    aurora_define_feature(AURORA_BACKEND_GLFW EXPORT)
    # glfw 目标自带 PUBLIC include/ 路径，随链接传递给消费者，无需手动加头目录。
    # OpenGL 库名按平台区分：Windows 用系统 opengl32.lib；类 Unix（X11/Wayland）用 CMake
    # 标准导入目标 OpenGL::GL（即 libGL.so）。原代码无条件写死 opengl32，会在非 Windows 平台
    # 链接失败（cannot find -lopengl32）。
    if (WIN32)
        target_link_libraries(aurora PUBLIC glfw opengl32)
    else ()
        find_package(OpenGL REQUIRED)
        target_link_libraries(aurora PUBLIC glfw OpenGL::GL)
    endif ()
    aurora_log("GLFW backend enabled (source build under third_party/glfw)")
endif ()

# ---- GLFW GPU GL（DisplayList 的 OpenGL 3.3 core GPU 栅格能力；默认 OFF；依赖 AURORA_BACKEND_GLFW） ----
# 依赖 AURORA_BACKEND_GLFW：GL 上下文创建 / swapBuffers 呈现均由 GlfwSurface 承担，本能力
# 只实现「DisplayList → GL 批渲染」（自写最小函数表 loader，无 GLAD/gl3w 三方依赖）。
# 开启后 `GlfwSurface::Config` 提供 GPU 渲染模式；初始化失败（驱动过老/无 GL）运行期
# 自动回退软件纹理上传路径，不抛异常。注意：此选项**不是**独立 `Surface` 后端（无
# `SurfaceKind`），仅切换 GLFW 窗口的栅格实现，故名为 `AURORA_ENABLE_*` 而非 `AURORA_BACKEND_*`。
option(AURORA_ENABLE_GLFW_GPU_GL "Enable GLFW GPU OpenGL 3.3 core DisplayList raster (requires AURORA_BACKEND_GLFW)" OFF)
if (AURORA_ENABLE_GLFW_GPU_GL)
    if (NOT AURORA_BACKEND_GLFW)
        aurora_error("AURORA_ENABLE_GLFW_GPU_GL requires AURORA_BACKEND_GLFW=ON"
                " (GL context creation and present are owned by the GLFW backend).")
    endif ()
    aurora_define_feature(AURORA_ENABLE_GLFW_GPU_GL EXPORT)
    aurora_log("GLFW GPU GL raster enabled (OpenGL 3.3 core DisplayList raster)")
endif ()

# ---- GPU WGPU（wgpu-native RHI 后端，跨平台 GPU 主力：Vulkan/D3D12/Metal/GL 自动选择；默认 OFF） ----
# 依赖来源：仓库内置 third_party/wgpu-native（Rust，gfx-rs v29.0.1.1，源码保持上游原样、
# 不做本地修改）经 cargo 构建为静态库（staticlib）链接——对齐「静态交付、消费者无额外 DLL」
# 口径。与 freetype/harfbuzz 的差异：Rust crate 依赖图按平台/工具链三元组而异，**不做
# cargo vendor 入库**，首次构建由 cargo 在线自 crates.io 拉取（国内可配 rsproxy 镜像，见
# codespec/BUILD_OPTIONS.md）；拉取成功后 cargo 本地缓存即支持断网增量构建。
# 构建机前提：rustup 工具链（host 三元组须与 C++ 编译器 ABI 一致：MinGW↔windows-gnu、
# MSVC↔windows-msvc、Linux↔gnu），缺失或不匹配时 configure 阶段 FATAL 并给出安装指引。
option(AURORA_BACKEND_GPU_WGPU "Build wgpu-native GPU RHI backend (WgpuRhi; requires Rust toolchain)" OFF)
if (AURORA_BACKEND_GPU_WGPU)
    set(_wgpu_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native")
    if (NOT EXISTS "${_wgpu_src}/Cargo.toml" OR NOT EXISTS "${_wgpu_src}/ffi/wgpu.h"
            OR NOT EXISTS "${_wgpu_src}/ffi/webgpu-headers/webgpu.h")
        aurora_error("AURORA_BACKEND_GPU_WGPU=ON but third_party/wgpu-native sources are missing"
                " (expected Cargo.toml, ffi/wgpu.h and ffi/webgpu-headers/webgpu.h; the webgpu-headers"
                " submodule must be populated: git submodule update --init under third_party/wgpu-native).")
    endif ()

    # Rust 工具链探测：cargo 必须存在（rustup 安装）。
    find_program(AURORA_CARGO_EXECUTABLE cargo)
    if (NOT AURORA_CARGO_EXECUTABLE)
        if (WIN32)
            aurora_error("AURORA_BACKEND_GPU_WGPU=ON but 'cargo' was not found on PATH."
                    " Install the Rust toolchain first, e.g.: winget install --id Rustlang.Rustup -e;"
                    " then a toolchain whose host triple matches the C++ ABI - on Windows with MinGW:"
                    " rustup toolchain install stable-x86_64-pc-windows-gnu"
                    " && rustup default stable-x86_64-pc-windows-gnu.")
        else ()
            aurora_error("AURORA_BACKEND_GPU_WGPU=ON but 'cargo' was not found on PATH."
                    " Install the Rust toolchain first, e.g.:"
                    " curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y;"
                    " then make sure ~/.cargo/bin is on PATH.")
        endif ()
    endif ()

    # host 三元组 + 完整工具链 id 探测（在 wgpu-native 目录之外执行，避开上游
    # rust-toolchain.toml 钉版触发的一次性工具链下载）。⚠️ 不能用裸 "stable" 覆盖
    # RUSTUP_TOOLCHAIN——rustup 对通道别名按「通道+宿主启发」解析（可能命中非默认宿主
    # 的损坏工具链）；必须注入 active toolchain 的完整 id（如 stable-x86_64-pc-windows-gnu）。
    find_program(AURORA_RUSTUP_EXECUTABLE rustup)
    if (AURORA_RUSTUP_EXECUTABLE)
        execute_process(COMMAND "${AURORA_RUSTUP_EXECUTABLE}" show active-toolchain
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE _rust_active OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REGEX MATCH "^[^ \t]+" AURORA_RUST_TOOLCHAIN_ID "${_rust_active}")
    endif ()
    # host 三元组探测：用 `rustc -vV` 而非 `cargo rustc -vV`——后者是「编译」子命令，
    # 在无 Cargo.toml 的目录（仓库根）直接报错。同样在 wgpu-native 之外执行，避开钉版文件。
    find_program(AURORA_RUSTC_EXECUTABLE rustc)
    if (NOT AURORA_RUSTC_EXECUTABLE)
        aurora_error("AURORA_BACKEND_GPU_WGPU=ON but 'rustc' was not found on PATH"
                " (rustup's cargo found without rustc is abnormal - try: rustup self update).")
    endif ()
    execute_process(COMMAND "${AURORA_RUSTC_EXECUTABLE}" -vV
            OUTPUT_VARIABLE _rust_vv OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX MATCH "host: ([A-Za-z0-9_.-]+)" _rust_host_match "${_rust_vv}")
    set(AURORA_RUST_HOST_TRIPLE "${CMAKE_MATCH_1}")
    if (NOT AURORA_RUST_HOST_TRIPLE)
        aurora_error("AURORA_BACKEND_GPU_WGPU=ON but the Rust host triple could not be determined"
                " ('cargo rustc -vV' failed - the default toolchain may be corrupted;"
                " repair with: rustup toolchain install stable && rustup default stable).")
    endif ()
    # ABI 守卫：Windows 上 Rust host 与 C++ 编译器必须同族（静态库跨 ABI 不可链接）。
    # （不用 MINGW 预变——其自 CMake 3.25 才定义，本仓库下限 3.20，改判编译器 ID。）
    if (WIN32 AND NOT MSVC AND NOT AURORA_RUST_HOST_TRIPLE MATCHES "-windows-gnu$")
        aurora_error("AURORA_BACKEND_GPU_WGPU: C++ compiler is GNU/MinGW but the default Rust toolchain is"
                " '${AURORA_RUST_HOST_TRIPLE}' (ABI-incompatible static libs). Install & select the gnu host:"
                " rustup toolchain install stable-x86_64-pc-windows-gnu"
                " && rustup default stable-x86_64-pc-windows-gnu.")
    endif ()
    if (MSVC AND NOT AURORA_RUST_HOST_TRIPLE MATCHES "-windows-msvc$")
        aurora_error("AURORA_BACKEND_GPU_WGPU: C++ compiler is MSVC but the default Rust toolchain is"
                " '${AURORA_RUST_HOST_TRIPLE}' (ABI-incompatible static libs). Fix with:"
                " rustup default stable-x86_64-pc-windows-msvc.")
    endif ()
    if (UNIX AND NOT APPLE AND NOT AURORA_RUST_HOST_TRIPLE MATCHES "-linux-(gnu|musl)$")
        aurora_error("AURORA_BACKEND_GPU_WGPU: C++ compiler is GNU (Linux) but the default Rust toolchain is"
                " '${AURORA_RUST_HOST_TRIPLE}' (ABI-incompatible static libs). Expecting a"
                " *-linux-gnu (or explicitly *-linux-musl) host; fix with:"
                " rustup toolchain install stable-<triple> && rustup default stable-<triple>.")
    endif ()

    # libclang 探测：wgpu-native 的 build.rs 经 bindgen 从 webgpu.h 生成 FFI 头，构建期硬
    # 依赖 libclang 共享库。
    # ⚠️ 本文件**不得出现任何本机安装路径**（盘符 / 用户目录一律禁止写死——换机即失效、
    # 且污染他人构建）。定位按「显式传入优先、自动探测兜底」四级：
    #   1) -DAURORA_LIBCLANG_DIR=<目录>：显式旋钮（与 AURORA_LLD_DIR 同口径，推荐）
    #   2) 环境变量 LIBCLANG_PATH：bindgen/LLVM 生态标准变量
    #   3) PATH 上的 clang 可执行旁目录
    #   4) 平台通用默认位（相对量，不含盘符）：Windows 查 LLVM 安装器写入的注册表键与
    #      %ProgramFiles%；类 Unix 查 /usr/lib/llvm-*
    set(AURORA_LIBCLANG_DIR "" CACHE PATH "libclang 所在目录提示（未在 PATH 上时显式指定）")
    set(_wgpu_libclang_dir "")
    set(_wgpu_libclang_explicit FALSE)
    if (AURORA_LIBCLANG_DIR)
        set(_wgpu_libclang_dir "${AURORA_LIBCLANG_DIR}")
        set(_wgpu_libclang_explicit TRUE)
    elseif (DEFINED ENV{LIBCLANG_PATH} AND NOT "$ENV{LIBCLANG_PATH}" STREQUAL "")
        set(_wgpu_libclang_dir "$ENV{LIBCLANG_PATH}")
        set(_wgpu_libclang_explicit TRUE)
    endif ()
    # 显式传入即按契约校验：路径写错时立刻失败，而不是等到 cargo/bindgen 阶段报无头绪的错。
    if (_wgpu_libclang_explicit AND NOT EXISTS "${_wgpu_libclang_dir}")
        aurora_error("AURORA_BACKEND_GPU_WGPU: the libclang directory '${_wgpu_libclang_dir}' does not exist"
                " (passed via -DAURORA_LIBCLANG_DIR or the LIBCLANG_PATH environment variable).")
    endif ()
    if (NOT _wgpu_libclang_dir)
        find_program(AURORA_CLANG_EXECUTABLE clang)
        if (AURORA_CLANG_EXECUTABLE)
            get_filename_component(_wgpu_libclang_dir "${AURORA_CLANG_EXECUTABLE}" DIRECTORY)
        elseif (WIN32)
            # 注册表键由 LLVM 官方安装器写入；ProgramFiles/ProgramW6432 双查覆盖 32 位 CMake
            # 跑在 64 位系统时的变量差异。两者都是相对量，不含盘符。
            foreach (_cand "[HKEY_LOCAL_MACHINE\\SOFTWARE\\LLVM\\LLVM;]"
                    "$ENV{ProgramFiles}/LLVM/bin" "$ENV{ProgramW6432}/LLVM/bin")
                if (_cand AND EXISTS "${_cand}/libclang.dll")
                    set(_wgpu_libclang_dir "${_cand}")
                    break ()
                endif ()
            endforeach ()
        endif ()
    endif ()
    if (NOT _wgpu_libclang_dir AND UNIX)
        # Ubuntu/Debian 的 libclang 实际文件名为 libclang-XX.so(.1)，dev 包装为
        # /usr/lib/llvm-XX/lib/libclang.so 符号链接——多模式 GLOB 后按版本号取最高
        # （字典序会把 llvm-9 排在 llvm-19 前）。
        file(GLOB _wgpu_libclang_cands "/usr/lib/llvm-*/lib/libclang.so*" "/usr/lib/*/libclang*.so*"
                "/usr/lib/libclang*.so*")
        set(_wgpu_libclang_so "")
        set(_wgpu_libclang_ver "0")
        foreach (_cand IN LISTS _wgpu_libclang_cands)
            set(_cand_ver "0")
            if (_cand MATCHES "libclang-([0-9]+(\\.[0-9]+)*)\\.so")
                set(_cand_ver "${CMAKE_MATCH_1}")
            elseif (_cand MATCHES "llvm-([0-9]+)")
                set(_cand_ver "${CMAKE_MATCH_1}")
            endif ()
            if (_wgpu_libclang_so STREQUAL "" OR _cand_ver VERSION_GREATER _wgpu_libclang_ver)
                set(_wgpu_libclang_so "${_cand}")
                set(_wgpu_libclang_ver "${_cand_ver}")
            endif ()
        endforeach ()
        if (_wgpu_libclang_so)
            get_filename_component(_wgpu_libclang_dir "${_wgpu_libclang_so}" DIRECTORY)
        endif ()
    endif ()
    if (NOT _wgpu_libclang_dir)
        aurora_error("AURORA_BACKEND_GPU_WGPU=ON but no libclang shared library was found (required by"
                " wgpu-native's bindgen build script). Pass its directory explicitly:"
                " -DAURORA_LIBCLANG_DIR=<LLVM bin 目录>（或设环境变量 LIBCLANG_PATH）；"
                " 也可把 clang 加入 PATH 由本模块自动探测。")
    endif ()

    # cargo 构建：--target 显式指定 host 三元组，产物路径确定为 target/<triple>/release，
    # 不受「有无 --target」的目录布局差异影响。产物名按平台：gnu/类 Unix 为
    # libwgpu_native.a，MSVC 为 wgpu_native.lib。产物拷贝进 build 目录供 IMPORTED 引用
    # （cargo target 树本身不入库，见 .gitignore）。
    if (MSVC)
        set(_wgpu_staticlib_name "wgpu_native.lib")
    else ()
        set(_wgpu_staticlib_name "libwgpu_native.a")
    endif ()
    set(_wgpu_artifact "${_wgpu_src}/target/${AURORA_RUST_HOST_TRIPLE}/release/${_wgpu_staticlib_name}")
    set(_wgpu_out "${CMAKE_BINARY_DIR}/wgpu-native/${_wgpu_staticlib_name}")
    # 上游冻结源码：configure 期 GLOB 一次即可（本仓库不编辑 .rs 文件）。
    file(GLOB _wgpu_rs_sources "${_wgpu_src}/src/*.rs" "${_wgpu_src}/build.rs")
    # 注入的环境：LIBCLANG_PATH 指向探测到的 libclang 目录；有 rustup 时以 active
    # toolchain 完整 id 覆盖上游 rust-toolchain.toml 的 1.93 一次性钉版（纯 cargo 安装
    # 无 rustup 时该文件本就不生效，不注入）。add_custom_command 无 ENVIRONMENT 参数，
    # 经 cmake -E env 注入。
    set(_wgpu_env "LIBCLANG_PATH=${_wgpu_libclang_dir}")
    if (AURORA_RUST_TOOLCHAIN_ID)
        list(APPEND _wgpu_env "RUSTUP_TOOLCHAIN=${AURORA_RUST_TOOLCHAIN_ID}")
    endif ()
    add_custom_command(OUTPUT "${_wgpu_out}"
            COMMAND "${CMAKE_COMMAND}" -E env ${_wgpu_env}
            "${AURORA_CARGO_EXECUTABLE}" build --release --target "${AURORA_RUST_HOST_TRIPLE}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_wgpu_artifact}" "${_wgpu_out}"
            DEPENDS "${_wgpu_src}/Cargo.toml" "${_wgpu_src}/Cargo.lock"
            "${_wgpu_src}/ffi/wgpu.h" "${_wgpu_src}/ffi/webgpu-headers/webgpu.h"
            ${_wgpu_rs_sources}
            WORKING_DIRECTORY "${_wgpu_src}"
            VERBATIM
            COMMENT "cargo build --release wgpu-native (${AURORA_RUST_HOST_TRIPLE})")
    add_custom_target(wgpu_native_build DEPENDS "${_wgpu_out}")

    add_library(wgpu_native STATIC IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
            IMPORTED_LOCATION "${_wgpu_out}"
            IMPORTED_NO_SONAME TRUE)
    add_dependencies(wgpu_native wgpu_native_build)

    aurora_define_feature(AURORA_BACKEND_GPU_WGPU EXPORT)
    # wgpu.h / webgpu.h 只给库内实现 TU（wgpu_rhi.*）用——公共头 pimpl 隔离，不外泄三方头。
    target_include_directories(aurora PRIVATE "${_wgpu_src}/ffi" "${_wgpu_src}/ffi/webgpu-headers")
    # Rust staticlib 的系统库依赖：windows crate 族引入的 WinAPI 库 + 运行时；
    # Linux 侧 Vulkan/EGL 均运行期 dlopen（ash/khronos-egl dynamic feature），仅需
    # dl/pthread/m（Rust std 的 libm 符号，缺则 pow/fmod 未定义）。
    if (WIN32)
        target_link_libraries(aurora PUBLIC wgpu_native ws2_32 userenv bcrypt advapi32 oleaut32 ntdll)
    elseif (UNIX)
        target_link_libraries(aurora PUBLIC wgpu_native ${CMAKE_DL_LIBS} pthread m)
    else ()
        target_link_libraries(aurora PUBLIC wgpu_native)
    endif ()
    aurora_log("wgpu GPU RHI enabled (wgpu-native source build, host=${AURORA_RUST_HOST_TRIPLE},"
            " libclang=${_wgpu_libclang_dir})")
endif ()

# ---- X11 / Wayland / macOS / WASM ----
# X11/Wayland 用于 Linux 桌面、macOS 用于 Apple、WASM 用于 Emscripten 工具链；
# 默认构建（含本机 Windows/MinGW）不受影响，仍仅 Headless 必开。
option(AURORA_BACKEND_X11 "Enable X11 (Xlib) backend for Linux desktop (requires libX11)" OFF)
option(AURORA_BACKEND_WAYLAND "Enable native Wayland backend for Linux desktop (requires wayland-client + wayland-cursor + xkbcommon)" OFF)
option(AURORA_BACKEND_MACOS "Enable macOS (Cocoa/AppKit) backend (Apple only)" OFF)
option(AURORA_BACKEND_WASM "Enable WebAssembly (Emscripten) backend (Emscripten toolchain only)" OFF)

if (AURORA_BACKEND_X11)
    if (NOT (UNIX AND NOT APPLE))
        aurora_error("AURORA_BACKEND_X11 is only supported on Linux/Unix (non-Apple) platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_X11=OFF.")
    endif ()
    find_package(X11 REQUIRED)
    aurora_define_feature(AURORA_BACKEND_X11 EXPORT)
    target_include_directories(aurora PUBLIC ${X11_INCLUDE_DIR})
    target_link_libraries(aurora PUBLIC ${X11_LIBRARIES})
    aurora_log("X11 backend enabled: X11_LIBRARIES=${X11_LIBRARIES}")
endif ()

if (AURORA_BACKEND_WAYLAND)
    if (NOT (UNIX AND NOT APPLE))
        aurora_error("AURORA_BACKEND_WAYLAND is only supported on Linux/Unix (non-Apple) platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_WAYLAND=OFF.")
    endif ()
    # 依赖：wayland-client（线协议）+ wayland-cursor（客户端主题光标：加载 XCursor 位图）
    # + xkbcommon（键盘 keymap）+ wayland-protocols（xdg-shell XML）
    # + wayland-scanner（协议 XML → C 胶水；生成物落在 build 目录，不入仓）。
    # Wayland 客户端不能像 X11 那样让服务端换光标：形状必须由本进程自绘成 ARGB 位图，
    # 经独立 cursor `wl_surface` 提交后用 `wl_pointer_set_cursor` 交回合成器，故 libwayland-cursor
    # 是**必需**依赖而非可选增强（缺它则 set_cursor 只能停在契约级落盘）。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED wayland-cursor)
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
    # text-input-unstable-v3（客户端输入法：preedit/commit/候选窗定位）来自 wayland-protocols，
    # 但该 XML 是较晚（≥1.24）才进入协议集，且并非所有发行版都打包它。缺失时不硬失败（否则
    # 老协议集的整体 Wayland 构建会红）：跳过生成并置 AURORA_HAVE_WL_TEXT_INPUT=0，
    # 宿主侧 text-input 桥整段降级为 no-op（preedit 仍可由合成器/输入法自带窗体外绘）。
    set(AURORA_WL_TEXT_INPUT_XML "${AURORA_WL_PROTO_DIR}/unstable/text-input/text-input-unstable-v3.xml")
    if (EXISTS "${AURORA_WL_TEXT_INPUT_XML}")
        list(APPEND AURORA_WL_PROTOS "${AURORA_WL_TEXT_INPUT_XML}|text-input-unstable-v3")
        target_compile_definitions(aurora PUBLIC AURORA_HAVE_WL_TEXT_INPUT=1)
        aurora_log("Wayland text-input-unstable-v3: 协议 XML 就绪，客户端输入法桥启用")
    else ()
        target_compile_definitions(aurora PUBLIC AURORA_HAVE_WL_TEXT_INPUT=0)
        aurora_log("Wayland text-input-unstable-v3: 协议 XML 缺失（wayland-protocols 过旧？），输入法桥降级 no-op")
    endif ()
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
    target_include_directories(aurora PRIVATE "${AURORA_WL_GEN_DIR}" ${WAYLAND_CLIENT_INCLUDE_DIRS}
            ${WAYLAND_CURSOR_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
    aurora_define_feature(AURORA_BACKEND_WAYLAND EXPORT)
    target_link_libraries(aurora PUBLIC ${WAYLAND_CLIENT_LIBRARIES} ${WAYLAND_CURSOR_LIBRARIES}
            ${XKBCOMMON_LIBRARIES})
    aurora_log("Wayland backend enabled: protocols=${AURORA_WL_PROTO_DIR} scanner=${AURORA_WL_SCANNER}")
endif ()

if (AURORA_BACKEND_MACOS)
    if (NOT APPLE)
        aurora_error("AURORA_BACKEND_MACOS is only supported on Apple platforms;"
                " cannot enable on the current platform. Disable with -DAURORA_BACKEND_MACOS=OFF.")
    endif ()
    # OBJCXX 语言已在顶层 CMakeLists（add_library(aurora) 之前）统一启用：语言规则须在
    # 目标定义前就绪，否则 Ninja generate 阶段取不到 CMAKE_OBJCXX_* 规则。此处仅平台守卫。
    aurora_define_feature(AURORA_BACKEND_MACOS EXPORT)
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
    aurora_define_feature(AURORA_BACKEND_WASM EXPORT)
    aurora_log("WASM backend enabled (Emscripten).")
endif ()

# ---- 音频（图模型 API 恒编译；内置设备后端 opt-in，默认 OFF） ----
# media/audio.h 的 AudioContext 图 API 始终编译（对齐 RHI 先例：契约恒在，能力运行期查询）；
# AURORA_ENABLE_AUDIO 决定是否编入内置音频设备后端。未启用或设备初始化失败 →
# AudioContext 静默模式（图照常运转、样本消费后丢弃），对齐 GPU 通道回退语义。
option(AURORA_ENABLE_AUDIO "Build built-in audio device backends (graph API always compiled; OFF = silent mode)" OFF)
if (AURORA_ENABLE_AUDIO)
    aurora_define_feature(AURORA_ENABLE_AUDIO EXPORT)
    if (WIN32)
        option(AURORA_ENABLE_AUDIO_WASAPI "Build WASAPI audio backend (Windows shared-mode, event-driven)" ON)
    else ()
        option(AURORA_ENABLE_AUDIO_WASAPI "Build WASAPI audio backend (Windows shared-mode, event-driven)" OFF)
    endif ()
    if (AURORA_ENABLE_AUDIO_WASAPI)
        if (NOT WIN32)
            aurora_error("AURORA_ENABLE_AUDIO_WASAPI is only supported on Windows;"
                    " disable it or turn off AURORA_ENABLE_AUDIO on other platforms.")
        endif ()
        aurora_define_feature(AURORA_ENABLE_AUDIO_WASAPI EXPORT)
        # ole32：COM 初始化（CoInitialize/CoCreateInstance，MMDevice + IAudioClient）。
        target_link_libraries(aurora PUBLIC ole32)
        aurora_log("WASAPI audio backend enabled")
    endif ()
    # Linux 对位后端：ALSA（音频子系统对称设计——Windows WASAPI / Linux ALSA）。
    # 运行时绑定 dlopen("libasound.so.2")：无 <alsa> 头、无 dev 包、不链 libasound，
    # 构建机有无需 ALSA 开发环境即可编译（缺失仅运行期降级 start false → 静默模式）。
    # ${CMAKE_DL_LIBS}：glibc < 2.34 需显式 -ldl（2.34+ 并入 libc 后为空）。
    if (LINUX)
        option(AURORA_ENABLE_AUDIO_ALSA "Build ALSA audio backend (Linux, runtime-bound libasound via dlopen)" ON)
    else ()
        option(AURORA_ENABLE_AUDIO_ALSA "Build ALSA audio backend (Linux, runtime-bound libasound via dlopen)" OFF)
    endif ()
    if (AURORA_ENABLE_AUDIO_ALSA)
        if (NOT LINUX)
            aurora_error("AURORA_ENABLE_AUDIO_ALSA is only supported on Linux;"
                    " disable it or turn off AURORA_ENABLE_AUDIO on other platforms.")
        endif ()
        aurora_define_feature(AURORA_ENABLE_AUDIO_ALSA EXPORT)
        target_link_libraries(aurora PUBLIC ${CMAKE_DL_LIBS})
        aurora_log("ALSA audio backend enabled (runtime-bound libasound, zero build dependency)")
    endif ()
endif ()

# ---- 存储 SQLite 后端（记录仓储第三后端，opt-in，默认 OFF） ----
# 存储层 Memory/Filesystem 两后端恒编译；SqliteBackend（真事务 BEGIN/COMMIT/ROLLBACK +
# 二进制载荷 BLOB 内联存储）由 AURORA_ENABLE_STORAGE_SQLITE 决定是否编入。sqlite3 以
# amalgamation 源码入库（third_party/sqlite/，public domain），独立静态目标编译，
# 不 FetchContent 联网拉取。未启用时公共头不声明该类，消费者构建口径由 EXPORT 宏对齐。
option(AURORA_ENABLE_STORAGE_SQLITE "Build SqliteBackend storage backend (SQLite amalgamation under third_party/sqlite)" OFF)
if (AURORA_ENABLE_STORAGE_SQLITE)
    add_library(aurora_sqlite3 STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
    target_include_directories(aurora_sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
    # SQLITE_THREADSAFE=1：serialized 模式——Storage 异步 API 会从 worker 线程触库
    # （C++ 侧另有互斥守卫事务边界，双保险）。SQLITE_OMIT_LOAD_EXTENSION：封死
    # load_extension 任意代码加载面。
    target_compile_definitions(aurora_sqlite3 PUBLIC SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
    # 三方 amalgamation 源码静音：不受本项目告警配置影响（同 freetype/harfbuzz 纪律）。
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aurora_sqlite3 PRIVATE -w)
    elseif (MSVC)
        target_compile_options(aurora_sqlite3 PRIVATE /w)
    endif ()
    if (NOT WIN32)
        target_link_libraries(aurora_sqlite3 PUBLIC ${CMAKE_DL_LIBS} pthread)
    endif ()
    # 头不带 sqlite3 类型（pimpl），PRIVATE 足够且不外泄三方 include。
    target_link_libraries(aurora PRIVATE aurora_sqlite3)
    aurora_define_feature(AURORA_ENABLE_STORAGE_SQLITE EXPORT)
    aurora_log("SQLite storage backend enabled (third_party/sqlite amalgamation source build)")
endif ()

# ---- 架构级优化开关（性能，独立退化，默认开启） ----
# 三项互不依赖的渲染/布局优化；宏由 aurora 目标以 PUBLIC 编译定义传播给所有消费者。
# 关闭任一开关即回退到原始实现路径（等价无优化）。
option(AURORA_ENABLE_LAYOUT_CACHE "Enable layout constraint cache (skip redundant subtree layout)" ON)
option(AURORA_ENABLE_OCCLUSION_CULLING "Enable occlusion culling (skip children outside clip region)" ON)
option(AURORA_ENABLE_DISPLAY_LIST "Enable display list record/replay (skip unchanged subtree paint)" ON)
foreach (_opt AURORA_ENABLE_LAYOUT_CACHE AURORA_ENABLE_OCCLUSION_CULLING AURORA_ENABLE_DISPLAY_LIST)
    if (${_opt})
        aurora_define_feature(${_opt} EXPORT)
    endif ()
endforeach ()
