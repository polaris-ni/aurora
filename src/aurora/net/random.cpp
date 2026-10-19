// random.cpp —— 系统熵源的三条腿与进程级唯一缺省源（口径见 `aurora/net/random.h`）。
//
// 本文件是全仓唯一读操作系统熵的地方。三条腿各自只用该腿一定存在的入口，刻意不做「编译期探测 + 运行期回落」
// 的四层瀑布：探测要新增 CMake 检查与 `BUILD_OPTIONS.md` 登记项，而收益只是少写一段 `open` / `read`，
// 换不来任何正确性。取不到熵一律硬失败并点名是哪条腿——本层的失败文本会被 `Error::message` 直接交给调用方。
//
// 链接面：Windows 需要 `bcrypt`（`BCryptGenRandom` 的导入库），它从可选 TLS provider 的链接面升为 Windows 腿
// 的基础链接库（见顶层 `CMakeLists.txt` 的 `ws2_32 / advapi32 / bcrypt / winhttp` 那一行与 MSVC 侧的
// `#pragma comment`）。
#include "aurora/net/random.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "aurora/core/error_codes.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"

#ifdef AURORA_PLATFORM_WINDOWS

#ifndef NOMINMAX  // NOLINT(readability-identifier-naming)
#define NOMINMAX  // NOLINT(readability-identifier-naming)
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// `bcrypt.h` 依赖 `windows.h` 先行的基础类型定义，顺序不可交换。
#include <bcrypt.h>
// clang-format on

#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma comment(lib, "bcrypt.lib")
#endif

#elif defined(AURORA_PLATFORM_WASM)

#include <emscripten.h>

#else

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#endif

namespace aurora::net {
namespace {

/// @brief 把一段字节转成十进制文本（失败 detail 里的状态码与长度都用它）。
///
/// 本腿（wasm）的失败 detail 是常量文本、不带数值，故它在该口径下无人调用；标 `[[maybe_unused]]`
/// 而不是再切一层 `#if`——同一枚辅助函数在两条腿上都有用处，只是第三条用不到。
/// @param value 待转数值。
/// @return 十进制文本。
[[nodiscard]] [[maybe_unused]] auto decimal(unsigned long long value) -> std::string { return std::to_string(value); }

#ifdef AURORA_PLATFORM_WINDOWS

/// @brief Windows 腿单次调用的字节上限：`BCryptGenRandom` 的缓冲区长度按 `ULONG` 交出去，
///        而文档并未保证任意长度都接受；切段让「填多大都行」这条承诺不押在某个未验证的上限上。
constexpr std::size_t AURORA_WINDOWS_RANDOM_CHUNK_BYTES = 65536U;

/// @brief Windows 腿：`BCryptGenRandom` 的系统首选随机数发生器，按 `AURORA_WINDOWS_RANDOM_CHUNK_BYTES` 切段。
///
/// 传 `BCRYPT_USE_SYSTEM_PREFERRED_RNG` 就不必自己开算法句柄：那一枚句柄是进程级资源，随取随放会让每次
/// 掩码取值都多一对 `BCryptOpenAlgorithmProvider` / `BCryptCloseAlgorithmProvider`，而关掉句柄也关不掉
/// 「系统首选」这个选择本身。
/// @param out 目标缓冲。
/// @return 成功，或 `net-platform-unavailable`。
auto fill_platform(std::span<std::byte> out) -> Result<void> {
    std::size_t offset = 0;
    while (offset < out.size()) {
        const std::size_t chunk = std::min(AURORA_WINDOWS_RANDOM_CHUNK_BYTES, out.size() - offset);
        const auto piece = out.subspan(offset, chunk);  // 取切片而不是指针加法：段边界交给 span 管
        // 目标类型只能是该 API 声明的 `PUCHAR`：`std::byte *` 到它的转换除 reinterpret 之外没有别的合法形态。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto *buffer = reinterpret_cast<PUCHAR>(piece.data());
        const auto status =
            BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(chunk), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            return make_error(ErrorCode::NetPlatformUnavailable, "random: BCryptGenRandom failed with NTSTATUS " +
                                                                     decimal(static_cast<unsigned long>(status)));
        }
        offset += chunk;
    }
    return Result<void>{};
}

#elif defined(AURORA_PLATFORM_WASM)

/// @brief WebCrypto 单次调用的字节上限：`getRandomValues` 超过它就抛 `QuotaExceededError`。
constexpr std::size_t AURORA_WASM_CRYPTO_CHUNK_BYTES = 65536U;

// 桥的形态与 `audio_webaudio.cpp` / `http_fetch.cpp` 的 EM_JS 先例同口径：形参取具名 C++ 整型而不是
// `EM_ASM` 的 `$0` / `$1` 占位——后者会让本腿逐枚命中 `-Wdollar-in-identifier-extension`（同族写法见
// `tools/verify/wasm_raf_live_probe.cpp` 里为该告警加的 pragma）。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, aurora_random_fill_js, (std::intptr_t at, int length), {
    const web_crypto = globalThis.crypto;
    if (!web_crypto || typeof web_crypto.getRandomValues !== 'function') {
        return -1;
    }
    try {
        const values = new Uint8Array(length);
        web_crypto.getRandomValues(values);
        HEAPU8.set(values, at);
    } catch (error) {
        return -1;
    }
    return length;
});
// clang-format on

/// @brief wasm 腿：浏览器与 node 都有的 `crypto.getRandomValues`，按 `AURORA_WASM_CRYPTO_CHUNK_BYTES` 切段。
///
/// 走 JS 而不是 Emscripten 的 `getentropy` shim：后者在无 WebCrypto 的环境里会拿 `Math.random` 顶替，那正是
/// 本层要杜绝的「以为有熵、实际没有」。JS 侧先写进一枚新分配的 `Uint8Array` 再 `HEAPU8.set` 回本层缓冲，
/// 不拿 `HEAPU8.buffer` 造视图——线性内存增长会让旧视图失效，而增长只可能发生在调用之间。
/// @param out 目标缓冲。
/// @return 成功，或 `net-platform-unavailable`（环境没交出 `crypto.getRandomValues`）。
auto fill_platform(std::span<std::byte> out) -> Result<void> {
    std::size_t offset = 0;
    while (offset < out.size()) {
        const std::size_t chunk = std::min(AURORA_WASM_CRYPTO_CHUNK_BYTES, out.size() - offset);
        const auto piece = out.subspan(offset, chunk);  // 取切片而不是指针加法：段边界交给 span 管
        // `EM_JS` 形参只收整型，缓冲地址到 `std::intptr_t` 的转换除 reinterpret 之外没有别的合法形态。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto address = reinterpret_cast<std::intptr_t>(piece.data());
        const auto taken = aurora_random_fill_js(address, static_cast<int>(chunk));
        if (taken < 0 || static_cast<std::size_t>(taken) != chunk) {
            return make_error(ErrorCode::NetPlatformUnavailable,
                              "random: crypto.getRandomValues is unavailable on this leg");
        }
        offset += chunk;
    }
    return Result<void>{};
}

#else

/// @brief POSIX 与 macOS 腿：读 `/dev/urandom` 直到填满。
///
/// 只用内核一定供给的入口（`open` / `read` / `close`），不调 `getentropy`——那枚符号在 glibc 上要有版本前提，
/// 探测它就得给构建面加一枚检查。`/dev/urandom` 在现代内核上不是「等到熵池攒够」的阻塞源：`getrandom`
/// 的 `URANDOM_NONBLOCK` 语义与它同形，而本层的调用点在握手与发送路径上，绝不允许长时间阻塞。
/// @param out 目标缓冲。
/// @return 成功，或 `net-platform-unavailable`（打不开设备、读到 EOF、或读失败）。
auto fill_platform(std::span<std::byte> out) -> Result<void> {
    if (out.empty()) {
        return Result<void>{};
    }
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return make_error(ErrorCode::NetPlatformUnavailable,
                          "random: cannot open /dev/urandom, errno " + decimal(static_cast<unsigned long>(errno)));
    }
    // 句柄在这里手工收尾：三段出口都要关，包一层 RAII 类型只为这一处用不划算。
    std::size_t offset = 0;
    std::string reason;
    while (offset < out.size()) {
        const auto piece = out.subspan(offset);  // 取切片而不是指针加法：剩余预算由 span 的长度给出
        const ssize_t read = ::read(fd, piece.data(), piece.size());
        if (read > 0) {
            offset += static_cast<std::size_t>(read);
            continue;
        }
        if (read < 0 && errno == EINTR) {
            continue;  // 被信号打断不是失败：同一预算重读，与 `net::wait_ready` 的 EINTR 口径同源
        }
        if (read == 0) {
            reason = "reached end of /dev/urandom";
        } else {
            reason = "read of /dev/urandom failed, errno " + decimal(static_cast<unsigned long>(errno));
        }
        break;
    }
    const int close_errno = errno;
    ::close(fd);
    if (!reason.empty()) {
        return make_error(ErrorCode::NetPlatformUnavailable, "random: " + reason);
    }
    (void)close_errno;  // 关闭本身失败不改写取数结论：缓冲已填满，没有资源需要调用方替我们担心
    return Result<void>{};
}

#endif

/// @brief 进程级系统熵源：把 `fill` 转发到本腿的 `fill_platform`。
///
/// 无状态：每次取值直接进系统调用，因此可以并发调，也不需要护住任何东西。生命周期取静态期（函数内静态），
/// 首枚交出的就是同一枚对象——`default_random_source()` 的返回类型是引用而不是 `unique_ptr`，靠的正是这点。
/// @note Thread: thread-safe（无共享可变状态）
/// @note Side-effects: 读系统熵源
class SystemRandomSource final : public RandomSource {
  public:
    /// @brief 用系统熵填满 `out`。
    /// @param out 目标缓冲；可为空段。
    /// @return 成功，或 `net-platform-unavailable`。
    [[nodiscard]] auto fill(std::span<std::byte> out) -> Result<void> override { return fill_platform(out); }
};

}  // namespace

auto default_random_source() -> RandomSource & {
    static SystemRandomSource source;  // 静态期单例：C++11 起线程安全的初始化正是这里要的语义
    return source;
}

}  // namespace aurora::net
