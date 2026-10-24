# ============================================================
# AuroraI18n.cmake — 构建期消息管线（gen_i18n）的下游入口
#
# 提供 `aurora_i18n_generate()`：把 ARB 形态的词表 JSON（基准语言 + 任意多份译文）
# 交给 `gen_i18n`，产出两件——薄生成头（强类型 Id + 每 key 一枚具名形参构造函数）与
# AUM1 二进制词表（运行期 `MessageCatalog::load_binary` 载入）。CLI 契约与校验清单见
# codespec/specification/08-tooling.md，bin 布局与回退口径见
# codespec/specification/07-environment-modifier.md §6。
#
# 前置：`gen_i18n` 目标随 AuroraTools.cmake 常建（Emscripten 交叉下由宿主原生子配置产出，
# 见 cmake/AuroraTools.cmake 的 native_gen_tools 段）。经 `find_package(Aurora)` 消费的
# 下游拿不到该目标，故本助手只服务 in-tree / `add_subdirectory` 形态；安装交付的产物形态
# 是直接调用已构建的 `gen_i18n` 可执行文件。
#
# 用法（须在 `add_executable(myapp ...)` 之后调用）：
#   aurora_i18n_generate(
#           TARGET myapp
#           NAMESPACE myapp::msg
#           BASE i18n/messages_en.json
#           LOCALES i18n/messages_zh-Hans-CN.json i18n/messages_ru.json
#           OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/i18n
#           ID_BASE 1024)
#   # 源文件里：#include "messages_en.hpp"（OUT_DIR 已进 include 路径）
#   # 运行期：整读 messages_en.bin 交 default_catalog().load_binary()
#   #        bin 路径已由 AURORA_I18N_BIN_PATH 宏给出（构建期绝对路径；也随可执行文件落同目录）
# ============================================================

# 生成器可执行路径：由 AuroraTools.cmake 按工具链落定（原生 exe 或宿主原生子配置 exe）。
# 未定义时本助手直接报错——静默降级会让词表停在旧版本，比红灯危险。
function(aurora_i18n_generate)
    set(_one_value_args TARGET NAMESPACE BASE OUT_DIR ID_BASE)
    set(_multi_value_args LOCALES)
    cmake_parse_arguments(AURORA_I18N "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if (AURORA_I18N_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "aurora_i18n_generate: unknown argument(s): ${AURORA_I18N_UNPARSED_ARGUMENTS} "
                "(known: TARGET NAMESPACE BASE LOCALES OUT_DIR [ID_BASE])")
    endif ()
    foreach(_required IN ITEMS TARGET NAMESPACE BASE OUT_DIR)
        if (NOT AURORA_I18N_${_required})
            message(FATAL_ERROR "aurora_i18n_generate: ${_required} is required")
        endif ()
    endforeach ()
    if (NOT AURORA_I18N_ID_BASE)
        set(AURORA_I18N_ID_BASE 1024)  # 内置保留区 [0, 1024) 之外的应用词表缺省基址
    endif ()
    if (NOT TARGET ${AURORA_I18N_TARGET})
        message(FATAL_ERROR
                "aurora_i18n_generate: TARGET '${AURORA_I18N_TARGET}' does not exist yet "
                "(call the helper after add_executable / add_library)")
    endif ()
    if (NOT DEFINED AURORA_GEN_I18N_EXE)
        message(FATAL_ERROR
                "aurora_i18n_generate: the gen_i18n tool is not available in this configuration "
                "(AuroraTools.cmake defines AURORA_GEN_I18N_EXE; downstream find_package users call "
                "the built gen_i18n executable directly)")
    endif ()

    # 入参路径按调用方的源码目录解析，产出统一落 OUT_DIR（相对写法在 add_subdirectory 与
    # 顶层 include 两种接入形态下都能锚定到同一处）。
    set(_base "${AURORA_I18N_BASE}")
    if (NOT IS_ABSOLUTE "${_base}")
        get_filename_component(_base "${_base}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif ()
    set(_locales "")
    foreach(_locale IN LISTS AURORA_I18N_LOCALES)
        if (IS_ABSOLUTE "${_locale}")
            list(APPEND _locales "${_locale}")
        else ()
            get_filename_component(_abs_locale "${_locale}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            list(APPEND _locales "${_abs_locale}")
        endif ()
    endforeach ()
    set(_out_dir "${AURORA_I18N_OUT_DIR}")
    get_filename_component(_out_dir "${_out_dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    file(MAKE_DIRECTORY "${_out_dir}")

    # 产物名取基准文件 stem：`messages_en.json` -> `messages_en.hpp` / `messages_en.bin`。
    # 同一 OUT_DIR 内不得放两套词表同名基准文件（Id 常量与 bin 会互相覆盖）。
    get_filename_component(_stem "${_base}" NAME_WE)
    set(_out_hpp "${_out_dir}/${_stem}.hpp")
    set(_out_bin "${_out_dir}/${_stem}.bin")

    # 生成命令逐字节确定性（同输入同输出）由 gen_i18n 本身保证，故只依赖输入文件与生成器；
    # 译文增删改、基准文件改动、重新构建生成器三者都会自然触发重生成。
    add_custom_command(
            OUTPUT "${_out_hpp}" "${_out_bin}"
            COMMAND "${AURORA_GEN_I18N_EXE}" "${_out_hpp}" "${_out_bin}" "${AURORA_I18N_NAMESPACE}"
                    --id-base "${AURORA_I18N_ID_BASE}" "${_base}" ${_locales}
            DEPENDS "${AURORA_GEN_I18N_EXE}" "${_base}" ${_locales}
            COMMENT "Generating i18n message header and binary vocabulary from ${_stem}.json"
            VERBATIM)

    # 头进 target 源表：CMake 由此知道「先有生成物再编源」；bin 不属任何源表，故由本
    # 自定义目标兜住（否则只改译文不改头用法时 bin 不会重生成）。bin 随可执行文件落同目录，
    # 应用侧整读该文件喂 MessageCatalog::load_binary（Aurora 不做文件 IO，wasm 同理）。
    add_custom_target(${AURORA_I18N_TARGET}_i18n DEPENDS "${_out_hpp}" "${_out_bin}")
    add_dependencies(${AURORA_I18N_TARGET} ${AURORA_I18N_TARGET}_i18n)
    # 全局登记本生成目标：`lint` / `lint-fix` 是聚合目标、不走编译链，clang-tidy 直接读源文件时
    # 这份生成头还不存在（与 wayland-gen 同类的先决产物）。AuroraLint.cmake 在全部模块 include
    # 之后读本清单并把它挂成 lint 的依赖，CI 四条平台腿与本机 `--target lint` 因此同一口径。
    set_property(GLOBAL APPEND PROPERTY AURORA_I18N_LINT_PREREQ_TARGETS "${AURORA_I18N_TARGET}_i18n")
    set_source_files_properties("${_out_hpp}" PROPERTIES GENERATED TRUE)
    target_sources(${AURORA_I18N_TARGET} PRIVATE "${_out_hpp}")
    target_include_directories(${AURORA_I18N_TARGET} PRIVATE "${_out_dir}")
    # bin 的构建期绝对路径以宏给出：in-tree 载体（demo / 测试）据此整读文件喂 load_binary，
    # 不必猜自身 cwd。安装交付形态由应用自行安置 bin，本宏只是便利项（未定义时读侧自择路径）。
    target_compile_definitions(${AURORA_I18N_TARGET} PRIVATE AURORA_I18N_BIN_PATH="${_out_bin}")
    add_custom_command(TARGET ${AURORA_I18N_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out_bin}"
                    "$<TARGET_FILE_DIR:${AURORA_I18N_TARGET}>/${_stem}.bin"
            COMMENT "Copying i18n message binary next to ${AURORA_I18N_TARGET}"
            VERBATIM)
endfunction()
