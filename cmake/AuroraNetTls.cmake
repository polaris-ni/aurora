# ============================================================
# AuroraNetTls.cmake — TLS 后端装配（CHG-018：§5 平台矩阵 + §8 宏矩阵）
# ------------------------------------------------------------
# 本模块是「哪个 TLS 引擎被编进 aurora」的唯一决策点，产出三样东西：
#   - 后端宏（`AURORA_NET_TLS_*`，PRIVATE 作用域：公共头零 `#ifdef`，故消费者编译不需要它，
#     也不导出——导出会让安装侧出现「头里没有、宏却有」的剪裁不一致）；
#   - provider 形态下的导入目标 `aurora::net::tls`（系统栈形态下不存在该目标）；
#   - 配置日志里一条「本腿用的是哪条腿」的显式记录。
#
# 运行期真值不 here：`Engine::capabilities().tls` 由 `detail::tls_backend_available()` 回答，而它由
# 被编译的那个后端 TU 定义，因此能力位与「调用真的能成功」严格同源。本模块只保证恰好编译一个后端 TU。
#
# provider 语义（§5 定案，逐条对应下方实现）：
#   - 值形如 `<linkage>:<prefix>`，`linkage` ∈ {static, shared}，`prefix` 是**安装根**（含 `include/`
#     与 `lib/`）；不做向上猜测或递归搜索，写错路径时诊断打印实际检查过的组合。
#   - **后端种类由探测判定**：`include/openssl/ssl.h` → OpenSSL，`include/mbedtls/ssl.h` → mbedTLS，
#     两者皆无 → configure 期硬失败（不静默降级为「无 TLS」），多命中 → OpenSSL 优先并显式记录。
#   - 宏由探测推导，**不接受手工设置**（`-DAURORA_NET_TLS_OPENSSL=ON` 一类不认）。
#   - `shared` 的运行期寻址交宿主部署负责：本模块不写 RPATH、不拷 DLL。
#   - 仓库不自带 TLS 源码：`third_party/` 无新增，「断网可构建」不变。
# ============================================================

set(AURORA_NET_TLS_PROVIDER "" CACHE STRING
    "TLS provider as <static|shared>:<prefix> (prefix = install root with include/ and lib/). Empty = platform system stack.")

# ---- provider 为空：按平台缺省装配系统栈 ----
# Linux / 其它 POSIX 腿**没有缺省系统栈**：未给 provider 即编译 `tls_unavailable.cpp`，
# 运行期以 `capabilities().tls == false` 诚实回答（提案 §5 的行「无默认」）。
set(_aurora_tls_system_macro "")
if (WIN32)
    set(_aurora_tls_system_macro "AURORA_NET_TLS_SCHANNEL")
elseif (APPLE)
    set(_aurora_tls_system_macro "AURORA_NET_TLS_SECURE_TRANSPORT")
endif ()

if (AURORA_NET_TLS_PROVIDER STREQUAL "")
    if (_aurora_tls_system_macro)
        aurora_define_feature(${_aurora_tls_system_macro} SCOPE PRIVATE)
        aurora_log("TLS backend: system stack (${_aurora_tls_system_macro})")
    else ()
        aurora_log("TLS backend: none (set AURORA_NET_TLS_PROVIDER=<static|shared>:<prefix> to enable)")
    endif ()
else ()
    # ---- provider 形态：先校验值文法，再探测头，最后定位库 ----
    # 分隔符只按第一个冒号切分：Windows 安装根可能带盘符冒号（C:\openssl）。
    string(FIND "${AURORA_NET_TLS_PROVIDER}" ":" _aurora_tls_colon)
    if (_aurora_tls_colon LESS 0)
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDER must be <static|shared>:<prefix>, got '${AURORA_NET_TLS_PROVIDER}' "
                "(no ':' separator; e.g. static:/opt/mbedtls)")
    endif ()
    string(SUBSTRING "${AURORA_NET_TLS_PROVIDER}" 0 ${_aurora_tls_colon} _aurora_tls_linkage)
    math(EXPR _aurora_tls_rest "${_aurora_tls_colon} + 1")
    string(SUBSTRING "${AURORA_NET_TLS_PROVIDER}" ${_aurora_tls_rest} -1 _aurora_tls_prefix)

    if (NOT _aurora_tls_linkage STREQUAL "static" AND NOT _aurora_tls_linkage STREQUAL "shared")
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDER linkage must be 'static' or 'shared', got '${_aurora_tls_linkage}' "
                "(full value: '${AURORA_NET_TLS_PROVIDER}')")
    endif ()
    if (_aurora_tls_prefix STREQUAL "" OR _aurora_tls_prefix STREQUAL "${AURORA_NET_TLS_PROVIDER}")
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDER needs an install root after the colon, got '${AURORA_NET_TLS_PROVIDER}' "
                "(expected <static|shared>:<prefix> with <prefix>/include and <prefix>/lib)")
    endif ()
    file(TO_CMAKE_PATH "${_aurora_tls_prefix}" _aurora_tls_prefix)
    if (NOT IS_DIRECTORY "${_aurora_tls_prefix}/include")
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDER prefix '${_aurora_tls_prefix}' has no include/ directory "
                "(checked: ${_aurora_tls_prefix}/include). Only an install root is accepted.")
    endif ()

    # 头探测：种类由文件存在性推导，两个都命中时 OpenSSL 优先并显式记录（不静默）。
    set(_aurora_tls_header_openssl "${_aurora_tls_prefix}/include/openssl/ssl.h")
    set(_aurora_tls_header_mbedtls "${_aurora_tls_prefix}/include/mbedtls/ssl.h")
    set(_aurora_tls_macro "")
    if (EXISTS "${_aurora_tls_header_openssl}")
        set(_aurora_tls_macro "AURORA_NET_TLS_OPENSSL")
        if (EXISTS "${_aurora_tls_header_mbedtls}")
            aurora_log("TLS provider probe: both openssl/ssl.h and mbedtls/ssl.h found under "
                       "${_aurora_tls_prefix}/include -> OpenSSL wins by rule (mbedTLS ignored)")
        endif ()
    elseif (EXISTS "${_aurora_tls_header_mbedtls}")
        set(_aurora_tls_macro "AURORA_NET_TLS_MBEDTLS")
    else ()
        message(FATAL_ERROR
                "AURORA_NET_TLS_PROVIDER prefix '${_aurora_tls_prefix}' offers no TLS library: checked "
                "${_aurora_tls_header_openssl} and ${_aurora_tls_header_mbedtls}. "
                "Pass the install root (not include/ itself), or clear AURORA_NET_TLS_PROVIDER for the system stack.")
    endif ()

    # 库定位：只在该前缀的 lib/ 下找，找不到即 configure 期硬失败并打印查找过的目录。
    # Debian 系把库放在 `lib/<三元组>`（multiarch），那是 `lib/` 的子目录而非另一个安装根，故它属于
    # 「只接受安装根」的口径之内；不带上它就会在 WSL/Ubuntu 上「头探到了、库说没有」。
    set(_aurora_tls_libdir "${_aurora_tls_prefix}/lib")
    set(_aurora_tls_hints "${_aurora_tls_libdir}")
    if (CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND _aurora_tls_hints "${_aurora_tls_libdir}/${CMAKE_LIBRARY_ARCHITECTURE}")
    endif ()
    find_library(AURORA_NET_TLS_SSL_LIB
                 NAMES ssl mbedtls
                 HINTS ${_aurora_tls_hints} NO_DEFAULT_PATH
                 NAMES_PER_DIR)
    find_library(AURORA_NET_TLS_CRYPTO_LIB
                 NAMES crypto mbedcrypto
                 HINTS ${_aurora_tls_hints} NO_DEFAULT_PATH
                 NAMES_PER_DIR)
    if (NOT AURORA_NET_TLS_SSL_LIB OR NOT AURORA_NET_TLS_CRYPTO_LIB)
        message(FATAL_ERROR
                "TLS provider '${_aurora_tls_macro}' headers found under ${_aurora_tls_prefix}/include but its "
                "libraries are missing in ${_aurora_tls_hints} "
                "(ssl/mbedtls='${AURORA_NET_TLS_SSL_LIB}' crypto/mbedcrypto='${AURORA_NET_TLS_CRYPTO_LIB}').")
    endif ()
    set(_aurora_tls_ssl_lib "${AURORA_NET_TLS_SSL_LIB}")
    set(_aurora_tls_crypto_lib "${AURORA_NET_TLS_CRYPTO_LIB}")

    # mbedTLS 还需要 x509 层（证书链解析在库内，不在 TLS 记录层）。
    if (_aurora_tls_macro STREQUAL "AURORA_NET_TLS_MBEDTLS")
        find_library(AURORA_NET_TLS_X509_LIB
                     NAMES mbedx509
                     HINTS ${_aurora_tls_hints} NO_DEFAULT_PATH
                     NAMES_PER_DIR)
        if (NOT AURORA_NET_TLS_X509_LIB)
            message(FATAL_ERROR
                    "TLS provider mbedTLS is missing mbedx509 in ${_aurora_tls_hints} "
                    "(certificate chain verification needs it).")
        endif ()
    endif ()

    # 导入目标：一次创建、三条腿（aurora 本体 / 测试 / 工具）共用同一份 include 与库路径。
    if (_aurora_tls_linkage STREQUAL "shared")
        set(_aurora_tls_imported SHARED)
    else ()
        set(_aurora_tls_imported STATIC)
    endif ()
    if (NOT TARGET aurora::net::tls)
        add_library(aurora::net::tls ${_aurora_tls_imported} IMPORTED GLOBAL)
    endif ()
    if (_aurora_tls_linkage STREQUAL "shared" AND WIN32)
        # Windows 共享腿的「链接面」与「运行面」是两枚文件：lib/ 下是导入库（.lib，链接器经它
        # 找符号），bin/ 下是 DLL（运行期载入，vcpkg 名带版本后缀如 libssl-3-x64.dll）。MSVC 对
        # 共享导入目标强制要求 IMPORTED_IMPLIB，缺失即 generate 期报「IMPORTED_IMPLIB not set」；
        # 而 find_library 的 Windows 后缀表（.dll.lib/.lib/.a）只可能命中 .lib，正好当 implib。
        # DLL 按 glob 补登 IMPORTED_LOCATION；glob 落空（布局异常）时只留 implib 同样能生成，
        # 且 Windows 共享腿的消费者不运行（见 CI 口径），链接面齐备即足。
        file(GLOB _aurora_tls_ssl_dll "${_aurora_tls_prefix}/bin/*ssl*.dll")
        set_target_properties(aurora::net::tls PROPERTIES
                              IMPORTED_IMPLIB "${_aurora_tls_ssl_lib}"
                              INTERFACE_INCLUDE_DIRECTORIES "${_aurora_tls_prefix}/include")
        if (_aurora_tls_ssl_dll)
            list(GET _aurora_tls_ssl_dll 0 _aurora_tls_ssl_dll_first)
            set_target_properties(aurora::net::tls PROPERTIES IMPORTED_LOCATION "${_aurora_tls_ssl_dll_first}")
        endif ()
    else ()
        set_target_properties(aurora::net::tls PROPERTIES
                              IMPORTED_LOCATION "${_aurora_tls_ssl_lib}"
                              INTERFACE_INCLUDE_DIRECTORIES "${_aurora_tls_prefix}/include")
    endif ()
    # 其余库经 INTERFACE_LINK_LIBRARIES 传导（IMPORTED_LOCATION 只放首枚，避免同一 lib 重复出现在链接行）。
    # 顺序按 GNU ld 的左到右求解规则排：被依赖者在后。mbedTLS 三枚archive的依赖方向是
    # mbedtls → mbedx509 → mbedcrypto，故 x509 必须排在 crypto **之前**，否则静态链接会把
    # `mbedtls_pk_parse_subpubkey` / `mbedtls_pk_load_file` 报成未定义（实测 WSL 腿）。
    # OpenSSL 侧只有 ssl → crypto 一层依赖，`_aurora_tls_crypto_lib` 排在后即正确。
    if (_aurora_tls_macro STREQUAL "AURORA_NET_TLS_MBEDTLS")
        set(_aurora_tls_extra "${AURORA_NET_TLS_X509_LIB}" "${_aurora_tls_crypto_lib}")
    else ()
        set(_aurora_tls_extra "${_aurora_tls_crypto_lib}")
    endif ()
    if (NOT WIN32)
        # 两枚系统栈都依赖线程本地存储与 dl；静态链时这两项不会由库自身带上。
        find_package(Threads REQUIRED)
        set(_aurora_tls_extra ${_aurora_tls_extra} ${CMAKE_DL_LIBS} Threads::Threads)
    endif ()
    set_property(TARGET aurora::net::tls PROPERTY INTERFACE_LINK_LIBRARIES "${_aurora_tls_extra}")

    aurora_define_feature(${_aurora_tls_macro} SCOPE PRIVATE)
    target_link_libraries(aurora PRIVATE aurora::net::tls)
    aurora_log("TLS backend: provider ${_aurora_tls_macro} (${_aurora_tls_linkage}) from ${_aurora_tls_prefix}")
endif ()

# ---- 系统栈的链接面 ----
# Schannel：secur32（握手与记录层）+ crypt32（证书链 / DN 文本 / SPKI 编码）+ bcrypt（SPKI 摘要）。
# SecureTransport 与 CoreFoundation / Security 框架同理，挂在顶层而非后端块内（TLS 腿与渲染后端无关）。
if (WIN32 AND _aurora_tls_system_macro STREQUAL "AURORA_NET_TLS_SCHANNEL")
    target_link_libraries(aurora PUBLIC secur32 crypt32 bcrypt)
elseif (APPLE AND _aurora_tls_system_macro STREQUAL "AURORA_NET_TLS_SECURE_TRANSPORT")
    target_link_libraries(aurora PUBLIC "-framework Security" "-framework CoreFoundation")
endif ()

unset(_aurora_tls_system_macro)
