// Aurora net — wasm fetch 引擎桥（CHG-018 §16 第 37 项）。
//
// 职责只有一件：把一次 HTTP 交换打包给浏览器 `fetch`，把结局带回来。协议层（重定向链、
// 超时改道、认证环、进度、Cookie 选择）全在发起侧既有环里；本文件是 wasm 腿的「传输」。
//
// 桥的形态（与 audio_webaudio / wasm_aria 的 EM_JS 先例同口径）：
//   - C++ → JS：`aurora_fetch_start(id, …)` 只收整型与地址；字符串与体先拷进线性内存的
//     临时缓冲，start 的同步段完成 UTF8ToString 与 Uint8Array 拷贝后即由 C++ 侧释放。
//   - JS → C++：`aurora_fetch_settle(id, …)`（`extern "C"` + `EMSCRIPTEN_KEEPALIVE`）；
//     JS 侧经 `aurora_fetch_alloc` / `aurora_fetch_free` 在 wasm 堆上交接应答字节。
//   - 待决交换表：C++ 与 JS 各持一份（C++ 的是结算投递口，JS 的是 `AbortController` 索引），
//     都以交换 id 为键；id 由 C++ 侧单调分配。

#include "aurora/net/detail/http_fetch.h"

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WASM

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <utility>

#include <emscripten.h>

#include "aurora/core/result.h"

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off

namespace aurora::net {
namespace {

using detail::FetchOutcome;

/// @brief 待决交换的 C++ 侧登记项：结算投递口（调用方协程的续体）。
struct PendingFetch {
    std::function<void(Result<FetchOutcome>)> settle;  ///< 结算投递口；只许调用一次
};

/// @brief 待决交换表（进程级单例；wasm 单线程，无需加锁）。
/// @return 表引用。
auto pending_table() -> std::unordered_map<int, PendingFetch> & {
    static std::unordered_map<int, PendingFetch> table;
    return table;
}

/// @brief 交换 id 分配器（单调递增；wasm 单线程）。
auto next_exchange_id() -> int {
    static int counter = 0;
    return ++counter;
}

}  // namespace

namespace detail {
namespace {

/// @brief 在 wasm 堆上分配一块并拷入字节串（含 NUL 终结位，UTF8ToString 需要）。
/// @param text 待拷字节串。
/// @return 堆地址；调用方经 `aurora_fetch_free` 归还。
auto copy_to_heap(const std::string &text) -> std::intptr_t {
    const auto raw = std::malloc(text.size() + 1);  // NOLINT(cppcoreguidelines-owning-memory,-warnings-as-errors)
    if (raw == nullptr) {
        return 0;
    }
    std::memcpy(raw, text.data(), text.size());
    (static_cast<char *>(raw))[text.size()] = '\0';
    return reinterpret_cast<std::intptr_t>(raw);
}

// @brief JS 侧向 wasm 堆要一块应答缓冲（`extern "C"` 供 EM_JS 调用，导出面仅此三枚）。
extern "C" EMSCRIPTEN_KEEPALIVE auto aurora_fetch_alloc(int bytes) -> std::intptr_t {
    if (bytes <= 0) {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,-warnings-as-errors) wasm 堆上的交接缓冲
    return reinterpret_cast<std::intptr_t>(std::malloc(static_cast<std::size_t>(bytes)));
}

// @brief 归还交接缓冲（C++ 侧收尾与 JS 侧异常路径共用）。
extern "C" EMSCRIPTEN_KEEPALIVE auto aurora_fetch_free(std::intptr_t address) -> void {
    std::free(reinterpret_cast<void *>(address));  // NOLINT(cppcoreguidelines-owning-memory,-warnings-as-errors)
}

// @brief JS 侧结算口：把一次交换的结局交回待决表并恢复发起协程。
//
// `kind`：0 = 拿到应答（状态 / 头 / 体有效），1 = fetch 本身拒绝，2 = 被 `AbortController`
// 打断。缓冲按同一口径归还：凡指针非 0 即 `aurora_fetch_free`。
extern "C" EMSCRIPTEN_KEEPALIVE void aurora_fetch_settle(int id, int kind, int status, std::intptr_t reason_ptr,
                                                         int reason_len, std::intptr_t headers_ptr, int headers_len,
                                                         std::intptr_t body_ptr, int body_len) {
    auto &table = pending_table();
    const auto slot = table.find(id);
    if (slot == table.end()) {
        // 待决项已不在（发起协程随取消提前收场）：缓冲照常归还，结局就地丢弃。
        aurora_fetch_free(reason_ptr);
        aurora_fetch_free(headers_ptr);
        aurora_fetch_free(body_ptr);
        return;
    }
    auto settle = std::move(slot->second.settle);
    table.erase(slot);

    FetchOutcome outcome;
    switch (kind) {
        case 0:
            outcome.ending = FetchOutcome::Ending::Completed;
            outcome.status = static_cast<std::uint16_t>(status);
            if (reason_ptr != 0 && reason_len > 0) {
                outcome.reason.assign(reinterpret_cast<const char *>(reason_ptr), static_cast<std::size_t>(reason_len));
            }
            if (headers_ptr != 0 && headers_len > 0) {
                outcome.headers_text.assign(reinterpret_cast<const char *>(headers_ptr),
                                            static_cast<std::size_t>(headers_len));
            }
            if (body_ptr != 0 && body_len > 0) {
                const auto *first = reinterpret_cast<const char *>(body_ptr);
                outcome.body.assign(first, first + static_cast<std::size_t>(body_len));
            }
            break;
        case 2:
            outcome.ending = FetchOutcome::Ending::Aborted;
            break;
        default:
            outcome.ending = FetchOutcome::Ending::NetworkError;
            break;
    }
    aurora_fetch_free(reason_ptr);
    aurora_fetch_free(headers_ptr);
    aurora_fetch_free(body_ptr);
    settle(Result<FetchOutcome>{std::move(outcome)});
}

// clang-format on

// @brief JS 侧发起口：解析规格缓冲 → fetch → 结算。
//
// 字符串与体在**同步段**内完成拷贝（JS 字符串与 `new Uint8Array(...)` 都是复制语义），返回后
// C++ 侧即可释放缓冲。重定向永不自动跟随（`redirect: 'manual'`）；浏览器 cookie 存储不参与
// （`credentials: 'omit'`）；截止到点由 `AbortController` 打断。
// clang-format off
EM_JS(void, aurora_fetch_start,
      (int id, std::intptr_t url_ptr, int url_len, std::intptr_t method_ptr, int method_len,
       std::intptr_t headers_ptr, int headers_len, std::intptr_t body_ptr, int body_len, double deadline_ms),
      {
        if (globalThis.__auroraFetchPending === undefined) {
            globalThis.__auroraFetchPending = new Map();
        }
        const url = UTF8ToString(url_ptr, url_len);
        const method = UTF8ToString(method_ptr, method_len);
        const headers = {};
        const header_lines = UTF8ToString(headers_ptr, headers_len).split('\n');
        for (const line of header_lines) {
            const at = line.indexOf(':');
            if (at > 0) {
                headers[line.slice(0, at)] = line.slice(at + 1);
            }
        }
        const body = body_len > 0 ? new Uint8Array(HEAPU8.subarray(body_ptr, body_ptr + body_len)) : undefined;
        const controller = new AbortController();
        globalThis.__auroraFetchPending.set(id, controller);
        const deadline = deadline_ms > 0 ? setTimeout(() => controller.abort(), deadline_ms) : null;
        // fetch 包进一枚已决 promise 再 .then：实现（或替身）同步抛出时折算成 rejection 走同一结算口，
        // 绝不让 JS 异常穿越 EM_JS 落进 C++ 帧。
        new Promise((resolve) => resolve(fetch(url, {
            method, headers, body, signal: controller.signal, redirect: 'manual', credentials: 'omit',
        })))
            .then(async (resp) => {
                if (deadline !== null) {
                    clearTimeout(deadline);
                }
                globalThis.__auroraFetchPending.delete(id);
                const lines = [];
                resp.headers.forEach((value, name) => lines.push(name + ': ' + value));
                // Set-Cookie 不进 forEach（平台把它从响应头迭代里排除）：getSetCookie 在场就逐枚补入，
                // 缺席（旧平台）就如实收不到——§18-6 的「Set-Cookie 不可读」注记的运行期形态。
                if (typeof resp.headers.getSetCookie === 'function') {
                    for (const cookie of resp.headers.getSetCookie()) {
                        lines.push('set-cookie: ' + cookie);
                    }
                }
                const headers_text = lines.join('\n');
                const buffer = new Uint8Array(await resp.arrayBuffer());
                const reason_bytes = lengthBytesUTF8(resp.statusText);
                const reason_ptr = _aurora_fetch_alloc(reason_bytes + 1);
                if (reason_ptr !== 0) {
                    stringToUTF8(resp.statusText, reason_ptr, reason_bytes + 1);
                }
                const headers_bytes = lengthBytesUTF8(headers_text);
                const headers_ptr = _aurora_fetch_alloc(headers_bytes + 1);
                if (headers_ptr !== 0) {
                    stringToUTF8(headers_text, headers_ptr, headers_bytes + 1);
                }
                const body_ptr = _aurora_fetch_alloc(buffer.length);
                if (body_ptr !== 0 && buffer.length > 0) {
                    HEAPU8.set(buffer, body_ptr);
                }
                _aurora_fetch_settle(id, 0, resp.status, reason_ptr, reason_ptr === 0 ? 0 : reason_bytes,
                                     headers_ptr, headers_ptr === 0 ? 0 : headers_bytes, body_ptr, buffer.length);
            })
            .catch((err) => {
                if (deadline !== null) {
                    clearTimeout(deadline);
                }
                const controller = globalThis.__auroraFetchPending.get(id);
                globalThis.__auroraFetchPending.delete(id);
                const aborted = controller !== undefined && controller.signal.aborted;
                _aurora_fetch_settle(id, aborted ? 2 : 1, 0, 0, 0, 0, 0, 0, 0);
            });
      });
// clang-format on

// @brief JS 侧打断口：按 id 找到在途交换的 `AbortController` 并打断（幂等；找不到即空操作）。
// clang-format off
EM_JS(void, aurora_fetch_abort, (int id), {
    if (globalThis.__auroraFetchPending !== undefined) {
        const controller = globalThis.__auroraFetchPending.get(id);
        if (controller !== undefined) {
            controller.abort();
        }
    }
});
// clang-format on

}  // namespace

auto fetch_leg_active() -> bool { return true; }

auto fetch_exchange(const FetchSpec &spec, const Cancellation &token) -> CallbackAwaiter<FetchOutcome> {
    return CallbackAwaiter<FetchOutcome>(
        // 发起器经 std::function 类型擦除转入触发的系统性假告警面：结算只做移动与投递，不抛。
        // 令牌按值入闭包（共享状态句柄）：登记取消钩子要一枚可变的句柄，形参的 const 引用给不了。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [&spec, token_handle = token](CallbackAwaiter<FetchOutcome>::Sink on_done) mutable -> void {
            if (token_handle.is_cancelled()) {
                on_done(Result<FetchOutcome>{make_error(ErrorCode::NetCancelled, "the exchange was already cancelled")});
                return;
            }
            const int id = next_exchange_id();
            // 取消钩子：打断在途 fetch（结局仍由 settle 交回，此处不直接投递）。结算后撤登记
            // （令牌的钩子是单槽替换语义，见 `Cancellation::on_cancel`——与原生传输逐段换登记同构）。
            token_handle.on_cancel([id]() -> void { aurora_fetch_abort(id); });
            auto &slot = pending_table()[id];
            slot.settle = std::move(on_done);

            // 请求头合成「name: value\n」合并文本交给 JS 侧解回对象（同名多枚不进这里：请求侧
            // 本就按名唯一，Set-Cookie 是应答侧字段）。
            std::string headers_text;
            for (const auto &field : spec.headers) {
                headers_text += field.first;
                headers_text += ':';
                headers_text += field.second;
                headers_text += '\n';
            }
            // 缓冲只须活过 start 的同步段：字符串已被 UTF8ToString 拷走、体已被 Uint8Array 拷走。
            const auto url_ptr = copy_to_heap(spec.url);
            const auto method_ptr = copy_to_heap(spec.method_text);
            const auto headers_ptr = copy_to_heap(headers_text);
            auto body_ptr = std::intptr_t{0};
            if (!spec.body.empty()) {
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,-warnings-as-errors)
                body_ptr = reinterpret_cast<std::intptr_t>(std::malloc(spec.body.size()));
                if (body_ptr != 0) {
                    std::memcpy(reinterpret_cast<void *>(body_ptr), spec.body.data(), spec.body.size());
                }
            }
            if (url_ptr == 0 || method_ptr == 0 || headers_ptr == 0 ||
                (!spec.body.empty() && body_ptr == 0)) {
                // 分配失败：登记即撤、钩子即撤，以运行时不可用收场（不静默假装发过）。
                auto failed = std::move(pending_table()[id].settle);
                pending_table().erase(id);
                token_handle.on_cancel({});
                aurora_fetch_free(url_ptr);
                aurora_fetch_free(method_ptr);
                aurora_fetch_free(headers_ptr);
                aurora_fetch_free(body_ptr);
                failed(Result<FetchOutcome>{
                    make_error(ErrorCode::NetRuntimeFailed, "fetch bridge: out of memory while staging the request")});
                return;
            }
            aurora_fetch_start(id, url_ptr, static_cast<int>(spec.url.size()), method_ptr,
                               static_cast<int>(spec.method_text.size()), headers_ptr,
                               static_cast<int>(headers_text.size()), body_ptr, static_cast<int>(spec.body.size()),
                               static_cast<double>(spec.deadline.count()));
            aurora_fetch_free(url_ptr);
            aurora_fetch_free(method_ptr);
            aurora_fetch_free(headers_ptr);
            aurora_fetch_free(body_ptr);
        });
}

}  // namespace detail
}  // namespace aurora::net

#else  // !AURORA_PLATFORM_WASM

#include "aurora/core/result.h"

namespace aurora::net::detail {

auto fetch_leg_active() -> bool { return false; }

auto fetch_exchange(const FetchSpec & /*spec*/, const Cancellation & /*token*/) -> CallbackAwaiter<FetchOutcome> {
    // native 腿的占位形态：调用方以 `fetch_leg_active()` 分岔，本函数永不应被调用；
    // 保留一枚可编译的落点，让 fetch 腿的代码在 native 构建里也参与编译（防锈）。
    return CallbackAwaiter<FetchOutcome>(
        [](const CallbackAwaiter<FetchOutcome>::Sink &on_done) -> void {
            on_done(Result<FetchOutcome>{
                make_error(ErrorCode::NetPlatformUnavailable, "fetch exchange is only available on the wasm leg")});
        });
}

}  // namespace aurora::net::detail

#endif
