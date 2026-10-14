# AuroraCompression.cmake
# ---------------------------------------------------------------------------
# core 级通用解压能力（src/aurora/core/compression.cpp）的构建接线。
#
# 实现对三方件 wuffs（third_party/wuffs）的复用：wuffs 单文件 C 库提供
# gzip / zlib / deflate 三族解码器（无编码器，压缩方向只设计不实现）。
# OBJECT 库 aurora_wuffs 由本模块统一持有，创建条件取「图片 PNG 开关 ∨
# 本开关」并集——两个消费者（图片编解码与压缩能力）共享同一份编译产物，
# 避免同开时双份符号链接冲突。
#
# AURORA_ENABLE_COMPRESSION 默认 ON：解压是 HTTP 语义完整性件（提案 §12
# 「缺这些即语义不完整」），且无替换路径（不同于可缺席的替换型后端
# SQLite / 图片编解码 / 音频设备），默认构建即提供；关时 compression.cpp
# 全格式落 compression-unavailable、HTTP 侧同步不发 Accept-Encoding
# （诚实降级，对齐音频 silent-mode 先例）。
# PRIVATE 注入且不导出：能力属库内部，消费者与安装后口径一致（无该宏）。
# ---------------------------------------------------------------------------

option(AURORA_ENABLE_COMPRESSION "Build core compression capability (gzip/zlib/deflate decompression via wuffs)" ON)

if (AURORA_ENABLE_COMPRESSION)
    # PRIVATE 注入且不导出：压缩能力属库内部，消费者与安装后口径一致（无该宏）。
    aurora_define_feature(AURORA_ENABLE_COMPRESSION SCOPE PRIVATE)
endif ()

# ---- wuffs 单文件 C 库（图片 PNG 编解码与压缩能力共用一份 OBJECT 产物）----
# ⚠️ 本块须在 include(AuroraImageCodecs) 之前执行：PNG codec 不再自建 OBJECT 库，
# 改由此处按并集条件创建后直接链入 aurora。
if (AURORA_ENABLE_IMAGE_PNG OR AURORA_ENABLE_COMPRESSION)
    set(_WUFFS_C ${AURORA_SOURCE_DIR}/third_party/wuffs/release/c/wuffs-v0.3.c)
    add_library(aurora_wuffs OBJECT ${_WUFFS_C})
    set_source_files_properties(${_WUFFS_C} PROPERTIES LANGUAGE C)
    target_compile_definitions(aurora_wuffs PRIVATE WUFFS_IMPLEMENTATION)
    # -std=c99 为 GCC/Clang 专属；MSVC 对 C 目标用默认标准即可。
    if (MSVC)
        target_compile_options(aurora_wuffs PRIVATE /w)
    else ()
        target_compile_options(aurora_wuffs PRIVATE -std=c99 -w)
    endif ()
    target_include_directories(aurora_wuffs PRIVATE ${AURORA_SOURCE_DIR}/third_party/wuffs/release/c)
    # compression.cpp 与图片 codec 均以 "wuffs/release/c/wuffs-v0.3.c" 声明式包含
    # （wuffs 官方单文件集成约定：整份 .c 当头包含取声明，WUFFS_IMPLEMENTATION 由
    # 本 OBJECT 库在唯一 TU 定义），故 aurora 需要 third_party 根作为搜索路径。
    target_include_directories(aurora PRIVATE ${AURORA_SOURCE_DIR}/third_party)
    target_link_libraries(aurora PRIVATE aurora_wuffs)
endif ()
