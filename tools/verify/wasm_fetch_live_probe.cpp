// Aurora verify — wasm fetch 引擎真跑探针（§16 第 37 项的验收腿）。
//
// 覆盖范围：wasm 腿的 HTTP 交换经浏览器 `fetch` 交付——本探针在 node ≥ 18 下真跑（全局 `fetch`
// 在场），分两相：
//   相一（真实 fetch，对端是编排脚本起的 node http 服务端，只绑 127.0.0.1、零出站）：
//     real_get_roundtrip                 GET 的状态码与体按服务端应答抵达（合成线上字节流的物化路径）
//     real_post_body_roundtrip           请求体整单交给 fetch 并被服务端收到（echo 应答回证）
//     real_custom_header_reaches_server  自定义请求头进入 fetch init 并抵达服务端
//     real_redirect_chain_followed_by_hand  `redirect: 'manual'` 不自动跟随：3xx 交回既有重定向链
//     real_set_cookie_harvest_and_replay `Set-Cookie` 经 `getSetCookie()` 收进 Jar、下一跳按目标域
//                                        重新附着（`credentials: 'omit'`，浏览器存储不参与）
//   相二（脚本化 fetch 替身 + 同步判据）：
//     capabilities_bits_measured                    NullEngine 能力位按实测改判
//     mock_network_failure_maps_to_connect_failed   fetch 拒绝 → `net-connect-failed`（浏览器不披露成因）
//     mock_deadline_abort_maps_to_http_timeout      截止到点 → `http-timeout`（connect 档折算
//                                                   `AbortController`）
//     mock_opaque_redirect_reports_platform_limit   浏览器不透明重定向 → `net-platform-unavailable`
//     streaming_request_body_reports_unavailable    流式请求体前置拒（不悄悄一次性物化）
//     open_stream_reports_unavailable               流式接收按能力位拒（`streaming` 恒 false）
//     cleartext_gate_applies_on_fetch_leg           明文闸与本腿无关，照常在 fetch 之前起作用
//
// 构建与运行（node 编排见同目录 `wasm_fetch_node_drive.mjs`：起服务端、驱动探针、给退出码）：
//   cmake --preset wasm
//   cmake --build build-wasm --target aurora_verify_wasm_fetch
//   node build-wasm/tools/verify/wasm_fetch_node_drive.mjs
//
// 退出码语义：0 = 全部判据过；1 = 有判据未过（逐项结果已打在 stdout）。
// 探针只绑回环，不构成出站流量；相二的替身与真 fetch 无关（脚本化，不打网络）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#ifndef AURORA_PLATFORM_WASM
#error "wasm_fetch_live_probe targets the wasm leg only; configure with the `wasm` preset (Emscripten)."
#endif

#include <emscripten.h>

#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_session.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/state/async.h"

// 以下 4 个为 EM_JS 在文件尾以 `extern "C"` 生成的外部链接函数；前向声明置于全局作用域（外部链接），
// 与 EM_JS 定义一致，避免匿名命名空间内的内部声明因「内部链接却无内部定义」触发 -Wundefined-internal。
extern "C" auto probe_notify_done() -> void;
extern "C" auto install_mock_fetch() -> void;
extern "C" auto script_mock_step(const char *url, const char *kind) -> void;
extern "C" auto probe_defer_js(std::intptr_t fn_ptr) -> void;

namespace {

using aurora::ErrorCode;
using aurora::Result;

/// @brief stdout 无前缀输出（verify 探针族的同一 emit 形态）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 单项用例的落账条目。
struct CaseResult {
    std::string name;  ///< 用例名
    bool ok = true;  ///< 全部判据成立与否
    std::string detail;  ///< 首个失败现场（成功为空）
};

/// @brief 探针运行态（台账 + 相位 + 在途计数）。函数局部静态：探针为程序级单线程工具，
/// 收进访问器即无文件级可变全局。
struct ProbeState {
    std::vector<CaseResult> results;  ///< 判据台账（用例并发完成，按槽位落账）
    int pending = 0;  ///< 在途用例数：归零即进入下一相（或收摊）
    int port = 0;  ///< node 服务端端口（编排脚本传入）
    int phase = 1;  ///< 当前相号：1 = 真实 fetch，2 = 替身与同步判据
};

/// @brief 探针运行态访问器。
auto probe_state() -> ProbeState & {
    static ProbeState state;
    return state;
}

// 前置声明（EM_JS 定义在文件尾，均为 `extern "C"` 外部链接；同名前向声明见文件顶部全局作用域）。
auto start_phase_two() -> void;

/// @brief 判据原语：把一次断言记进本用例的台账条目（首个失败现场保留）。
auto require(CaseResult &entry, bool ok, const std::string &detail) -> void {
    if (!ok && entry.ok) {
        entry.ok = false;
        entry.detail = detail;
    }
}

/// @brief 以服务端路由拼出本探针的回环 URL。
auto probe_url(std::string_view route) -> std::string {
    return "http://127.0.0.1:" + std::to_string(probe_state().port) + std::string{route};
}

/// @brief 应答体转文本（判据可比对）。
auto body_text(const Result<aurora::net::http::Response> &outcome) -> std::string {
    if (!outcome.ok()) {
        return {};
    }
    const auto &bytes = outcome.value().body();
    return std::string{bytes.data(), bytes.size()};
}

/// @brief 在途计数归零即开下一相（或通知编排脚本收摊）。
auto case_count_down() -> void {
    if (--probe_state().pending != 0) {
        return;
    }
    if (probe_state().phase == 1) {
        probe_state().phase = 2;
        start_phase_two();
        return;
    }
    probe_notify_done();
}

/// @brief 开一个新台账条目并交出槽位（判据经 require(entry, …) 落账）。
auto open_case(const std::string &name) -> std::size_t {
    probe_state().results.push_back(CaseResult{.name = name, .ok = true, .detail = {}});
    return probe_state().results.size() - 1;
}

/// @brief 发起一个异步用例。
///
/// 客户端按 `shared_ptr` 进完成观察者：协程帧持客户端的 `this`，判据在完成那刻同步跑，客户端须
/// 活过整个交换——探针的用例在 `start_phase_real` 返回之后才陆续收尾，栈上对象撑不住。
/// @param name 用例名。
/// @param client 客户端（探针全程持有一份 shared_ptr，本参数共享所有权）。
/// @param request 请求。
/// @param judge 判据体：拿一次交换的结局与台账槽位落账（可链后续交换）。
auto start_case(const std::string &name, std::shared_ptr<const aurora::net::http::Client> client,
                aurora::net::http::Request request,
                std::function<void(const Result<aurora::net::http::Response> &, CaseResult &)> judge) -> void {
    const auto slot = open_case(name);
    ++probe_state().pending;
    auto task = client->send(std::move(request));
    // `CoroTask<Result<Response>>::on_done` 的钩子载荷是 `Result<Result<Response>>`（T 本身已是
    // Result 形态）：剥一层再交 judge——帧收尾时结果槽必有值（promise 的收尾契约）。
    task.on_done([slot, client = std::move(client), judge = std::move(judge)](
                     const Result<Result<aurora::net::http::Response>> &wrapped) mutable -> void {
        judge(wrapped.value(), probe_state().results[slot]);
        case_count_down();
    });
}

/// @brief 为探针客户端放行回环明文（相一对端是 node http 服务端；缺省策略拒未点名明文目标）。
auto allow_loopback(aurora::net::http::Client &client) -> void {
    aurora::net::NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(permissive);
}

// ── 相一：真实 fetch（对端 = node http 服务端）。

auto start_phase_real() -> void {
    // GET 往返：状态码 + 体。
    auto getter = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*getter);
    start_case("real_get_roundtrip", getter, aurora::net::http::Request::get(probe_url("/hello")),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, outcome.ok() && outcome.value().status() == 200,
                           outcome.ok() ? "status != 200" : outcome.error().message);
                   require(entry, body_text(outcome) == "hello fetch leg", "body mismatch: " + body_text(outcome));
               });

    // POST 体往返：服务端 echo 回证请求体整单抵达。
    auto poster = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*poster);
    start_case("real_post_body_roundtrip", poster,
               aurora::net::http::Request::post(probe_url("/echo")).body(std::vector<char>{'p', 'i', 'n', 'g'}),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, outcome.ok() && body_text(outcome) == "ping", "echo mismatch: " + body_text(outcome));
               });

    // 自定义请求头抵达服务端。
    auto headered = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*headered);
    start_case("real_custom_header_reaches_server", headered,
               aurora::net::http::Request::get(probe_url("/marker")).set_header("x-probe-marker", "fetch-leg"),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, outcome.ok() && body_text(outcome) == "fetch-leg",
                           "marker mismatch: " + body_text(outcome));
               });

    // 重定向链：/hop1 → 302 → /next → 200 landed。
    auto follower = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*follower);
    start_case("real_redirect_chain_followed_by_hand", follower, aurora::net::http::Request::get(probe_url("/hop1")),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, outcome.ok() && outcome.value().status() == 200 && body_text(outcome) == "landed",
                           "final landing mismatch: " + body_text(outcome));
               });

    // Jar 往返：第一跳收 Set-Cookie，第二跳（同一 Session）按目标域重附 Cookie。两跳串行：
    // 第二跳由第一跳的完成观察者发起；会话随观察者闭包活过两跳。
    const auto slot = open_case("real_set_cookie_harvest_and_replay");
    ++probe_state().pending;
    auto session = std::make_shared<aurora::net::http::Session>();
    allow_loopback(session->client());
    auto first = session->client().send(aurora::net::http::Request::get(probe_url("/set-cookie")));
    first.on_done([slot, session](const Result<Result<aurora::net::http::Response>> &wrapped) mutable -> void {
        const auto &outcome = wrapped.value();  // 钩子载荷剥一层：内层才是 Result<Response>
        require(probe_state().results[slot], outcome.ok() && outcome.value().status() == 200, "set-cookie hop failed");
        auto second = session->client().send(aurora::net::http::Request::get(probe_url("/read-cookie")));
        second.on_done(
            [slot, session](const Result<Result<aurora::net::http::Response>> &inner_wrapped) mutable -> void {
                const auto text = body_text(inner_wrapped.value());
                require(probe_state().results[slot], inner_wrapped.ok() && text.find("probe=1") != std::string::npos,
                        "cookie did not come back: " + text);
                case_count_down();
            });
    });
}

// ── 相二：脚本化 fetch 替身 + 同步判据。

/// @brief 只数入口次数的接收宿主（流式拒判据只问「进没进闸」，不需要载荷）。
class CountingSink final : public aurora::net::http::DataSink {
  public:
    auto on_head([[maybe_unused]] const aurora::net::http::Response &head) -> aurora::net::http::SinkFlow override {
        ++entries;
        return aurora::net::http::SinkFlow::Continue;
    }
    auto on_data([[maybe_unused]] std::span<const char> bytes) -> aurora::net::http::SinkFlow override {
        ++entries;
        return aurora::net::http::SinkFlow::Continue;
    }
    auto on_end() -> void override { ++entries; }

    int entries = 0;  ///< 入口被调次数（判据预期恒 0）
};

/// @brief 流式请求体的最小替身（只用于「前置拒」判据，体永远不会被读）。
class FixedSource final : public aurora::net::http::BodySource {
  public:
    [[nodiscard]] auto total_size() const -> std::optional<std::uint64_t> override { return 4; }
    auto restart() -> bool override { return true; }
    auto next_chunk() -> Result<std::vector<char>> override {
        if (done_) {
            return Result<std::vector<char>>{std::vector<char>{}};
        }
        done_ = true;
        return Result<std::vector<char>>{std::vector<char>{'b', 'o', 'd', 'y'}};
    }

  private:
    bool done_ = false;  ///< 段游标
};

auto start_phase_two() -> void {
    // 替身就位：按 URL 脚本化应答 / 拒绝 / 挂起 / 不透明应答。
    install_mock_fetch();

    // 能力位按实测改判（同步判据）：custom_headers 真（fetch 收自定义头，预检失败以错误形态抵达）、
    // streaming 假（本切片不消费 ReadableStream）、tcp 假（本腿无 fd 语义）。
    {
        const auto slot = open_case("capabilities_bits_measured");
        const auto caps = aurora::net::Engine::instance().capabilities();
        require(probe_state().results[slot], caps.custom_headers, "custom_headers should be true on the wasm leg");
        require(probe_state().results[slot], !caps.streaming, "streaming should be false on the wasm leg");
        require(probe_state().results[slot], !caps.tcp, "tcp should be false on the wasm leg");
    }

    // fetch 拒绝 → net-connect-failed。
    auto failer = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*failer);
    script_mock_step("https://mock.test/fail", "reject");
    start_case("mock_network_failure_maps_to_connect_failed", failer,
               aurora::net::http::Request::get("https://mock.test/fail"),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, !outcome.ok() && outcome.error().code_enum == ErrorCode::NetConnectFailed,
                           outcome.ok() ? "unexpected success" : outcome.error().message);
               });

    // 截止到点 → http-timeout（connect 档折算 AbortController）。
    auto waiter = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*waiter);
    script_mock_step("https://mock.test/hang", "hang");
    start_case("mock_deadline_abort_maps_to_http_timeout", waiter,
               aurora::net::http::Request::get("https://mock.test/hang")
                   .timeouts(aurora::net::http::Timeouts{.connect = std::chrono::milliseconds{60}}),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, !outcome.ok() && outcome.error().code_enum == ErrorCode::HttpTimeout,
                           outcome.ok() ? "unexpected success" : outcome.error().message);
               });

    // 浏览器不透明重定向 → net-platform-unavailable。
    auto opaque = std::make_shared<aurora::net::http::Client>();
    allow_loopback(*opaque);
    script_mock_step("https://mock.test/opaque", "opaque");
    start_case("mock_opaque_redirect_reports_platform_limit", opaque,
               aurora::net::http::Request::get("https://mock.test/opaque"),
               [](const Result<aurora::net::http::Response> &outcome, CaseResult &entry) -> void {
                   require(entry, !outcome.ok() && outcome.error().code_enum == ErrorCode::NetPlatformUnavailable,
                           outcome.ok() ? "unexpected success" : outcome.error().message);
               });

    // 流式请求体前置拒（发起侧在 fetch 之前就拒，不发起任何交换；判据同步落账）。
    {
        const auto slot = open_case("streaming_request_body_reports_unavailable");
        auto client = std::make_shared<aurora::net::http::Client>();
        allow_loopback(*client);
        auto source = std::make_shared<FixedSource>();
        auto task = client->send(aurora::net::http::Request::post("https://mock.test/upload").body(*source));
        task.on_done(
            [slot, client, source](const Result<Result<aurora::net::http::Response>> &wrapped) mutable -> void {
                require(probe_state().results[slot], !wrapped.ok() || !wrapped.value().ok(),
                        "unexpected success on the streaming-body request");
                if (!wrapped.ok()) {
                    require(probe_state().results[slot], false, wrapped.error().message);
                } else {
                    const auto &outcome = wrapped.value();
                    require(probe_state().results[slot],
                            !outcome.ok() && outcome.error().code_enum == ErrorCode::NetPlatformUnavailable,
                            outcome.ok() ? "unexpected success" : outcome.error().message);
                }
                case_count_down();
            });
    }

    // 流式接收按能力位拒（open_stream 的既有闸；判据同步落账，宿主不得被调过一次）。
    {
        const auto slot = open_case("open_stream_reports_unavailable");
        auto client = std::make_shared<aurora::net::http::Client>();
        allow_loopback(*client);
        auto sink = std::make_shared<CountingSink>();
        auto task = client->open_stream(aurora::net::http::Request::get("https://mock.test/stream"), *sink);
        task.on_done([slot, client,
                      sink](const Result<Result<aurora::net::http::StreamOutcome>> &wrapped) mutable -> void {
            require(probe_state().results[slot], !wrapped.ok() || !wrapped.value().ok(),
                    "unexpected success on the streaming response");
            if (!wrapped.ok()) {
                require(probe_state().results[slot], false, wrapped.error().message);
            } else {
                const auto &outcome = wrapped.value();
                require(probe_state().results[slot],
                        !outcome.ok() && outcome.error().code_enum == ErrorCode::NetPlatformUnavailable,
                        outcome.ok() ? "unexpected success" : outcome.error().message);
            }
            require(probe_state().results[slot], sink->entries == 0, "the sink was invoked before the capability gate");
            case_count_down();
        });
    }

    // 明文闸照常在 fetch 之前起作用（缺省策略拒未点名明文目标；判据同步落账）。
    {
        const auto slot = open_case("cleartext_gate_applies_on_fetch_leg");
        auto client = std::make_shared<aurora::net::http::Client>();
        auto task = client->send(aurora::net::http::Request::get(probe_url("/hello")));
        task.on_done([slot, client](const Result<Result<aurora::net::http::Response>> &wrapped) mutable -> void {
            require(probe_state().results[slot], !wrapped.ok() || !wrapped.value().ok(), "unexpected success");
            if (!wrapped.ok()) {
                require(probe_state().results[slot], false, wrapped.error().message);
            } else {
                const auto &outcome = wrapped.value();
                require(probe_state().results[slot],
                        !outcome.ok() && outcome.error().code_enum == ErrorCode::NetPolicyViolated,
                        outcome.ok() ? "unexpected success" : outcome.error().message);
            }
            case_count_down();
        });
    }
}

// @brief 收摊通知：probe_state().pending 归零时唤回编排脚本（resolve 那枚 promise）。
// clang-format off
EM_JS(void, probe_notify_done, (), {
    if (typeof globalThis.__auroraFetchProbeResolve === 'function') {
        globalThis.__auroraFetchProbeResolve();
    }
});
// clang-format on

// @brief 装脚本化 fetch 替身：按 URL 查表（reject = 拒绝 / hang = 等信号打断 / opaque = 状态 0 /
//        常规 = 状态 + 头 + 体文本）。与相一的真实 fetch 互不污染（相一结束后才装）。
// clang-format off
EM_JS(void, install_mock_fetch, (), {
    globalThis.__auroraFetchMock = {steps: {}, log: []};
    globalThis.fetch = (url, init) => {
        const target = String(url);
        globalThis.__auroraFetchMock.log.push({
            url: target,
            method: init && init.method ? init.method : 'GET',
            headers: init && init.headers ? init.headers : {},
            body_bytes: init && init.body ? init.body.length : 0,
        });
        const step = globalThis.__auroraFetchMock.steps[target];
        if (step === undefined) {
            return Promise.reject(new TypeError('mock fetch: no script for ' + target));
        }
        if (step === 'reject') {
            return Promise.reject(new TypeError('mock fetch: network failure'));
        }
        if (step === 'hang') {
            return new Promise((resolve, reject) => {
                const signal = init && init.signal;
                if (signal) {
                    signal.addEventListener('abort', () => reject(new DOMException('The operation was aborted.', 'AbortError')));
                }
            });
        }
        if (step === 'opaque') {
            return Promise.resolve({
                status: 0,
                statusText: "",
                headers: new Headers(),
                arrayBuffer: async () => new ArrayBuffer(0),
            });
        }
        const headers = new Headers();
        for (const pair of step.headers || []) {
            headers.append(pair[0], pair[1]);
        }
        const bytes = step.body ? new TextEncoder().encode(step.body) : new Uint8Array(0);
        return Promise.resolve({
            status: step.status,
            statusText: step.reason || "",
            headers,
            arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
        });
    };
});
// clang-format on

// @brief 给一枚 URL 登记替身剧本（kind ∈ reject / hang / opaque）。
// clang-format off
EM_JS(void, script_mock_step, (const char *url, const char *kind), {
    globalThis.__auroraFetchMock.steps[UTF8ToString(url)] = UTF8ToString(kind);
});
// clang-format on

// @brief 把一枚主线程续体推迟到下一个宏任务（`setTimeout` 0）。
// clang-format off
EM_JS(void, probe_defer_js, (std::intptr_t fn_ptr), {
    setTimeout(() => { _aurora_probe_deferred_run(fn_ptr); }, 0);
});
// clang-format on

// @brief 推迟续体的回收口：调用并释放（EM_JS 只传地址，所有权归本函数）。
//        地址 0 是哨兵「只排水、不回收」——值班租约的 wake 用它把一次排空推迟到本宏任务。
extern "C" EMSCRIPTEN_KEEPALIVE void aurora_probe_deferred_run(std::intptr_t fn_ptr) {
    if (fn_ptr == 0) {
        aurora_probe_drain_duty();
        return;
    }
    // 跨语言只传地址（reinterpret_cast 无类型安全替代）；⚠️ 该语句被折行，行尾式豁免会落在第二行
    // 而静默失效，故用区间式豁免覆盖整条语句。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    auto *holder = reinterpret_cast<std::function<void()> *>(fn_ptr);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    (*holder)();
    delete holder;
}

}  // namespace

extern "C" {

// @brief 排水值班队列：take_batch 取出全部滞留续体，逐个交给 JS 宏任务执行。
extern "C" EMSCRIPTEN_KEEPALIVE void aurora_probe_drain_duty() {
    for (auto &fn : aurora::MainQueue::take_batch()) {
        if (!fn) {
            continue;
        }
        auto *holder = new std::function<void()>(std::move(fn));
        // 同上：跨语言只传地址；语句折行 ⇒ 行尾式豁免会落到第二行失效，改用区间式豁免。
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        probe_defer_js(reinterpret_cast<std::intptr_t>(holder));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }
}

/// @brief 探针入口（由编排脚本在模块就绪后调用；`-sINVOKE_RUN=0`，main 不自启）。
///
/// 领进程级唯一队列的值班租约再开跑：结算回调里的续体（重定向的下一跳、Jar 的第二跳）
/// 被推迟到下一个宏任务
/// 发起——node/undici 对「同一 tick 内嵌套发起的请求」在退出收尾上有一枚 uv_async 断言（进程级
/// 噪声，与判据无关），新鲜回合发起即避开；这也与真实宿主的姿态同构（本探针就是值班循环，
/// 续体回全局队列、经宏任务排空）。`wake` 即「把排水本身推迟到下一个宏任务」。
/// @param port node 服务端端口（相一的回环对端）。
EMSCRIPTEN_KEEPALIVE void aurora_fetch_probe_run(int port) {
    probe_state().port = port;
    static auto duty = aurora::MainQueue::on_duty([]() -> void { probe_defer_js(0); });
    start_phase_real();
}

/// @brief 判据台账读出口：返回未过的用例数（0 = 全过）并逐项打点 stdout。
EMSCRIPTEN_KEEPALIVE int aurora_fetch_probe_report() {
    int failures = 0;
    for (const auto &entry : probe_state().results) {
        emit(entry.name + ": " + (entry.ok ? "PASS" : "FAIL " + entry.detail));
        failures += entry.ok ? 0 : 1;
    }
    return failures;
}

}  // extern "C"

// 探针入口只有一句告示输出：名义抛点是 `std::string` 构造（bad_alloc），而 `main` 无法上报——
// 换成 try/catch 也只是把失败吞掉，故就地豁免。
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    emit("wasm_fetch_live_probe: waiting for the node driver (aurora_fetch_probe_run)");
    return 0;
}
