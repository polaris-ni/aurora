# AuroraImageCodecs.cmake
# ---------------------------------------------------------------------------
# 统一图片编解码 API 的外部编解码器接入。
#
# 注册表内核（src/aurora/image/{registry,api}.cpp）与基础编解码器
# （stb/bmp/svg/png-write，GLOB 自动收集）始终构建；本模块仅负责
# 「外部库编解码器」：libjpeg-turbo (JPEG)、libwebp (WebP)、wuffs (PNG/GIF)。
#
# 三个开关默认 OFF，以保证仓库默认构建（MinGW Makefiles）零错误零警告。
# 启用任一开关时，对应 third_party 源码被编译为 OBJECT 库并链接进 aurora；
# 各 codec .cpp（src/aurora/image/codecs/*_codec.cpp）内部以
# `#ifdef AURORA_BUILD_IMAGE_*` 包裹，OFF 时该 TU 为空，不会引入悬空引用。
# ---------------------------------------------------------------------------

option(AURORA_BUILD_IMAGE_JPEG "Build JPEG codec via libjpeg-turbo" OFF)
option(AURORA_BUILD_IMAGE_WEBP "Build WebP codec via libwebp" OFF)
option(AURORA_BUILD_IMAGE_PNG "Build PNG/GIF codec via wuffs" OFF)

# ---------------------------------------------------------------------------
# JPEG (libjpeg-turbo)
# 注意：libjpeg-turbo 的 CMakeLists.txt 显式禁止 add_subdirectory，故从源码
# 直接编译为 OBJECT 库，并生成 jconfig.h。
# ---------------------------------------------------------------------------
if (AURORA_BUILD_IMAGE_JPEG)
    add_compile_definitions(AURORA_BUILD_IMAGE_JPEG)

    set(_JPEG_SRC ${CMAKE_SOURCE_DIR}/third_party/libjpeg-turbo)
    # jpeg_turbo_codec.cpp 以 <jpeglib.h> 引用 libjpeg-turbo 头，需要把其 src 目录
    # 加入 aurora 目标的私有 include 路径（jconfig.h/jconfigint.h/jversion.h 由上方
    # configure_file 生成到 gen/jpeg，同样需加入）。
    target_include_directories(aurora PRIVATE
            ${_JPEG_SRC}/src
            ${CMAKE_BINARY_DIR}/gen/jpeg)
    # 由模板生成 jconfig.h / jconfigint.h（MinGW/Windows 默认值）。
    # 仅编译 src/*.c（不含 simd/），故 WITH_SIMD / 算术编码一律关闭；
    # 下方临时设置与模板占位同名的变量供 @ONLY 替换，#cmakedefine 行在无对应变量时
    # 自动展开为 #undef。生成后立即 unset，避免污染外层作用域。
    # JPEG_LIB_VERSION=80 对应 libjpeg v8 ABI（libjpeg-turbo 默认）。
    set(JPEG_LIB_VERSION "80")
    set(VERSION "3.2.0")
    set(LIBJPEG_TURBO_VERSION_NUMBER "3020000")
    set(COPYRIGHT_YEAR "2026")
    set(BUILD "")
    set(HIDDEN "")
    set(INLINE "inline")
    set(THREAD_LOCAL "__thread")
    set(SIZE_T "${CMAKE_SIZEOF_VOID_P}")
    set(SIMD_ARCHITECTURE "0")
    configure_file(
            ${_JPEG_SRC}/src/jconfig.h.in
            ${CMAKE_BINARY_DIR}/gen/jpeg/jconfig.h
            @ONLY)
    configure_file(
            ${_JPEG_SRC}/src/jconfigint.h.in
            ${CMAKE_BINARY_DIR}/gen/jpeg/jconfigint.h
            @ONLY)
    configure_file(
            ${_JPEG_SRC}/src/jversion.h.in
            ${CMAKE_BINARY_DIR}/gen/jpeg/jversion.h
            @ONLY)
    unset(JPEG_LIB_VERSION)
    unset(VERSION)
    unset(LIBJPEG_TURBO_VERSION_NUMBER)
    unset(COPYRIGHT_YEAR)
    unset(BUILD)
    unset(HIDDEN)
    unset(INLINE)
    unset(THREAD_LOCAL)
    unset(SIZE_T)
    unset(SIMD_ARCHITECTURE)
    # Precision-dependent sources are compiled via the upstream src/wrapper/*-8/12/16.c
    # shims, which #define BITS_IN_JSAMPLE before #include-ing the real .c. This is
    # required because jcinit.c references the 12-bit (and, under C_LOSSLESS_SUPPORTED,
    # 16-bit) variants of the color converter / downsampler / prep / main / coef
    # controllers unconditionally; compiling the raw .c directly yields only the 8-bit
    # symbols and breaks the link (undefined j12init_*). The tool-only rd*/wr* helpers
    # (cjpeg/djpeg) are intentionally omitted — the codec uses only the libjpeg API.
    set(_JPEG_WRAP ${_JPEG_SRC}/src/wrapper)
    # Multi-precision sources are compiled via the upstream src/wrapper/*-8/12/16.c
    # shims, which #define BITS_IN_JSAMPLE before #include-ing the real .c. This is
    # required because jcinit.c references the 12-bit (and, under C_LOSSLESS_SUPPORTED,
    # 16-bit) variants of the color converter / downsampler / prep / main / coef
    # controllers unconditionally; compiling the raw .c directly yields only the 8-bit
    # symbols and breaks the link (undefined j12init_*).  Collect all shims by glob
    # (they are pure library files) but drop the cjpeg/djpeg tool I/O shims, which the
    # codec does not need, to mirror the upstream JPEG_SOURCES library set.
    file(GLOB _JPEG_WRAP_SRCS
            ${_JPEG_WRAP}/*-8.c ${_JPEG_WRAP}/*-12.c ${_JPEG_WRAP}/*-16.c)
    list(FILTER _JPEG_WRAP_SRCS EXCLUDE REGEX
            "/(rdppm|wrppm|rdpng|wrpng|wrgif|rdcolmap)-")
    # Single-precision (8-bit) library sources have no multi-precision wrapper and are
    # compiled directly.  (src/ also contains turbojpeg / cjpeg / djpeg / jpegtran /
    # example / test sources, which are intentionally NOT part of the library.)
    add_library(aurora_jpeg OBJECT
            ${_JPEG_WRAP_SRCS}
            ${_JPEG_SRC}/src/jcapimin.c
            ${_JPEG_SRC}/src/jchuff.c ${_JPEG_SRC}/src/jcicc.c ${_JPEG_SRC}/src/jcinit.c ${_JPEG_SRC}/src/jclhuff.c
            ${_JPEG_SRC}/src/jcmarker.c ${_JPEG_SRC}/src/jcmaster.c ${_JPEG_SRC}/src/jcomapi.c
            ${_JPEG_SRC}/src/jcparam.c ${_JPEG_SRC}/src/jcphuff.c
            ${_JPEG_SRC}/src/jctrans.c ${_JPEG_SRC}/src/jdapimin.c
            ${_JPEG_SRC}/src/jdatadst.c ${_JPEG_SRC}/src/jdatasrc.c
            ${_JPEG_SRC}/src/jdhuff.c ${_JPEG_SRC}/src/jdicc.c ${_JPEG_SRC}/src/jdinput.c ${_JPEG_SRC}/src/jdlhuff.c
            ${_JPEG_SRC}/src/jdmarker.c ${_JPEG_SRC}/src/jdmaster.c
            ${_JPEG_SRC}/src/jdphuff.c
            ${_JPEG_SRC}/src/jdtrans.c ${_JPEG_SRC}/src/jerror.c ${_JPEG_SRC}/src/jfdctflt.c
            ${_JPEG_SRC}/src/jmemmgr.c ${_JPEG_SRC}/src/jmemnobs.c ${_JPEG_SRC}/src/jpeg_nbits.c
            ${_JPEG_SRC}/src/jaricom.c ${_JPEG_SRC}/src/jcarith.c ${_JPEG_SRC}/src/jdarith.c)
    target_include_directories(aurora_jpeg PRIVATE
            ${_JPEG_SRC}/src
            ${CMAKE_BINARY_DIR}/gen/jpeg)
    # 抑制第三方源码告警（不参与项目 -Wall/-Wextra/-Wpedantic）
    target_compile_options(aurora_jpeg PRIVATE -w -Wno-implicit-function-declaration)
    target_link_libraries(aurora PRIVATE aurora_jpeg)
endif ()

# ---------------------------------------------------------------------------
# WebP (libwebp)
# ---------------------------------------------------------------------------
if (AURORA_BUILD_IMAGE_WEBP)
    add_compile_definitions(AURORA_BUILD_IMAGE_WEBP)
    set(_WEBP_SRC ${CMAKE_SOURCE_DIR}/third_party/libwebp)
    file(GLOB_RECURSE _WEBP_C ${_WEBP_SRC}/src/*.c)
    # libwebp's enc layer references the SharpYUV RGB->YUV helpers unconditionally
    # (SharpYuvInit / SharpYuvConvert / SharpYuvGetConversionMatrix). Those live in
    # the top-level sharpyuv/ directory, which is NOT under src/, so the GLOB above
    # misses them and the link fails with undefined SharpYuv*.  Add the base
    # sharpyuv sources plus the x86 SSE2 dispatcher (WEBP_HAVE_SSE2 is defined in
    # this build, so InitSharpYuvSSE2 is referenced by sharpyuv_dsp.c) and exclude
    # the ARM NEON file on non-NEON targets.
    file(GLOB _WEBP_SHARPYUV ${_WEBP_SRC}/sharpyuv/*.c)
    list(FILTER _WEBP_SHARPYUV EXCLUDE REGEX "sharpyuv_neon\\.c$")
    list(APPEND _WEBP_C ${_WEBP_SHARPYUV})
    add_library(aurora_webp OBJECT ${_WEBP_C})
    target_include_directories(aurora_webp PRIVATE
            ${_WEBP_SRC}
            ${_WEBP_SRC}/src
            ${_WEBP_SRC}/src/webp
            ${_WEBP_SRC}/src/dec
            ${_WEBP_SRC}/src/dsp
            ${_WEBP_SRC}/src/enc
            ${_WEBP_SRC}/src/utils
            ${_WEBP_SRC}/src/mux
            ${_WEBP_SRC}/src/demux)
    target_compile_definitions(aurora_webp PRIVATE WEBP_HAVE_SSE2=0)
    target_compile_options(aurora_webp PRIVATE -w)
    # webp_codec.cpp 以 <webp/decode.h> 引用 libwebp 公共头，需把其 src 目录加入 aurora 的 include 路径。
    target_include_directories(aurora PRIVATE ${_WEBP_SRC}/src)
    target_link_libraries(aurora PRIVATE aurora_webp)
endif ()

# ---------------------------------------------------------------------------
# PNG / GIF (wuffs) —— 单文件 C 库，最易接入
# ---------------------------------------------------------------------------
if (AURORA_BUILD_IMAGE_PNG)
    add_compile_definitions(AURORA_BUILD_IMAGE_PNG)
    set(_WUFFS_C ${CMAKE_SOURCE_DIR}/third_party/wuffs/release/c/wuffs-v0.3.c)
    add_library(aurora_wuffs OBJECT ${_WUFFS_C})
    set_source_files_properties(${_WUFFS_C} PROPERTIES LANGUAGE C)
    target_compile_definitions(aurora_wuffs PRIVATE WUFFS_IMPLEMENTATION)
    target_compile_options(aurora_wuffs PRIVATE -std=c99 -w)
    target_include_directories(aurora_wuffs PRIVATE ${CMAKE_SOURCE_DIR}/third_party/wuffs/release/c)
    target_include_directories(aurora PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(aurora PRIVATE aurora_wuffs)
endif ()
