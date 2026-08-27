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
    # 由模板生成 jconfig.h（MinGW/Windows 默认值）
    configure_file(
            ${_JPEG_SRC}/src/jconfig.h.in
            ${CMAKE_BINARY_DIR}/gen/jpeg/jconfig.h
            @ONLY)
    add_library(aurora_jpeg OBJECT
            ${_JPEG_SRC}/src/jcapimin.c ${_JPEG_SRC}/src/jcapistd.c ${_JPEG_SRC}/src/jcarith.c
            ${_JPEG_SRC}/src/jccoefct.c ${_JPEG_SRC}/src/jccolor.c ${_JPEG_SRC}/src/jcdctmgr.c
            ${_JPEG_SRC}/src/jchuff.c ${_JPEG_SRC}/src/jcinit.c ${_JPEG_SRC}/src/jcmainct.c
            ${_JPEG_SRC}/src/jcmarker.c ${_JPEG_SRC}/src/jcmaster.c ${_JPEG_SRC}/src/jcomapi.c
            ${_JPEG_SRC}/src/jcparam.c ${_JPEG_SRC}/src/jcphuff.c ${_JPEG_SRC}/src/jcprepct.c
            ${_JPEG_SRC}/src/jcsample.c ${_JPEG_SRC}/src/jctrans.c ${_JPEG_SRC}/src/jdapimin.c
            ${_JPEG_SRC}/src/jdapistd.c ${_JPEG_SRC}/src/jdarith.c ${_JPEG_SRC}/src/jdatadst.c
            ${_JPEG_SRC}/src/jdatasrc.c ${_JPEG_SRC}/src/jdcoefct.c ${_JPEG_SRC}/src/jdcolor.c
            ${_JPEG_SRC}/src/jddctmgr.c ${_JPEG_SRC}/src/jdhuff.c ${_JPEG_SRC}/src/jdinput.c
            ${_JPEG_SRC}/src/jdmainct.c ${_JPEG_SRC}/src/jdmarker.c ${_JPEG_SRC}/src/jdmaster.c
            ${_JPEG_SRC}/src/jdmerge.c ${_JPEG_SRC}/src/jdpostct.c ${_JPEG_SRC}/src/jdsample.c
            ${_JPEG_SRC}/src/jdtrans.c ${_JPEG_SRC}/src/jerror.c ${_JPEG_SRC}/src/jfdctflt.c
            ${_JPEG_SRC}/src/jfdctfst.c ${_JPEG_SRC}/src/jfdctint.c ${_JPEG_SRC}/src/jidctflt.c
            ${_JPEG_SRC}/src/jidctfst.c ${_JPEG_SRC}/src/jidctint.c ${_JPEG_SRC}/src/jidctred.c
            ${_JPEG_SRC}/src/jquant1.c ${_JPEG_SRC}/src/jquant2.c ${_JPEG_SRC}/src/jutils.c
            ${_JPEG_SRC}/src/jmemmgr.c ${_JPEG_SRC}/src/jmemnobs.c ${_JPEG_SRC}/src/jaricom.c
            ${_JPEG_SRC}/src/rdbmp.c ${_JPEG_SRC}/src/rdppm.c ${_JPEG_SRC}/src/rdgif.c
            ${_JPEG_SRC}/src/rdtarga.c ${_JPEG_SRC}/src/wrbmp.c ${_JPEG_SRC}/src/wrppm.c
            ${_JPEG_SRC}/src/wrtarga.c)
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
    add_library(aurora_webp OBJECT ${_WEBP_C})
    target_include_directories(aurora_webp PRIVATE
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
