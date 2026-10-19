# ============================================================
# AuroraNetTls.cmake — TLS 后端装配（多后端共存形态）
# ------------------------------------------------------------
# 本模块决定「哪几枚 TLS 引擎被编进 aurora」，产出三样东西：
#   - 后端宏（`AURORA_ENABLE_NET_TLS_*`，PRIVATE 作用域：公共头零 `#ifdef`，故消费者编译不需要它，
#     也不导出——导出会让安装侧出现「头里没有、宏却有」的剪裁不一致）；
#   - 每枚引擎一个导入目标（OpenSSL 来自 provider、mbedTLS 来自内置源码：`aurora::net::tls.openssl` /
#     `aurora::net::tls.mbedtls`），外加
#     一个聚合接口目标 `aurora::net::tls`（本体 / 测试 / 工具沿用的名字，不随 provider 数目变化）；
#   - 配置日志里逐条记录「本腿装了哪几枚后端」。
#
# **系统栈与 provider 是并存关系，不是二选一**：`AURORA_ENABLE_NET_TLS_SYSTEM` 与 provider 列表各自独立生效，
# Windows 上给了 OpenSSL 安装根，Schannel 这条腿照样编进来。哪一枚真正被用由运行期的后端注册表
# （`src/aurora/net/tls_backend.cpp`）按候选链决定，构建侧不再保证「恰好一枚」。
#
# 运行期真值不在 here：`Engine::capabilities().tls` 由注册表是否非空回答。本模块只保证「宏与编进来的
# TU 逐一对应」——多定义一枚宏就会多一个未被注册表声明的工厂符号，少定义则那条腿连声明都不存在。
#
# 三枚引擎各有唯一装配入口，互不重叠：
#   - 系统栈（Schannel / SecureTransport）：`AURORA_ENABLE_NET_TLS_SYSTEM`。
#   - OpenSSL：provider 列表 `AURORA_NET_TLS_PROVIDERS`（现**只承载 OpenSSL**）。
#   - mbedTLS：内置源码开关 `AURORA_ENABLE_NET_TLS_MBEDTLS`（third_party/mbedtls，见文件末块）。
#
# OpenSSL provider 语义（逐条对应下方实现）：
#   - 值形如 `<linkage>:<prefix>`，`linkage` ∈ {static, shared}，`prefix` 是**安装根**（含 `include/`
#     与 `lib/`）；不做向上猜测或递归搜索，写错路径时诊断打印实际检查过的组合。
#   - 多条 entry 的分隔符接受 `;`（CMake 列表原生）与 `,`（命令行友好）：命令行里裸 `;` 会被 shell
#     当成语句结束、写成 `\;` 又要额外管一层转义，装配面因此需要一个不需要引用的写法。安装根里
#     带逗号的目录名不被支持——这种路径在本库的三个 runner 上都不存在，而静默把它切开会让诊断
#     指向一条没打算装的 entry。
#   - 种类判定：探 `include/openssl/ssl.h` 命中即 OpenSSL；只探到 `include/mbedtls/ssl.h` 是旧「把
#     mbedTLS 当 provider」的写法，configure 期硬失败并指向 `AURORA_ENABLE_NET_TLS_MBEDTLS`；两者皆无
#     亦硬失败（不静默降级为「无 TLS」）。库定位只查 OpenSSL 自己的库名 `ssl` / `crypto`。
#   - **至多一枚 OpenSSL provider**：两个 OpenSSL 根没有可区分的语义，猜哪一个都是错，故重复命中即
#     configure 期硬失败。
#   - OpenSSL 宏由探测推导、**不接受手工设置**（`-DAURORA_ENABLE_NET_TLS_OPENSSL=ON` 一类不认）；mbedTLS
#     宏则由 `AURORA_ENABLE_NET_TLS_MBEDTLS` 直接决定。
#   - `shared` 的运行期寻址交宿主部署负责：本模块不写 RPATH、不拷 DLL。
#   - mbedTLS 自带源码进 `third_party/mbedtls`（「断网可构建」保持：源码在库、不联网、不改上游）；
#     OpenSSL 仍不内置，由 provider 提供方自负许可证。
# ============================================================

# 系统栈腿的缺省值取决于平台，而 `option()` 的初值只能是一个展开后的常量：先算常量再交进去。
if (WIN32 OR APPLE)
    set(_aurora_tls_system_default ON)
else ()
    set(_aurora_tls_system_default OFF)
endif ()
option(AURORA_ENABLE_NET_TLS_SYSTEM
       "Compile the platform's system TLS stack (Schannel on Windows, SecureTransport on macOS) as a backend"
       ${_aurora_tls_system_default})
unset(_aurora_tls_system_default)

set(AURORA_NET_TLS_PROVIDERS "" CACHE STRING
    "TLS providers as a ';'- or ','-separated list of <static|shared>:<prefix> values (prefix = install root with include/ and lib/)")
set(AURORA_NET_TLS_PROVIDER "" CACHE STRING
    "Legacy single-provider spelling of AURORA_NET_TLS_PROVIDERS; setting both is a configuration error")

# mbedTLS 的构建方式：仓库内置源码构建（`third_party/mbedtls`，与 GLFW / SQLite 同口径——源码进仓库、
# 断网可构建、版本确定、不改上游），不再作为 provider 安装根传入。缺省 OFF；置 ON 即编译并链接
# `third_party/mbedtls`。provider 列表（`AURORA_NET_TLS_PROVIDERS`）自此只承载 OpenSSL。
option(AURORA_ENABLE_NET_TLS_MBEDTLS "Build the bundled mbedTLS TLS backend from third_party/mbedtls (offline source build)" OFF)

# ---- 系统栈腿 ----
# Linux / 其它 POSIX 腿**没有可驱动的系统 TLS 栈**：那里的缺省就是不开，运行期由注册表回答
# `capabilities().tls == false`（除非开 `AURORA_ENABLE_NET_TLS_MBEDTLS`、给 OpenSSL provider 或注入自研后端），
# 不存在退回明文的通路。
set(_aurora_tls_system_macro "")
if (WIN32)
    set(_aurora_tls_system_macro "AURORA_ENABLE_NET_TLS_SCHANNEL")
elseif (APPLE)
    set(_aurora_tls_system_macro "AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT")
endif ()

if (AURORA_ENABLE_NET_TLS_SYSTEM)
    if (NOT _aurora_tls_system_macro)
        # 显式在不支持的平台上打开系统栈是拼错了平台，而不是「想要但没有」：静默无后端会把一次配置
        # 错误伪装成一次成功的构建。
        message(FATAL_ERROR
                "AURORA_ENABLE_NET_TLS_SYSTEM=ON requires a platform with a system TLS stack that Aurora drives "
                "(WIN32 or APPLE); on this platform use AURORA_NET_TLS_PROVIDERS=<static|shared>:<prefix> "
                "instead, or turn the switch off.")
    endif ()
    aurora_define_feature(${_aurora_tls_system_macro} SCOPE PRIVATE)
    aurora_log("TLS backend: system stack (${_aurora_tls_system_macro})")
else ()
    aurora_log("TLS backend: system stack disabled (AURORA_ENABLE_NET_TLS_SYSTEM=OFF)")
endif ()

# ---- provider 腿（可多枚，与系统栈并存）----
# 旧单值变量保留为别名：CI 与手工验收既有写法仍可用，但两者同时置值即口径冲突，不猜以谁为准。
if (AURORA_NET_TLS_PROVIDER AND AURORA_NET_TLS_PROVIDERS)
    message(FATAL_ERROR
            "Both AURORA_NET_TLS_PROVIDER and AURORA_NET_TLS_PROVIDERS are set: they are the same knob in "
            "two spellings, so there is no answer to \"which one wins\". Clear the legacy "
            "AURORA_NET_TLS_PROVIDER and keep: ${AURORA_NET_TLS_PROVIDERS}")
endif ()
set(_aurora_tls_entries "${AURORA_NET_TLS_PROVIDERS}")
if (NOT _aurora_tls_entries AND AURORA_NET_TLS_PROVIDER)
    set(_aurora_tls_entries "${AURORA_NET_TLS_PROVIDER}")
endif ()
# `,` 归一为 `;` 后才进 foreach：分隔符口径只在这一处建立，下方逐条校验看到的已是 CMake 列表。
string(REPLACE "," ";" _aurora_tls_entries "${_aurora_tls_entries}")

set(_aurora_tls_seen_kinds "")
set(_aurora_tls_provider_targets "")
foreach (_aurora_tls_entry IN LISTS _aurora_tls_entries)
    # 分隔符只按第一个冒号切分：Windows 安装根可能带盘符冒号（C:\openssl）。
    string(FIND "${_aurora_tls_entry}" ":" _aurora_tls_colon)
    if (_aurora_tls_colon LESS 0)
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDERS entries must be <static|shared>:<prefix>, got '${_aurora_tls_entry}' "
                "(no ':' separator; e.g. static:/opt/mbedtls)")
    endif ()
    string(SUBSTRING "${_aurora_tls_entry}" 0 ${_aurora_tls_colon} _aurora_tls_linkage)
    math(EXPR _aurora_tls_rest "${_aurora_tls_colon} + 1")
    string(SUBSTRING "${_aurora_tls_entry}" ${_aurora_tls_rest} -1 _aurora_tls_prefix)

    if (NOT _aurora_tls_linkage STREQUAL "static" AND NOT _aurora_tls_linkage STREQUAL "shared")
        message(FATAL_ERROR
                "TLS provider linkage must be 'static' or 'shared', got '${_aurora_tls_linkage}' "
                "(full entry: '${_aurora_tls_entry}')")
    endif ()
    if (_aurora_tls_prefix STREQUAL "")
        message(FATAL_ERROR
                "TLS provider entry needs an install root after the colon, got '${_aurora_tls_entry}' "
                "(expected <static|shared>:<prefix> with <prefix>/include and <prefix>/lib)")
    endif ()
    file(TO_CMAKE_PATH "${_aurora_tls_prefix}" _aurora_tls_prefix)
    if (NOT IS_DIRECTORY "${_aurora_tls_prefix}/include")
        message(FATAL_ERROR
                "TLS provider prefix '${_aurora_tls_prefix}' has no include/ directory "
                "(checked: ${_aurora_tls_prefix}/include). Only an install root is accepted.")
    endif ()

    # provider 列表现只承载 OpenSSL：mbedTLS 改由仓库内置源码构建（文件末 `AURORA_ENABLE_NET_TLS_MBEDTLS`
    # 块），不再作为 provider 安装根。探测只认 OpenSSL 头；两个都命中时按 OpenSSL 处理并记账。
    set(_aurora_tls_header_openssl "${_aurora_tls_prefix}/include/openssl/ssl.h")
    set(_aurora_tls_kind "")
    if (EXISTS "${_aurora_tls_header_openssl}")
        set(_aurora_tls_kind "openssl")
        if (EXISTS "${_aurora_tls_prefix}/include/mbedtls/ssl.h")
            aurora_log("TLS provider probe: openssl/ssl.h and mbedtls/ssl.h both under "
                       "${_aurora_tls_prefix}/include -> this entry is OpenSSL (mbedTLS is bundled, not a provider)")
        endif ()
    elseif (EXISTS "${_aurora_tls_prefix}/include/mbedtls/ssl.h")
        # 只探到 mbedTLS 头：多半是旧「把 mbedTLS 当 provider」的写法。不静默降级——点名新入口。
        message(FATAL_ERROR
                "TLS provider prefix '${_aurora_tls_prefix}' offers only mbedTLS, but mbedTLS is no longer a "
                "provider — it is built from the bundled source. Drop this entry from AURORA_NET_TLS_PROVIDERS "
                "(the list now takes OpenSSL install roots only) and set -DAURORA_ENABLE_NET_TLS_MBEDTLS=ON instead.")
    else ()
        message(FATAL_ERROR
                "TLS provider prefix '${_aurora_tls_prefix}' offers no OpenSSL: checked "
                "${_aurora_tls_header_openssl}. AURORA_NET_TLS_PROVIDERS now accepts only OpenSSL install roots "
                "(mbedTLS is bundled via -DAURORA_ENABLE_NET_TLS_MBEDTLS=ON). Pass the install root (not include/ "
                "itself), or drop the entry to use the system stack.")
    endif ()
    if (_aurora_tls_kind IN_LIST _aurora_tls_seen_kinds)
        message(FATAL_ERROR
                "Two TLS providers resolve to the same backend kind '${_aurora_tls_kind}'. A provider list "
                "selects *which stacks exist*, not which installation a given link line uses, so two roots of "
                "one kind have no distinguishable meaning. Keep one ${_aurora_tls_kind} root "
                "(list so far: ${_aurora_tls_entries}).")
    endif ()
    list(APPEND _aurora_tls_seen_kinds "${_aurora_tls_kind}")

    # 库定位：只在该前缀的 lib/ 下找，找不到即 configure 期硬失败并打印查找过的目录。
    # Debian 系把库放在 `lib/<三元组>`（multiarch），那是 `lib/` 的子目录而非另一个安装根，故它属于
    # 「只接受安装根」的口径之内；不带上它就会在 WSL/Ubuntu 上「头探到了、库说没有」。
    set(_aurora_tls_libdir "${_aurora_tls_prefix}/lib")
    set(_aurora_tls_hints "${_aurora_tls_libdir}")
    if (CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND _aurora_tls_hints "${_aurora_tls_libdir}/${CMAKE_LIBRARY_ARCHITECTURE}")
    endif ()

    # 库定位按 OpenSSL 自己的库名查（不再取 ssl/mbedtls 并集——并集会让「头推导出 mbedTLS、却链上
    # OpenSSL 的库」这种错配在安装根混挂两套库时发生）。缓存变量按种类命名，多枚 provider 各自求解。
    find_library(AURORA_NET_TLS_OPENSSL_SSL_LIB
                 NAMES ssl
                 HINTS ${_aurora_tls_hints} NO_DEFAULT_PATH
                 NAMES_PER_DIR)
    find_library(AURORA_NET_TLS_OPENSSL_CRYPTO_LIB
                 NAMES crypto
                 HINTS ${_aurora_tls_hints} NO_DEFAULT_PATH
                 NAMES_PER_DIR)
    set(_aurora_tls_ssl_lib "${AURORA_NET_TLS_OPENSSL_SSL_LIB}")
    set(_aurora_tls_crypto_lib "${AURORA_NET_TLS_OPENSSL_CRYPTO_LIB}")
    # Windows 上的 OpenSSL DLL 名不带 "openssl"（vcpkg 是 `libssl-3-x64.dll`），glob 按库名取。
    set(_aurora_tls_dll_glob "*ssl*.dll")
    if (NOT _aurora_tls_ssl_lib OR NOT _aurora_tls_crypto_lib)
        message(FATAL_ERROR
                "TLS provider '${_aurora_tls_kind}' has its headers under ${_aurora_tls_prefix}/include but its "
                "libraries are missing in ${_aurora_tls_hints} "
                "(ssl='${_aurora_tls_ssl_lib}' crypto='${_aurora_tls_crypto_lib}').")
    endif ()

    # 导入目标：每枚 provider 一个，include 与库路径由本体 / 测试 / 工具共用。
    set(_aurora_tls_target "aurora::net::tls.${_aurora_tls_kind}")
    if (_aurora_tls_linkage STREQUAL "shared")
        set(_aurora_tls_imported SHARED)
    else ()
        set(_aurora_tls_imported STATIC)
    endif ()
    if (NOT TARGET ${_aurora_tls_target})
        add_library(${_aurora_tls_target} ${_aurora_tls_imported} IMPORTED GLOBAL)
    endif ()
    if (_aurora_tls_linkage STREQUAL "shared" AND WIN32)
        # Windows 共享腿的「链接面」与「运行面」是两枚文件：lib/ 下是导入库（.lib，链接器经它
        # 找符号），bin/ 下是 DLL（运行期载入，vcpkg 名带版本后缀如 libssl-3-x64.dll）。MSVC 对
        # 共享导入目标强制要求 IMPORTED_IMPLIB，缺失即 generate 期报「IMPORTED_IMPLIB not set」；
        # 而 find_library 的 Windows 后缀表（.dll.lib/.lib/.a）只可能命中 .lib，正好当 implib。
        # DLL 按 glob 补登 IMPORTED_LOCATION；glob 落空（布局异常）时只留 implib 同样能生成，
        # 且 Windows 共享腿的消费者不运行（见 CI 口径），链接面齐备即足。
        file(GLOB _aurora_tls_ssl_dll "${_aurora_tls_prefix}/bin/${_aurora_tls_dll_glob}")
        set_target_properties(${_aurora_tls_target} PROPERTIES
                              IMPORTED_IMPLIB "${_aurora_tls_ssl_lib}"
                              INTERFACE_INCLUDE_DIRECTORIES "${_aurora_tls_prefix}/include")
        if (_aurora_tls_ssl_dll)
            list(GET _aurora_tls_ssl_dll 0 _aurora_tls_ssl_dll_first)
            set_target_properties(${_aurora_tls_target} PROPERTIES IMPORTED_LOCATION "${_aurora_tls_ssl_dll_first}")
        endif ()
    else ()
        set_target_properties(${_aurora_tls_target} PROPERTIES
                              IMPORTED_LOCATION "${_aurora_tls_ssl_lib}"
                              INTERFACE_INCLUDE_DIRECTORIES "${_aurora_tls_prefix}/include")
    endif ()
    # 其余库经 INTERFACE_LINK_LIBRARIES 传导（IMPORTED_LOCATION 只放首枚 ssl，避免同一 lib 重复出现在链接行）。
    # OpenSSL 只有 ssl → crypto 一层依赖，crypto 排在后即正确；mbedTLS 三枚归档的依赖序改由文件末内置块负责。
    set(_aurora_tls_iface "${_aurora_tls_crypto_lib}")
    if (NOT WIN32)
        # 静态链时线程与 dl 这两项不会由库自身带上。
        find_package(Threads REQUIRED)
        list(APPEND _aurora_tls_iface "${CMAKE_DL_LIBS}" "Threads::Threads")
    endif ()
    set_property(TARGET ${_aurora_tls_target} PROPERTY INTERFACE_LINK_LIBRARIES "${_aurora_tls_iface}")

    list(APPEND _aurora_tls_provider_targets "${_aurora_tls_target}")
    string(TOUPPER "${_aurora_tls_kind}" _aurora_tls_kind_upper)
    aurora_define_feature(AURORA_ENABLE_NET_TLS_${_aurora_tls_kind_upper} SCOPE PRIVATE)
    aurora_log("TLS backend: provider ${_aurora_tls_kind_upper} (${_aurora_tls_linkage}) from ${_aurora_tls_prefix}")
endforeach ()

# ---- mbedTLS provider（仓库内置源码构建；AURORA_ENABLE_NET_TLS_MBEDTLS=ON 时启用，缺省 OFF）----
# 与 GLFW / SQLite 同口径：源码进 third_party、断网可构建、版本确定、不改上游源码。只编静态核心库
# （关测试与示例程序），EXCLUDE_FROM_ALL 让它仅在被 aurora 链接时连带构建。tls_mbedtls.cpp 的头与
# 符号需求（含 <psa/crypto.h>、psa_crypto_init）由本树满足，不再依赖外部安装根。
if (AURORA_ENABLE_NET_TLS_MBEDTLS)
    set(_mbedtls_src ${AURORA_SOURCE_DIR}/third_party/mbedtls)
    if (NOT EXISTS ${_mbedtls_src}/CMakeLists.txt)
        aurora_error([=[AURORA_ENABLE_NET_TLS_MBEDTLS=ON but third_party/mbedtls sources are missing (expected CMakeLists.txt and include/mbedtls/ssl.h). Vendor the 3.6 LTS tree, unmodified: git clone --depth 1 --branch mbedtls-3.6.5 https://github.com/Mbed-TLS/mbedtls.git third_party/mbedtls — mbedTLS 3.6 ships PSA Crypto in-tree; a 4.x / tf-psa-crypto split build also needs that submodule populated.]=])
    endif ()
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)        # 不编 mbedTLS 自带动辄几百个的测试程序
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)      # 不编 programs/（ssl_*、benchmark 等）
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    # mbedTLS 自带的 install() 会把 libmbedtls.a 与头文件装进 CMAKE_INSTALL_PREFIX，污染本库手写的
    # AuroraConfig 导出包（安装面必须只含 aurora）。add_subdirectory 前置目录属性 SKIP_INSTALL_RULES
    # 令其 install() 变空操作，出块还原——EXCLUDE_FROM_ALL 只挡 ALL 目标、不挡 install。
    # 同时用 CMAKE_POLICY_VERSION_MINIMUM（CMake 3.31+）把子项目的策略下限抬到 3.10：mbedTLS 3.6.5
    # 写的是 cmake_minimum_required(3.5.1)，在 CMake 4.x 下当前只是「<3.10 兼容将被移除」的告警，
    # 抬下限既消警告，也防后续 CMake 版本把它升级成硬错误（不改上游源码）。
    set(_saved_skip_install ${CMAKE_SKIP_INSTALL_RULES})
    set(_saved_policy_min ${CMAKE_POLICY_VERSION_MINIMUM})
    set(CMAKE_SKIP_INSTALL_RULES YES)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    add_subdirectory(${_mbedtls_src} ${CMAKE_BINARY_DIR}/third_party/mbedtls EXCLUDE_FROM_ALL)
    set(CMAKE_SKIP_INSTALL_RULES ${_saved_skip_install})
    set(CMAKE_POLICY_VERSION_MINIMUM ${_saved_policy_min})
    foreach (_mbed_tgt mbedtls mbedx509 mbedcrypto)
        if (NOT TARGET ${_mbed_tgt})
            aurora_error("mbedTLS source build did not produce the '${_mbed_tgt}' target; check third_party/mbedtls.")
        endif ()
        # 三方源码压制告警（与 glfw 同口径）：顶层 -Wall 已收敛到 aurora 目标级，这里再兜一层。
        if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${_mbed_tgt} PRIVATE "-w")
        elseif (MSVC)
            target_compile_options(${_mbed_tgt} PRIVATE "/w")
        endif ()
    endforeach ()
    # 沿用按种类命名的聚合口 `aurora::net::tls.mbedtls`（AuroraVerify / 探针 / 安装侧引用面不随装配方式变化）。
    if (NOT TARGET aurora::net::tls.mbedtls)
        add_library(aurora::net::tls.mbedtls INTERFACE IMPORTED GLOBAL)
    endif ()
    # 静态归档 GNU ld 左到右求解：mbedtls → mbedx509 → mbedcrypto（x509 必须排在 crypto 之前）。
    set_property(TARGET aurora::net::tls.mbedtls PROPERTY INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto")
    # 并入 provider 聚合列表，令下方 `aurora::net::tls` 聚合与 `target_link_libraries(aurora PRIVATE ...)` 一并带上。
    list(APPEND _aurora_tls_provider_targets aurora::net::tls.mbedtls)
    aurora_define_feature(AURORA_ENABLE_NET_TLS_MBEDTLS SCOPE PRIVATE)
    aurora_log("TLS backend: bundled mbedTLS (third_party/mbedtls source build)")
endif ()

if (_aurora_tls_provider_targets)
    # 聚合接口目标：名字沿用 `aurora::net::tls`，装配模块之外的引用点（AuroraVerify、真机探针、
    # 安装侧）因此不随 provider 数目变化。
    if (NOT TARGET aurora::net::tls)
        add_library(aurora::net::tls INTERFACE IMPORTED GLOBAL)
    endif ()
    set_target_properties(aurora::net::tls PROPERTIES INTERFACE_LINK_LIBRARIES "${_aurora_tls_provider_targets}")
    target_link_libraries(aurora PRIVATE aurora::net::tls)
endif ()

# ---- 系统栈的链接面 ----
# 只由 `AURORA_ENABLE_NET_TLS_SYSTEM` 决定，与 provider 是否存在无关（这正是系统栈与 provider 能共存的构建面
# 前提）。Schannel：secur32（握手与记录层）+ crypt32（证书链 / DN 文本 / SPKI 编码）+ bcrypt（SPKI 摘要）。
# SecureTransport 与 CoreFoundation / Security 框架同理，挂在顶层而非后端块内（TLS 腿与渲染后端无关）。
if (_aurora_tls_system_macro STREQUAL "AURORA_ENABLE_NET_TLS_SCHANNEL")
    target_link_libraries(aurora PUBLIC secur32 crypt32 bcrypt)
elseif (_aurora_tls_system_macro STREQUAL "AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT")
    target_link_libraries(aurora PUBLIC "-framework Security" "-framework CoreFoundation")
endif ()

unset(_aurora_tls_system_macro)
unset(_aurora_tls_entries)
unset(_aurora_tls_seen_kinds)
unset(_aurora_tls_provider_targets)
