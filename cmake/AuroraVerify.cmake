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
        # E2E 驱动内核（tools/include/e2e/harness.h）：探针的建窗 / 帧推进 / 像素读回统一经内核。
        target_include_directories(${_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/include")
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

    # ---- 链路态（ConnectivityMonitor）：真实路由 / 接口表上的三档读数 ----
    # 与后端无关：Windows 走 `IpHelper`、Linux 走 `LinkFiles`、其余构建如实回 `None`（退出码 2）。
    # 三段式（拔线 / 恢复）由 `--expect auto|offline|online` 分段驱动，自动段不需要人工动作。
    aurora_add_verify_probe(aurora_verify_net_connectivity "${_aurora_verify_dir}/net_connectivity_live_probe.cpp")
    list(APPEND _aurora_verify_targets aurora_verify_net_connectivity)

    # ---- 桌面外壳集成四枚（Windows / Linux / macOS 的用户级数据库 + 打印会话）----
    # 只验公共 API；判据全部走**独立取证**（PDF 交叉引用与 zlib 流自行解析、注册表 / mimeapps.list /
    # autostart 目录直接读），不复用库内折算结果自证。能力位为 false 的构建按退出码 2 报环境不可用。
    # 写入一律落在用户级（HKCU / XDG 用户目录），结束前把基线逐字节还原；不触碰 HKLM、不抢默认处理程序。
    if (WIN32 OR LINUX OR APPLE)
        aurora_add_verify_probe(aurora_verify_document_output
                "${_aurora_verify_dir}/document_output_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_document_output)

        aurora_add_verify_probe(aurora_verify_url_scheme "${_aurora_verify_dir}/url_scheme_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_url_scheme)

        aurora_add_verify_probe(aurora_verify_file_association
                "${_aurora_verify_dir}/file_association_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_file_association)

        aurora_add_verify_probe(aurora_verify_run_at_login "${_aurora_verify_dir}/run_at_login_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_run_at_login)
    endif ()

    # ---- X11 光标（Linux/BSD 桌面）：XFIXES 读回屏幕上真实显示的光标 ----
    if (AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_cursor "${_aurora_verify_dir}/x11_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_cursor)
    endif ()

    # ---- X11 缩放上报 + 窗口几何（真实 X server）：运行期改根窗口 Xft.dpi → scale_factor
    # 跟随、on_scale_changed 触发重绘；set_size / set_position → 独立 Xlib 观测窗口真实改变。
    # 自动段全自动（无需人工段），故不进 CTest：建真实窗口且会改动会话级 RESOURCE_MANAGER。
    if (AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_geometry "${_aurora_verify_dir}/x11_geometry_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_geometry)
    endif ()

    # ---- X11 Xdnd 拖入 / 拖出桥（真实 X 服务器）：`XdndAware` 广告 + 四态派发 + 动作协商 +
    # selection round-trip 取数 + 拖出 grab/`XdndSelection` 所有权/消息序列。自动段以「本窗口
    # 自拖放」完成全链路无人值守验收（要约 → 数据往返 → 终态 Succeeded/Copy），跨进程会话
    # （从文件管理器拖文件）交 --interactive 人工段。
    if (AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_drag "${_aurora_verify_dir}/x11_drag_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_drag)
    endif ()

    # ---- X11 XIM 输入法桥（风格协商 / PreeditCallbacks 回调 / IC 焦点宣告 / 候选窗锚点）----
    # 自动段验收协商与焦点接线（不要求本机装输入法；无 XIM 服务器按合法降级 SKIP）；
    # preedit/上屏内容段交 --interactive 人工（需真实 XIM 进程驱动组合）。
    if (AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_ime "${_aurora_verify_dir}/x11_ime_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_ime)
    endif ()

    # ---- Linux XDG 跨平台系统通知（libnotify.so.4 → libdbus-1.so.3 → notify-send 三级运行时降级）----
    # 自动段验收 API 面（last_notification 往返 / 三档紧急度 / 去重 / recording 注入 / 激活回调注入 /
    # pump_events 安全）；「屏幕真的出现气泡」与「点击触发回调」靠 --interactive 人工段。本探针仅
    # 在桌面 Linux 可构建（含 #error 守卫），WSLg/Weston 下经 XDG 桌面通知服务可验。
    if (AURORA_BACKEND_X11 OR AURORA_BACKEND_WAYLAND)
        aurora_add_verify_probe(aurora_verify_linux_notification "${_aurora_verify_dir}/linux_notification_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_linux_notification)
    endif ()

    # ---- Win32 家族光标（GDI 上屏 / D3D11 GPU 上屏共用 Win32Host 宿主）：GetCursorInfo 读回 ----
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

    # ---- Win32 OLE 拖入桥（RegisterDragDrop + IDropTarget 四态 / accept 回写 / 拖入入口）----
    # 自动段验收「宿主确实注册了 OLE 拖入目标」（重复注册返回 DRAGDROP_E_ALREADYREGISTERED）、
    # 能力位已翻、以及合成事件经真派发器得到 allowed 求交；跨进程会话本身（从资源管理器拖文件）
    # 只能由 --interactive 人工段核对，无头与无人工操作均无法产生 OS 会话。ole32 已随 aurora
    # PUBLIC 链接，探针无需额外链接。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_drag "${_aurora_verify_dir}/win32_drag_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_drag)
    endif ()

    # ---- Win32 OS 级最近文档投影（`SHAddToRecentDocs` + Recent 目录撤销）----
    # 自动段以**独立取证**（探针自带 Recent 目录枚举 + `IShellLinkW` 目标解析）证明「投影后 OS 里
    # 真的出现条目、撤销后消失、重复投影不多投」，并核对桥的进程级投影记账与 store 不受投影影响；
    # 探针不建窗口，顺带覆盖无宿主进程的通路自举。用户关掉「最近使用的项目」（`Start_TrackDocs=0`）
    # 时 API 静默无操作，自动段如实以退出码 2 报环境不可用，不改判 PASS。
    # 记账观测口 `win32_recent_documents_projected_count()` 是库内部契约（src/aurora/window/detail/），
    # 故需 src 包含目录（口径同 csd/WASAPI 探针）；shell32 / ole32 已随 aurora PUBLIC 链接。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_recent_documents
                "${_aurora_verify_dir}/win32_recent_documents_live_probe.cpp")
        target_include_directories(aurora_verify_win32_recent_documents PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_win32_recent_documents)
    endif ()

    # ---- Win32 原生菜单栏（真实隐藏窗口 + CreateMenu/SetMenu 建树 + WM_COMMAND 回投）----
    # 自动段以**独立取证**（探针直接读 GetMenu/GetMenuItemCount/GetSubMenu 核对原生菜单层级，不读桥私有
    # 记账）证明「install 真在窗口上建出 HMENU、层级条数对齐」；逐终端项调桥的 on_command（消息泵正是
    # 转发到这里）验证「稳定 id 正确回到路由回调、空 id 不回投」；并覆盖重复 install 替换旧树、uninstall
    # 摘下菜单。无头 CI 走 utest_menu_bar_translate.cpp 的假后端，本探针补真实 Win32 那条接缝。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_menu_bar
                "${_aurora_verify_dir}/win32_menu_bar_live_probe.cpp")
        target_include_directories(aurora_verify_win32_menu_bar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_win32_menu_bar)
    endif ()

    # ---- Win32 剪贴板富格式与惰性提供（CF_HDROP / CF_HTML / 自定义 MIME / WM_RENDERFORMAT）----
    # 自动段以**独立取证**（原生 OpenClipboard / GetClipboardData / DragQueryFileW /
    # EnumClipboardFormats）证明「库写的块系统真收得下」，并覆盖三件以 owner 窗口为投递目标的
    # 能力：惰性提供（注册后 provide 零次、原生请求触发一次）、变更监听（泵不到记 SKIP 而非 FAIL）、
    # 程序化清空。探针**自建窗口**——无 owner 窗口时这三件在平台层就不成立，建窗失败即环境不可用。
    # shell32（DragQueryFileW）已随 aurora PUBLIC 链接。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_clipboard "${_aurora_verify_dir}/win32_clipboard_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_clipboard)
    endif ()

    # ---- Win32 文件对话框投递（真实 COM 对话框对象上的 `SetFileName` / `SetDefaultExtension` /
    # `SetFolder`）：自动段以**独立取证**（探针直接读 OS 自己的 `GetFileName` / 日志与 `GetFolder`
    # 回读值，不读库内折算结果自证）证明「`Options` 的建议名、缺省扩展名与初始目录确实被 shell
    # 记下」——含正斜杠归一（解析名 API 不接受 `C:/...`）、前导点削除（OS 侧无扩展名 getter，故该格
    # 由确认后的 `GetResult` 路径末段机判，否则落成 `name..txt` 就是没削点）、非 ASCII 名逐字节往返、
    # 不可解析目录走降级并留一条可归因 WARN。自动段不调 `Show()`（不弹框、不等用户），故不进 CTest；
    # `屏幕上看得见预填的那一行` 交 --interactive 人工段。
    # 投递函数是库内部接缝（src/aurora/app/detail/，口径同 csd/WASAPI/最近文档探针），故需 src 包含目录；
    # shell32 / ole32 已随 aurora PUBLIC 链接。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_file_dialog
                "${_aurora_verify_dir}/win32_file_dialog_live_probe.cpp")
        target_include_directories(aurora_verify_win32_file_dialog PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_win32_file_dialog)
    endif ()

    # ---- Win32 OS 级全局热键（真实窗口 + 隐藏消息窗口接 WM_HOTKEY；「应用无焦点也触发」+ 排空语义）----
    # 自动段验收注册表 API 面（enabled / add / 判重 / 无效主键 / remove / clear / drain / 注入降级）；
    # 「OS 真把按键投到本进程」靠 --interactive 人工段（把焦点切到别的进程再按）。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_os_hotkey "${_aurora_verify_dir}/win32_os_hotkey_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_os_hotkey)
    endif ()

    # ---- Win32 WM_SYSKEY* 派发链（Alt 组合与常规键同路，消费即止 / 未消费回落 DefWindowProc）----
    # 自动段向真实宿主窗口同步注入 WM_SYSKEYDOWN/UP，核对 (i) Alt 组合进派发链且修饰位带 Alt、
    # (ii) Alt 自身与 F10 只推进态不派发、(iii) 消费与非消费两种返回值分岔；Alt+F4 的
    # 「消费则不关窗」是否定性断言，只能目视，故留 --interactive 人工段。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_syskey "${_aurora_verify_dir}/win32_syskey_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_syskey)
    endif ()

    # ---- Win32 DPI 缩放单点真值源（scale 成员 + to_physical / to_logical 唯二换算点）----
    # 自动段覆盖 set_size 往返、WM_SIZE 后帧/逻辑/scale 三方同源、鼠标 dp 映射、
    # WM_GETMINMAXINFO 与 set_size 同换算。本机 100% DPI 时五条判据恒真，如实以退出码 3 记 SKIP。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_dpi "${_aurora_verify_dir}/win32_dpi_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_dpi)
    endif ()

    # ---- 主键盘 Insert 键码（`KeyCode::Insert`）----
    # 自动段只做通道覆盖（注入的 VK_INSERT 是否译成 KeyCode::Insert），真验收在人工段：
    # 真前台按一次物理 Insert，断言 Down/Up 成对且总数为二。锁屏 / 未按键记 PENDING MANUAL（退出码 3），
    # 不记 PASS —— 用注入冒充真按键会让本条判据恒真。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_insert_key "${_aurora_verify_dir}/win32_insert_key_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_insert_key)
    endif ()

    # ---- Win32 程序化窗控四槽（GDI / D3D11 共用 Win32Host 宿主）：minimize / toggle_maximize /
    # set_window_mode / set_fullscreen 公共入口真接线 ----
    # 自动段以 OS 实值独立取证（IsIconic / IsZoomed / GetWindowRect，不读库内状态自证）：
    # 四请求真的落到窗口、应用级快照与 set_on_window_mode 回调同链上报、目标态连调不翻转、
    # 全屏档为「可查询的不支持」（矩形不动 + 恰一条 WARN + 能力位 false）。三档装饰
    # （Default / Borderless / Frameless）逐条重跑，覆盖应用自绘标题栏的缺口暴露档。
    # 无头 CI 建不了真窗口，故不进 CTest；任务栏动画与 Borderless 观感交 --interactive 人工段。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11))
        aurora_add_verify_probe(aurora_verify_win32_window_mode
                "${_aurora_verify_dir}/win32_window_mode_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_win32_window_mode)
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

        # ---- macOS 系统拖放（NSDraggingDestination / NSDraggingSource 双向接线）----
        # 自动段以「探针自带的 NSDraggingInfo」替身驱动桥（窗口服务器无法搬进进程，与 Wayland
        # 把合成器搬进进程同思路、不同落点）：真实 NSPasteboard 往返、库自带目标视图的五方法转发、
        # 折算层与真派发链全走 shipping 代码；AppKit 真实派发的会话与拖出终态留给人工段，
        # 无窗口服务器时记 PENDING（退出码 3），不冒充已验收。
        aurora_add_verify_probe(aurora_verify_macos_drag "${_aurora_verify_dir}/macos_drag_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        # 探针直连库内部头（`window/detail/macos_drag.h` 的桥面与 `system_drag_translate.h`
        # 的折算层不在公共 API，验收的是内部接线本身；口径同 csd/WASAPI/ALSA/Wayland 拖放探针）。
        target_include_directories(aurora_verify_macos_drag PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_drag PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_drag)

        # ---- macOS Surface 窗口/上屏/几何/显示/明暗偏好（真实 AppKit 窗口）----
        # 自动段以公共 API 建真实窗口，逐项核对：帧缓冲尺寸 = 逻辑 x backingScaleFactor、
        # Painter 像素落点与清屏底色、frame_count、set_size / set_position 往返、display_id
        # 与 app::list_displays 的同一 id 空间、os_color_scheme 对 NSApp 外观切换的实时跟随、
        # close / cancel_close 语义。不依赖截图（背景图形会话下 screencapture 不可用）。
        aurora_add_verify_probe(aurora_verify_macos_surface "${_aurora_verify_dir}/macos_surface_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_compile_options(aurora_verify_macos_surface PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_surface)

        # ---- macOS 最近文档 OS 投影（NSDocumentController + 独立取证）----
        # 探针不建窗口（顺带覆盖无宿主进程的自举通路），用 `recentDocumentURLs` 独立核对
        # 投影条目出现 / 重复投影去重 / 撤销后清空，并核对能力位与应用内 store 不受投影影响。
        # 记账观测口 `macos_recent_documents_projected_count()` 是库内部契约（src/aurora/window/detail/），
        # 故需 src 包含目录（口径同 Win32 最近文档 / csd / WASAPI 探针）。
        aurora_add_verify_probe(aurora_verify_macos_recent_documents
                "${_aurora_verify_dir}/macos_recent_documents_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_include_directories(aurora_verify_macos_recent_documents PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_recent_documents PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_recent_documents)
        # NSDocumentController 只对已签名的 .app 包记录最近文档（裸可执行文件调用成功但集合恒空，
        # 本机实测 macOS 26）。故构建后封装成 .app 并 ad-hoc 签名；探针须经包内可执行文件运行。
        find_program(AURORA_CODESIGN_EXECUTABLE codesign)
        if (AURORA_CODESIGN_EXECUTABLE)
            set(_aurora_recent_probe_app "${CMAKE_CURRENT_BINARY_DIR}/AuroraMacOSRecentProbe.app")
            add_custom_command(TARGET aurora_verify_macos_recent_documents POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aurora_recent_probe_app}/Contents/MacOS"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "$<TARGET_FILE:aurora_verify_macos_recent_documents>"
                            "${_aurora_recent_probe_app}/Contents/MacOS/aurora_verify_macos_recent_documents"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "${_aurora_verify_dir}/macos_recent_documents_probe_info.plist"
                            "${_aurora_recent_probe_app}/Contents/Info.plist"
                    COMMAND "${AURORA_CODESIGN_EXECUTABLE}" --force --deep --sign - "${_aurora_recent_probe_app}"
                    VERBATIM)
            aurora_log("macOS recent-documents probe bundle: ${_aurora_recent_probe_app}")
        else ()
            aurora_log("codesign not found: macOS recent-documents probe will not run from a bundle")
        endif ()

        # ---- macOS 通知中心（NSUserNotificationCenter + 系统条目独立取证）----
        # 逐项核对投递条目、同 tag 原位替换、紧急度声音映射、空 tag 占位标识、delegate 激活接线
        # 与 pump 安全；真实鼠标点击留给人工段（打印 PENDING 提示，不冒充已验收）。
        # 通知中心同样只对 bundle 应用可得（裸二进制单例为 nil），故同样封装 + ad-hoc 签名；
        # 裸跑时探针会核对「结构化失败而非谎报成功」并以退出码 3 报 PENDING。
        aurora_add_verify_probe(aurora_verify_macos_notification
                "${_aurora_verify_dir}/macos_notification_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_compile_options(aurora_verify_macos_notification PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_notification)
        if (AURORA_CODESIGN_EXECUTABLE)
            set(_aurora_notification_probe_app "${CMAKE_CURRENT_BINARY_DIR}/AuroraMacOSNotificationProbe.app")
            add_custom_command(TARGET aurora_verify_macos_notification POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aurora_notification_probe_app}/Contents/MacOS"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "$<TARGET_FILE:aurora_verify_macos_notification>"
                            "${_aurora_notification_probe_app}/Contents/MacOS/aurora_verify_macos_notification"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "${_aurora_verify_dir}/macos_notification_probe_info.plist"
                            "${_aurora_notification_probe_app}/Contents/Info.plist"
                    COMMAND "${AURORA_CODESIGN_EXECUTABLE}" --force --deep --sign - "${_aurora_notification_probe_app}"
                    VERBATIM)
            aurora_log("macOS notification probe bundle: ${_aurora_notification_probe_app}")
        else ()
            aurora_log("codesign not found: macOS notification probe will not run from a bundle")
        endif ()

        # ---- macOS 全局热键（Carbon RegisterEventHotKey + 合成事件）----
        # 自动段真实注册 Ctrl+Alt+K 等组合，再向应用事件目标合成 kEventHotKeyPressed，
        # 逐项核对「系统注册 -> 处理器 -> 队列 -> drain 执行」接线；真实键盘按键留给人工段。
        # 探针直连库内部签名常量（src/aurora/app/detail/macos_hotkey.h），故需 src 包含目录。
        aurora_add_verify_probe(aurora_verify_macos_hotkey "${_aurora_verify_dir}/macos_hotkey_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_include_directories(aurora_verify_macos_hotkey PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_hotkey PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_hotkey)

        # ---- macOS 原生菜单栏（NSMenu 建树 + target-action 回投）----
        # 自动段以公共门面安装真实 NSApp.mainMenu，逐项核对标题 / 分隔符 / 勾选 / 禁用 / 子菜单，
        # 并以 `performActionForItemAtIndex:` 驱动真实 target-action 链验证 id 路由；真实鼠标点击
        # 留给人工段。探针直连内部观测口 `macos_menu_bar_is_installed()`（src 包含目录）。
        aurora_add_verify_probe(aurora_verify_macos_menu_bar
                "${_aurora_verify_dir}/macos_menu_bar_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_include_directories(aurora_verify_macos_menu_bar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_menu_bar PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_menu_bar)

        # ---- macOS 剪贴板富格式（NSPasteboard + 惰性 data provider）----
        # 自动段走公共门面：原始载荷 / HTML / 文件清单往返、pbcopy 互操作、惰性提供触发与
        # 所有权丢失回调；跨应用读取留给人工段。探针直连内部观测口 `macos_clipboard_has_offer()`。
        aurora_add_verify_probe(aurora_verify_macos_clipboard
                "${_aurora_verify_dir}/macos_clipboard_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_include_directories(aurora_verify_macos_clipboard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_clipboard PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_clipboard)

        # ---- macOS 状态栏托盘（NSStatusItem + NSMenu 模型映射）----
        # 自动段核对状态项挂载 / 可见性 / tooltip / 图标 / 菜单映射与叶子项路由（合成
        # target-action）、移动构造与析构收尾；真实鼠标点击菜单栏留给人工段。
        # 观测面 `live_tray_bridges()` 与桥类在 src/aurora/app/detail/macos_tray.h，故需 src 包含目录。
        # 气泡走通知中心，同样只对 bundle 应用可得：封装 .app 并 ad-hoc 签名，裸跑报 PENDING（退出码 3）。
        aurora_add_verify_probe(aurora_verify_macos_tray "${_aurora_verify_dir}/macos_tray_live_probe.mm"
                OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_include_directories(aurora_verify_macos_tray PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_options(aurora_verify_macos_tray PRIVATE -fobjc-arc)
        list(APPEND _aurora_verify_targets aurora_verify_macos_tray)
        if (AURORA_CODESIGN_EXECUTABLE)
            set(_aurora_tray_probe_app "${CMAKE_CURRENT_BINARY_DIR}/AuroraMacOSTrayProbe.app")
            add_custom_command(TARGET aurora_verify_macos_tray POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aurora_tray_probe_app}/Contents/MacOS"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "$<TARGET_FILE:aurora_verify_macos_tray>"
                            "${_aurora_tray_probe_app}/Contents/MacOS/aurora_verify_macos_tray"
                    COMMAND "${CMAKE_COMMAND}" -E copy
                            "${_aurora_verify_dir}/macos_tray_probe_info.plist"
                            "${_aurora_tray_probe_app}/Contents/Info.plist"
                    COMMAND "${AURORA_CODESIGN_EXECUTABLE}" --force --deep --sign - "${_aurora_tray_probe_app}"
                    VERBATIM)
            aurora_log("macOS tray probe bundle: ${_aurora_tray_probe_app}")
        else ()
            aurora_log("codesign not found: macOS tray probe will not run from a bundle")
        endif ()
    endif ()

    # ---- GLFW 光标（跨平台真实窗口）：GLFW 无光标查询 API → 自动能力核对 + 人工目视（--interactive）----
    if (AURORA_BACKEND_GLFW)
        aurora_add_verify_probe(aurora_verify_glfw_cursor "${_aurora_verify_dir}/glfw_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_glfw_cursor)

    # ---- GLFW DPI 缩放上报（跨平台真实窗口）：接线/换算自动核对 + 跨屏拖动人工验收 ----
    # 自动段验「回调通道接通 + frame/逻辑/scale 三者同源」（任何 DPI 环境均可证伪）；
    # 跨屏拖动触发 content scale 变更只能由人操作，故人工段未做记 PENDING MANUAL（退出码 3）。
    aurora_add_verify_probe(aurora_verify_glfw_dpi "${_aurora_verify_dir}/glfw_dpi_live_probe.cpp")
    list(APPEND _aurora_verify_targets aurora_verify_glfw_dpi)

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

    # ---- X11 客户端自绘装饰（真实 X server）：ClientSide 撤 WM 口沿后标题栏带真绘出 +
    # content_inset 上报 + Auto/Frameless 零入侵口径。像素判据需 DEBUG 构建（data() 暴露帧缓冲）。
    if (AURORA_BACKEND_X11 AND AURORA_ENABLE_DEBUG)
        aurora_add_verify_probe(aurora_verify_x11_csd "${_aurora_verify_dir}/x11_csd_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_csd)
    endif ()

    # ---- Win32 客户端自绘装饰（真实桌面）：WM_NCCALCSIZE 吞框几何 + WM_NCHITTEST 逐点热区
    # 回归（SendMessage 直报，不依赖抓屏）+ DEBUG data() 像素判据（AdwaitaDark 栏带/hover/press
    # 分层/零入侵）+ 释放触发与拖离取消 + 降级归零 + 热更 + 最大化触发与 rcWork 几何。
    # 像素判据需 DEBUG 构建（data() 暴露帧缓冲）；期望值取库内共享层同一份函数
    # （src/aurora/window/detail/csd_*.h），故需 src 包含目录（同 WASAPI/ALSA 探针先例）。
    if (WIN32 AND AURORA_BACKEND_WIN32 AND AURORA_ENABLE_DEBUG)
        aurora_add_verify_probe(aurora_verify_win32_csd "${_aurora_verify_dir}/win32_csd_live_probe.cpp")
        target_include_directories(aurora_verify_win32_csd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_win32_csd)
    endif ()

    # ---- X11 wgpu GPU 栅格（Linux 真实窗口 + 离屏直驱，WSLg vGPU 可验）：与 Win32 探针同段结构，
    # 宿主动态类型为 WgpuX11Surface，另加 XGetImage 截图落盘物证。需 WGPU+X11 通道编译进库。
    if (AURORA_BACKEND_GPU_WGPU AND AURORA_BACKEND_X11)
        aurora_add_verify_probe(aurora_verify_x11_wgpu "${_aurora_verify_dir}/x11_wgpu_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_x11_wgpu)
    endif ()

    # ---- Wayland 光标（Linux/Wayland 真实窗口）：客户端主题光标提交物证 + 人工目视 ----
    # Wayland 无「屏幕当前光标」读回 API（不同于 Win32 GetCursorInfo / X11 XFIXES），故自动段判据
    # 落在「本端向合成器提交了什么」（主题命中名/位图尺寸/热点/buffer 身份/提交次数）。
    if (AURORA_BACKEND_WAYLAND)
        aurora_add_verify_probe(aurora_verify_wayland_cursor "${_aurora_verify_dir}/wayland_cursor_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_wayland_cursor)
    endif ()

    # ---- Wayland 窗口尺寸提议（真实合成器）：set_size 经 xdg_toplevel min/max 提议，
    # configure 落地后 size() 才变；set_position 如实 no-op。自动段全自动，故不进 CTest
    # （建真实窗口，且尺寸提议会改变用户桌面上的窗口几何）。
    if (AURORA_BACKEND_WAYLAND)
        aurora_add_verify_probe(aurora_verify_wayland_geometry "${_aurora_verify_dir}/wayland_geometry_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_wayland_geometry)
    endif ()

    # ---- Wayland text-input-unstable-v3 输入法桥（manager 绑定 / enter-enable 判据 / 组合事件）----
    # 合成器未发布 v3（WSLg Weston 常态）时自动段验收「优雅降级」；manager 在场时逐条断言
    # enable/disable/去重。preedit/上屏内容段交 --interactive（需合成器侧输入法进程）。
    # 依赖构建期代码生成门 AURORA_HAVE_WL_TEXT_INPUT（wayland-protocols 的 v3 XML）。
    if (AURORA_BACKEND_WAYLAND)
        aurora_add_verify_probe(aurora_verify_wayland_ime "${_aurora_verify_dir}/wayland_ime_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_wayland_ime)
    endif ()

    # ---- Wayland wl_data_device 系统拖放桥（拖入四态 + 拖出终态）----
    # 自动段跑在**进程内 mock 合成器**上（服务端自建 wl_display + wl_compositor / wl_seat /
    # wl_data_device_manager 三段，客户端经 socketpair 连上，走真实 wl_data_device 协议报文）。
    # 为何 mock：Wayland 没有客户端指针传送 API，「把指针泊进本窗口」无人值守做不到（X11 侧
    # 同一约束的表现见 x11_drag_live_probe）。冒烟腿（有 WAYLAND_DISPLAY 时建真实
    # WaylandSurface 泵帧）与 --interactive 人工段补「真实合成器上装得上」的覆盖。
    # 依赖 wayland-server 开发包（aurora 只链 wayland-client）：命中才定义目标。
    if (AURORA_BACKEND_WAYLAND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(AURORA_WAYLAND_SERVER QUIET wayland-server)
        if (AURORA_WAYLAND_SERVER_FOUND)
            aurora_add_verify_probe(aurora_verify_wayland_drag "${_aurora_verify_dir}/wayland_drag_live_probe.cpp")
            # 探针直连库内部头（`window/detail/wayland_drag.h` 的桥面与 `system_drag_translate.h`
            # 的折算层不在公共 API，验收的是内部接线本身；口径同 csd/WASAPI/ALSA 探针）。
            target_include_directories(aurora_verify_wayland_drag PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
            target_include_directories(aurora_verify_wayland_drag PRIVATE
                    ${AURORA_WAYLAND_SERVER_INCLUDE_DIRS})
            target_link_libraries(aurora_verify_wayland_drag PRIVATE ${AURORA_WAYLAND_SERVER_LIBRARIES})
            list(APPEND _aurora_verify_targets aurora_verify_wayland_drag)
        else ()
            aurora_log("aurora_verify_wayland_drag skipped: wayland-server not found (pkg-config)")
        endif ()
    endif ()

    # ---- Wayland wgpu GPU 栅格（Linux 真实窗口 + 离屏直驱，WSLg Weston 可验）：与 X11 探针同段
    # 结构，宿主动态类型为 WgpuWaylandSurface（WaylandOptions+GpuWgpu 直达）；Wayland 无抓屏
    # 原语故无截图项，装饰归属由 --interactive 人工目视核对。需 WGPU+WAYLAND 通道编译进库。
    if (AURORA_BACKEND_GPU_WGPU AND AURORA_BACKEND_WAYLAND)
        aurora_add_verify_probe(aurora_verify_wayland_wgpu "${_aurora_verify_dir}/wayland_wgpu_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_wayland_wgpu)
    endif ()

    # ---- AT-SPI2 无障碍桥（Linux 桌面）：真实 org.a11y.Bus 握手 / 桌面树可见性 /
    # Cache.GetItems 行格式 / 角色·状态·几何·文本·动作方法面 对上游 libatspi 客户端证明。
    # 客户端为 python3-gi Atspi 子进程（Accerciser 同栈），父进程帧循环泵桥应答；
    # DoAction 经桥路由回 Button::on_click 完成跨半程闭环。仅用公共 API，无需 src 私链。
    if (LINUX AND (AURORA_BACKEND_X11 OR AURORA_BACKEND_WAYLAND))
        aurora_add_verify_probe(aurora_verify_atspi "${_aurora_verify_dir}/atspi_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_atspi)
    endif ()

    # ---- WebKitGTK WebView（Linux/X11）：引擎 + 宿主接线的端到端真机验收 ——
    # 收养外部 X 窗口 / 页面加载与导航态 / 标题 / evaluate 往返 / 双向消息通道 /
    # X 子窗口几何与预期矩形闭环比对 / 改档后几何再下发 / 快照像素。强制 X11 路径
    # （Wayland 下收养机制不成立，引擎按设计报 UnsupportedPlatform）。仅用公共 API。
    if (LINUX AND AURORA_BACKEND_X11 AND AURORA_ENABLE_WEBVIEW_WEBKITGTK)
        aurora_add_verify_probe(aurora_verify_webview_webkitgtk
                "${_aurora_verify_dir}/webkitgtk_webview_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_webview_webkitgtk)
    endif ()

    # ---- WebView2 WebView（Windows）：引擎 + 宿主接线的端到端真机验收 ——
    # 异步环境 / 控制器创建 / 页面加载与导航态 / 标题 / evaluate 往返 / 双向消息通道 /
    # Win32 子窗口几何与可见性闭环比对 / 改档后几何再下发 / 快照像素。仅用公共 API。
    # 运行期需 WebView2 Evergreen 运行时与可被 LoadLibrary 找到的 WebView2Loader.dll
    # （应用侧可再分发件，运行前放到 exe 同目录或加进 PATH），见探针头注。
    if (WIN32 AND (AURORA_BACKEND_WIN32 OR AURORA_BACKEND_D3D11) AND AURORA_ENABLE_WEBVIEW_WEBVIEW2)
        aurora_add_verify_probe(aurora_verify_webview_webview2
                "${_aurora_verify_dir}/webview2_webview_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_webview_webview2)
    endif ()

    # ---- WASAPI 音频（Windows）：真实设备线程 / 格式协商 / 图时钟 / 出声路径真机核对 ----
    # 自动段核对激活/格式契约/时钟推进/缓冲源与推流通路/suspend-resume；--interactive 出声人工段
    # （扫频/双源混音/设备热切换）。需音频后端编译进库（AURORA_ENABLE_AUDIO=ON）。
    if (WIN32 AND AURORA_ENABLE_AUDIO_WASAPI)
        aurora_add_verify_probe(aurora_verify_wasapi_audio "${_aurora_verify_dir}/wasapi_audio_live_probe.cpp")
        # 守卫审计观察口直连：采集后端 `failed()` 是库内部契约（src/aurora/media/audio_wasapi.h），
        # 公共 API 不暴露，仅本探针直连验收其生命周期行为。
        target_include_directories(aurora_verify_wasapi_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_wasapi_audio)
    endif ()

    # ---- ALSA 音频（Linux）：libasound 运行时绑定 / 真实设备线程 / 格式协商 / 图时钟 /
    # 出声路径真机核对。段结构与 WASAPI 探针对齐（激活/格式契约/时钟/缓冲源与推流/
    # suspend-resume/采集观察口 + --interactive 出声人工段）。无 libasound 或无输出设备
    # 的机器（如未装音频栈的 WSL）自动段以退出码 2 报 ENV-UNAVAILABLE，属合法降级。
    if (LINUX AND AURORA_ENABLE_AUDIO_ALSA)
        aurora_add_verify_probe(aurora_verify_alsa_audio "${_aurora_verify_dir}/alsa_audio_live_probe.cpp")
        # 采集后端 `failed()` 是库内部契约（src/aurora/media/audio_alsa.h），
        # 公共 API 不暴露，仅本探针直连验收其生命周期行为（同 WASAPI 探针口径）。
        target_include_directories(aurora_verify_alsa_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_alsa_audio)
    endif ()

    # ---- CoreAudio 音频（macOS）：AudioQueue 建队列 / 真实队列线程 / 格式契约 / 图时钟 /
    # 出声路径真机核对。段结构与 WASAPI / ALSA 探针对齐（激活/格式契约/时钟/缓冲源与推流/
    # suspend-resume/重路由收尾/采集观察口 + --interactive 出声与设备热切换人工段）。
    # 采集段落「无 bundle → 未声明 NSMicrophoneUsageDescription → 拒启」这一腿（裸可执行
    # 文件据此永不被 TCC 终止），真采集须在声明该键的 .app 壳内跑。无任何输出设备的机器
    # （如本仓开发用的 VMware Fusion 虚拟机，CoreAudio device_count==0）自动段以退出码 2
    # 报 ENV-UNAVAILABLE，属合法降级——与 ALSA 探针在无 ALSA 栈的 WSL 上同形态。
    if (APPLE AND AURORA_ENABLE_AUDIO_COREAUDIO)
        aurora_add_verify_probe(aurora_verify_coreaudio_audio "${_aurora_verify_dir}/coreaudio_audio_live_probe.cpp")
        # 采集后端 `failed()` 与设备后端的重路由收尾都是库内部契约
        # （src/aurora/media/audio_coreaudio.h），公共 API 不暴露，仅本探针直连验收。
        target_include_directories(aurora_verify_coreaudio_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        list(APPEND _aurora_verify_targets aurora_verify_coreaudio_audio)
    endif ()
    # ---- TLS 通道（任意有出站通路的路）：真实公网证书链 / SNI / ALPN 回选 / 记录级收发 / SPKI pin ----
    # 自动段核对能力位、缺省口径握手、证书摘要形态、ALPN 两态回报、GET 往返与 pin 反向判据；
    # 「这张证书确实是这个站点的那张」只有人能判，故该项在 --interactive 人工段。本探针是唯一出站
    # 的真机面：CI runner 禁网且全部用例按零出站纪律构造，因此它不进 CTest，只在人工验收时运行。
    # 本腿没有 TLS 后端（POSIX 未给 AURORA_NET_TLS_PROVIDERS）时自动段以退出码 2 报 ENV-UNAVAILABLE。
    if (NOT EMSCRIPTEN)
        aurora_add_verify_probe(aurora_verify_tls_live "${_aurora_verify_dir}/tls_live_probe.cpp")
        list(APPEND _aurora_verify_targets aurora_verify_tls_live)
    endif ()
    # ---- WASM rAF 帧循环（浏览器）：Application::run() 经 requestAnimationFrame 真实驱动帧环，
    # 一次验收渲染上屏 / DOM 事件 / deferred 线程池帧尾排空三条接线（期望项见源文件头注释）。
    # 产物为 .html+.js+.wasm，须经 HTTP 静态服务器打开（emrun 最省事）；有意**不进**聚合
    # `aurora_verify`——该目标面向本机直接运行的探针，浏览器产物不满足该语义。
    if (EMSCRIPTEN AND AURORA_BACKEND_WASM)
        aurora_add_verify_probe(aurora_verify_wasm_raf "${_aurora_verify_dir}/wasm_raf_live_probe.cpp")
        set_target_properties(aurora_verify_wasm_raf PROPERTIES SUFFIX ".html")
        # 页面壳提供 <canvas id="aurora-canvas">（WasmOptions::canvas_id 默认值）；
        # 默认 shell 的 #canvas 与上屏契约不符，present 会静默丢帧。
        target_link_options(aurora_verify_wasm_raf PRIVATE
                "--shell-file=${_aurora_verify_dir}/wasm_raf_shell.html")
        # ---- WASM 多窗口（浏览器）：document/window 级单分发器——键盘按焦点
        # Surface 路由、resize 广播全部实例、构造即接管焦点；连带验收字符折算（KeyDown
        # 另发 TextInputEvent）、`raise()` 的 DOM 序真层叠（DOM 末位 + 交叠区
        # elementFromPoint 双证）、`document.title` 的焦点窗口代表制（改名缓存/易主重播/
        # 关窗无幽灵标题）。判据由随库的 CDP 驱动 `wasm_multiwin_cdp_drive.mjs` 派发
        # （需真实浏览器 + 本地 http，故照旧不进聚合 `aurora_verify`，口径同上）。
        aurora_add_verify_probe(aurora_verify_wasm_multiwin
                "${_aurora_verify_dir}/wasm_multiwin_live_probe.cpp")
        set_target_properties(aurora_verify_wasm_multiwin PROPERTIES SUFFIX ".html")
        target_link_options(aurora_verify_wasm_multiwin PRIVATE
                "--shell-file=${_aurora_verify_dir}/wasm_multiwin_shell.html")
        # ---- WASM ARIA 镜像桥（浏览器）：隐藏镜像容器进真实无障碍树（CDP
        # `Accessibility.getFullAXTree` 可证角色/名称）、读屏反向动作经页面级队列帧尾
        # 排水回灌控件、aria-live 播报、增量 ops（首帧全量后仅 applyOps）。同为浏览器
        # 产物，不进聚合 `aurora_verify`（口径同上）。
        aurora_add_verify_probe(aurora_verify_wasm_aria "${_aurora_verify_dir}/wasm_aria_live_probe.cpp")
        set_target_properties(aurora_verify_wasm_aria PROPERTIES SUFFIX ".html")
        target_link_options(aurora_verify_wasm_aria PRIVATE
                "--shell-file=${_aurora_verify_dir}/wasm_aria_shell.html")
        # ---- WASM Web Audio 设备后端（浏览器）：真机验收「推式环形缓冲 + 主线程定泵」
        # 这套零导出接线的四条判据——① 自动播放闸门（未手势：state=0、消费恒 0、图时钟冻结），
        # ② 手势开闸后消费按墙钟推进（≈sample_rate×秒，容差 ±15%）且稳态零欠载，
        # ③ 设备时钟与图时钟同轨（差 ≤0.3s），④ 主线程长任务饿死后欠载计数上升且能自愈续播。
        # 另以 AnalyserNode RMS 证明确有信号抵达 destination（无音频设备的 headless 唯一可测口径）。
        # 探针直连库内部头（`src/aurora/media/audio_webaudio.h` 的观察口不进公共 API，
        # 口径同 WASAPI/ALSA 探针），故需 src 包含目录。判据由 `wasm_audio_cdp_drive.mjs` 派发。
        if (AURORA_ENABLE_AUDIO_WEBAUDIO)
            aurora_add_verify_probe(aurora_verify_wasm_audio "${_aurora_verify_dir}/wasm_audio_live_probe.cpp")
            set_target_properties(aurora_verify_wasm_audio PROPERTIES SUFFIX ".html")
            target_include_directories(aurora_verify_wasm_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
            target_link_options(aurora_verify_wasm_audio PRIVATE
                    "--shell-file=${_aurora_verify_dir}/wasm_audio_shell.html")
        endif ()
    endif ()
    if (_aurora_verify_targets)
        add_custom_target(aurora_verify DEPENDS ${_aurora_verify_targets})
        aurora_log("Verify probes enabled: ${_aurora_verify_targets} (build all with --target aurora_verify)")
    else ()
        aurora_log("AURORA_BUILD_VERIFY_TOOLS=ON but this platform/config matches no probe; nothing defined")
    endif ()
endif ()
