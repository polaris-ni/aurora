# ============================================================
# AuroraVerify.cmake — 真机验收探针（tools/verify/，默认 OFF + EXCLUDE_FROM_ALL）
# ------------------------------------------------------------
# 「平台接线」类能力无法在无头 CI 内证明：以 光标形状为例，必须建**真实窗口**、观察
# **屏幕上真实显示的光标**才能验收。这类探针因此**不进 CTest**（会创建真实窗口、读取屏幕
# 光标，非确定且会干扰用户桌面），而是按需定义的可执行目标，由人工触发。
#
# 开启（默认 OFF，不影响任何既有构建/门禁）：
#   cmake -S . -B build-verify -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
#   cmake --build build-verify --target aurora_verify_x11_cursor   # 单个
#   cmake --build build-verify --target aurora_verify              # 本平台全部可用探针
#
# 目标按「当前平台 + 已开启后端」条件定义；无一匹配时连目标都不定义（与 demos 同口径）。
# 全部 EXCLUDE_FROM_ALL：`cmake --build build` 不会连带构建它们。
#
# 各探针的覆盖范围、构建/运行命令、逐项期望与退出码语义，均写在对应源文件头注释内。
# 全部开关/宏/环境变量统一列于 codespec/BUILD_OPTIONS.md（唯一权威来源）。
# ============================================================

option(AURORA_BUILD_VERIFY_TOOLS "Define the real-machine acceptance probes under tools/verify/ (EXCLUDE_FROM_ALL)" OFF)

if (NOT AURORA_BUILD_VERIFY_TOOLS)
    aurora_log("Verify probes disabled (set AURORA_BUILD_VERIFY_TOOLS=ON to define tools/verify/ probes)")
endif ()

if (AURORA_BUILD_VERIFY_TOOLS)
    set(_aurora_verify_dir "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify")

    # 统一样板：只做「链 aurora + C++20 + 项目告警 + MinGW 静态 runtime」。
    # 不 REUSE_FROM 消费者 PCH：探针首行即平台头（Xlib / windows.h / AppKit），且 macOS 探针为
    # ObjC++，复用 C++ 消费者 PCH 无收益且有语言失配风险（见 §2.3）。
    function(aurora_add_verify_probe _name _src)
        add_executable(${_name} EXCLUDE_FROM_ALL ${_src})
        target_link_libraries(${_name} PRIVATE aurora)
        set_target_properties(${_name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        # 额外目标属性按需追加（如 macOS 探针的 OBJCXX_STANDARD）：只对调用方显式传入者生效，
        # 避免对未启用 OBJCXX 语言的目标设置该属性。
        if (ARGN)
            set_target_properties(${_name} PROPERTIES ${ARGN})
        endif ()
        # tools/verify 下的共用头（verify_print.h）由本目录直接引入。
        target_include_directories(${_name} PRIVATE "${_aurora_verify_dir}")
        if (NOT MSVC)
            target_compile_options(${_name} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
        endif ()
        # 与 aurora_add_tool 同口径：MinGW 下静态链接 GCC runtime，使 exe 双击即跑。
        if (MINGW)
            target_link_options(${_name} PRIVATE
                    -static-libgcc -static-libstdc++
                    -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        endif ()
    endfunction()

    set(_aurora_verify_targets "")

    # ---- X11 光标（Linux/BSD 桌面）：XFIXES 读回屏幕上真实显示的光标 ----
    if (AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_cursor "${_aurora_verify_dir}/x11_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_cursor)
    endif ()

    # ---- Win32 家族光标（GDI 上屏 / D3D11 GPU 上屏共用 Win32Window 宿主）：GetCursorInfo 读回 ----
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_cursor "${_aurora_verify_dir}/win32_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_cursor)
    endif ()

    # ---- Win32 UIA 无障碍桥（WM_GETOBJECT / provider 树 / pattern 暴露）----
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_ua "${_aurora_verify_dir}/win32_ua_live_probe.cpp")
        # ObjectFromLresult（取回 provider）与 IID_* 符号来自 oleacc / uuid。
        target_link_libraries(aurora_verify_win32_ua PRIVATE oleacc)
        list(APPEND _aurora_verify_targets aurora_verify_win32_ua)
    endif ()

    # ---- Win32 IMM32 输入法桥（WM_IME_* → 组合事件 / 候选窗定位 / 双通道吞字纪律）----
    # 自动段覆盖「消息泵 + 派发 + 吞字纪律」；组合串注入（ImmSetCompositionStringW）在 TSF 型
    # 输入法（如微软拼音）下被拒，故 preedit/上屏为 best-effort SKIP，其验收交由 --interactive 人工段。
    # imm32 已随 aurora PUBLIC 链接，探针无需额外链接。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_ime "${_aurora_verify_dir}/win32_ime_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_ime)
    endif ()

    # ---- macOS 光标（NSCursor 派发接线）：[NSCursor currentCursor] 单例同一性读回 ----
    # 需 ObjC++ 语言：条件启用，其它平台上完全不涉及（不改动默认构建的语言集合）。
    if (APPLE AND AURORA_BACKEND_MACOS)
        enable_language(OBJCXX)
        aurora_add_verify_probe(aurora_verify_macos_cursor "${_aurora_verify_dir}/macos_cursor_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        # ARC 与手动编译命令同口径；探针内的 ObjC 指针转裸指针已按 ARC/非 ARC 双分支处理，
        # 故即便去掉本行亦可通过编译。
        target_compile_options(aurora_verify_macos_cursor PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_cursor)
    endif ()

    # ---- GLFW 光标（跨平台真实窗口）：GLFW 无光标查询 API → 自动能力核对 + 人工目视（--interactive）----
    if (AURORA_BACKEND_GLFW)
        aurora_add_verify_probe(aurora_verify_glfw_cursor "${_aurora_verify_dir}/glfw_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_glfw_cursor)

    # ---- GLFW GPU 特性（跨平台真实窗口）：常驻流式纹理与 GPU 层缓存真机核对 ----
    # 自动段核对能力位/契约位/流式逐版本像素/层缓存跨帧持久性；--interactive 人工目视。
    # 需 GPU 通道编译进库（AURORA_ENABLE_GLFW_GPU_GL）。
    if (AURORA_ENABLE_GLFW_GPU_GL)
        aurora_add_verify_probe(aurora_verify_glfw_gpu_features "${_aurora_verify_dir}/glfw_gpu_features_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_glfw_gpu_features)
    endif ()
    endif ()

    # ---- Win32 wgpu GPU 栅格（真实窗口 + 离屏直驱）：宿主装配 / 能力契约 / 流式逐版本像素 /
    # 层缓存跨帧持久性 / 平台 present 多帧上屏。需 WGPU 通道编译进库（AURORA_BACKEND_GPU_WGPU）。
    if (WIN32 AND AURORA_BACKEND_GPU_WGPU AND AURORA_BACKEND_WIN32)
        aurora_add_verify_probe(aurora_verify_win32_wgpu "${_aurora_verify_dir}/win32_wgpu_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_wgpu)
    endif ()

    # ---- WASAPI 音频（Windows）：真实设备线程 / 格式协商 / 图时钟 / 出声路径真机核对 ----
    # 自动段核对激活/格式契约/时钟推进/缓冲源与推流通路/suspend-resume；--interactive 出声人工段
    # （扫频/双源混音/设备热切换）。需音频后端编译进库（AURORA_ENABLE_AUDIO=ON）。
    if (WIN32 AND AURORA_ENABLE_AUDIO_WASAPI)
        aurora_add_verify_probe(aurora_verify_wasapi_audio "${_aurora_verify_dir}/wasapi_audio_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_wasapi_audio)
    endif ()
    if (_aurora_verify_targets)
        add_custom_target(aurora_verify DEPENDS ${_aurora_verify_targets})
        aurora_log("Verify probes enabled: ${_aurora_verify_targets} (build all with --target aurora_verify)")
    else ()
        aurora_log("AURORA_BUILD_VERIFY_TOOLS=ON but this platform/config matches no probe; nothing defined")
    endif ()
endif ()
