# ============================================================
# AuroraUtils.cmake — 公共辅助函数（消费者目标统一配置）
# ------------------------------------------------------------
# aurora_setup_consumer_target(<tgt> [extra private include dirs...])
# 统一处理所有「消费 aurora 库」的可执行目标（demo/测试/工具）的样板：
#   - PRIVATE 链接 aurora（自动获得其 PUBLIC 头目录 / feature 宏 / PCH 锚点）
#   - CXX_STANDARD 20
#   - 复用消费者共享 PCH（REUSE_FROM aurora_consumer_pch，受 AURORA_PCH_ENABLED 门控）
#   - 注入项目统一告警标志（与 aurora 自身一致）
#   - 可选额外 PRIVATE include 目录（ARGN）
# NOMINMAX 由顶层全局 add_compile_definitions 提供（第三方库同样需要），此处不再重复。
# 调用方须在本文件 include 之后、且 aurora_consumer_pch 锚点目标已定义之后调用。
# ============================================================

function(aurora_setup_consumer_target _tgt)
    # 链接 aurora 静态库（PUBLIC 传递头目录 / feature 宏 / 版本定义）。
    target_link_libraries(${_tgt} PRIVATE aurora)
    set_target_properties(${_tgt} PROPERTIES CXX_STANDARD 20)

    # 复用消费者共享 PCH（含 aurora.h，.gch 只编一份）；flags 不匹配时 GCC 安全回退为文本包含。
    # 插桩构建下 PCH 关闭，跳过 REUSE_FROM（见 AURORA_PCH_ENABLED）。
    if (AURORA_PCH_ENABLED)
        target_precompile_headers(${_tgt} REUSE_FROM aurora_consumer_pch)
    endif ()

    # 项目统一告警（mirrors CODING_STANDARDS.md §10）。
    # -Wno-missing-field-initializers：本库大量采用聚合 Props 的部分初始化，其余字段值初始化为零，
    # 该告警纯属噪音，故关闭。
    target_compile_options(${_tgt} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

    # 可选额外 PRIVATE include 目录（如 examples/demos、tests/）。
    if (ARGN)
        target_include_directories(${_tgt} PRIVATE ${ARGN})
    endif ()

    # 静态链接 GCC runtime（libgcc / libstdc++）与 winpthread，使开发工具自包含、双击即跑。
    # MinGW 默认动态链接 libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll，这些 DLL
    # 不在 exe 同目录、也不在普通终端（双击 / 裸 cmd / PowerShell）的 PATH 上，导致运行时报
    # 「无法定位程序输入点 _gthr_win32_self」。
    # 仅 GNU/Clang + Windows 生效；MSVC 与其他平台忽略。
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND WIN32)
        target_link_options(${_tgt} PRIVATE
                -static-libgcc -static-libstdc++
                -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif ()
endfunction()
