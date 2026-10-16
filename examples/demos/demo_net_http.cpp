// 网络 demo：声明式交付面（`net::http::bind_resource`）——`Resource` 三态直接驱动渲染，`reload()` 是真取消。
//
// 桩服务器是本进程内的回环监听口（只绑 127.0.0.1，临时端口），供一份自增序号的 JSON：于是本 demo 零真实远端、
// 零 DNS，断网也能跑。三处刻意留出的时序窗口对应三条手工判据（口径见 codespec/manual-test/23-net.md）：
//   * 写出应答之前先探一次「客户端还在不在」（一枚带超时的读），这段窗口既让 Loading 分支看得见，也让「再按一次
//     reload」来得及掐断前一次那条；
//   * `serving` 开关关掉后连接被直接收摊而不给应答，用于走 Failed 分支（传输失败才进错误侧）；
//   * 桩日志里的 "peer vanished before the answer" 是取消落到套接口上的现场证据——只丢结果不会有这一行。
//
// 取图面（`net::http::image_fetcher`）与流式面（`deliver_stream`）不在本 demo 里演示：前者的判据在
// `utest_net_http_binding`，后者需要可视的逐段落地，属 `04-image.md` 那条链路的后续。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/json.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/http.h"
#include "aurora/net/http_binding.h"
#include "aurora/net/http_client.h"
#include "aurora/net/policy.h"
#include "aurora/state/effect.h"
#include "aurora/state/resource.h"
#include "demo_common.h"

namespace {

namespace net_http = aurora::net::http;

/// @brief 一次取回并投影后的值：序号证明「这确实是本回合应答」，状态码证明「非 2xx 也算成功交付」。
struct Quote {
    std::string text;  ///< 应答里的引语文本
    std::int64_t seq{0};  ///< 桩服务器的自增序号
    std::uint16_t status{0};  ///< 应答状态码（原样带出，交渲染侧显示）
};

/// @brief 进程内回环 JSON 桩：一次一条连接，读到一个请求后按延迟供一份应答。
///
/// 刻意串行：一次只服务一条连接，于是「上一条被掐断」在日志里逐条可数。`close()` 会唤醒阻塞中的
/// `wait_readable`，故停机不需要额外的投递通道。
class Stub final {
  public:
    Stub() {
        auto bound = aurora::net::blocking::Listener::listen_loopback(0, 8);
        if (!bound) {
            AURORA_LOG_ERROR("net_demo", "stub did not start: " + bound.error().message);
            return;  // 起桩失败：端口留 0，客户端每次请求都落 Failed 分支，demo 仍可视
        }
        listener_ = std::move(bound.value());
        port_ = listener_.port();
        serving_.store(true);
        worker_ = std::thread([this]() -> void { serve(); });
    }

    /// @brief 停机：置停止位、关监听口并等在途线程收尾（重复调用安全）。
    ~Stub() { stop(); }

    Stub(const Stub &) = delete;
    auto operator=(const Stub &) -> Stub & = delete;
    // 后台线程持 `this`，搬走就等于让那枚线程跑到已迁走的对象上；本类型只是 demo 内的进程级桩，不参与搬运。
    Stub(Stub &&) = delete;
    auto operator=(Stub &&) -> Stub & = delete;

    /// @brief 实际监听端口。
    /// @return 系统分配的临时端口；起桩失败为 0。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief 当前是否在供答。
    /// @return 关掉后连接一建立就收摊而不给应答。
    [[nodiscard]] auto serving() const -> bool { return serving_.load(std::memory_order_acquire); }

    /// @brief 开 / 关供答（连接仍被接受，只是不回话）。
    /// @param on 是否供答。
    auto serving(bool on) -> void { serving_.store(on, std::memory_order_release); }

  private:
    /// @brief 接受循环：每 50 ms 醒一次检查停止位，停机经 `close()` 立刻打断等待。
    auto serve() -> void {
        while (running_.load(std::memory_order_acquire)) {
            const auto ready = listener_.wait_readable(std::chrono::milliseconds{50});
            if (!ready || !ready.value()) {
                continue;  // 纯超时或等待失败：都由停止位兜底，下一轮再看
            }
            auto accepted = listener_.accept();  // 非常量：`value()` 要交出可写的套接口
            if (accepted) {
                answer(accepted.value());
            }
        }
    }

    /// @brief 服务一条连接：读到请求后先探一次「客户端还在不在」，再决定供答还是直接收摊。
    /// @param socket 本条连接。
    auto answer(aurora::net::blocking::StreamSocket &socket) -> void {
        AURORA_LOG_INFO("net_demo", "accepted a connection");
        std::vector<char> buffer(2048);
        (void)socket.set_io_timeout(std::chrono::milliseconds{500});
        const auto read = socket.read_some(std::span<char>{buffer});
        if (!read || read.value() == 0) {
            AURORA_LOG_INFO("net_demo", "request vanished before the answer (that exchange was cancelled)");
            return;
        }
        // 这枚探针是整个 demo 的关键一步：写出应答之前再读一次，把「客户端还在等」与「客户端已经收摊」分成两种
        // 线上事实——还在等的只会让这次读超时，被取消的那一条交出一次真实的读端收摊（0 字节）。停顿因此不是
        // 睡出来的，而是「等一个答案该不该发出去」；只丢结果的取消永远不会让这里读到 0。
        (void)socket.set_io_timeout(std::chrono::milliseconds{AURORA_INFLIGHT_WINDOW_MS});
        const auto probe = socket.read_some(std::span<char>{buffer});
        const bool still_waiting =
            probe ? probe.value() > 0 : probe.error().code_enum == aurora::ErrorCode::NetRecvTimeout;
        if (!still_waiting) {
            AURORA_LOG_INFO("net_demo", "peer vanished before the answer (that exchange was cancelled)");
            return;
        }
        if (!serving()) {
            AURORA_LOG_INFO("net_demo", "serving halted: connection closed without an answer");
            return;  // 不给应答直接收摊：客户端读到 EOF，传输失败进 Failed 分支
        }
        AURORA_LOG_INFO("net_demo", "answered, closing the connection");
        const std::string body = R"({"seq": )" + std::to_string(++seq_) + R"(, "quote": "the socket is the truth"})";
        const std::string reply =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        (void)socket.write_all(std::span<const char>{reply});
    }

    /// @brief 停线程并关句柄。
    auto stop() -> void {
        running_.store(false);
        listener_.close();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    static constexpr int AURORA_INFLIGHT_WINDOW_MS = 1500;  ///< 应答前那次探针读的超时：Loading 可见、来得及中途取消

    aurora::net::blocking::Listener listener_{};  ///< 回环监听口（起桩失败即无效态）
    std::thread worker_;  ///< 接受循环所在的后台线程（默认构造即「不可连接」，起桩失败时也不必额外初始化）
    std::atomic<bool> running_{true};  ///< 接受循环的运行位
    std::atomic<bool> serving_{false};  ///< 是否供答（关=连上就收摊）
    std::atomic<int> seq_{0};  ///< 应答序号：每次供答自增，读数因此可比「是不是这一回合」
    std::uint16_t port_{0};  ///< 实际端口
};

/// @brief 应答投影：把 JSON 体折成 `Quote`；解析不成即这次交付失败（错误走 `Result` 的错误侧）。
/// @param response 物化应答（非 2xx 同样到这里，状态码原样带出）。
/// @return `Quote` 或解析类错误。
[[nodiscard]] auto project(const net_http::Response &response) -> aurora::Result<Quote> {
    const std::string text{response.body().begin(), response.body().end()};
    const auto parsed = aurora::json::parse(text);
    if (!parsed) {
        return aurora::Result<Quote>{parsed.error()};
    }
    const auto *seq = parsed.value().at("seq");
    const auto *quote = parsed.value().at("quote");
    const auto seq_value = seq != nullptr ? seq->as_int() : std::nullopt;
    const auto quote_value = quote != nullptr ? quote->as_string() : std::nullopt;
    if (!seq_value || !quote_value) {
        return aurora::Result<Quote>{
            aurora::make_error(aurora::ErrorCode::JsonTypeMismatch, "reply carries neither seq nor quote: " + text)};
    }
    return aurora::Result<Quote>{Quote{
        .text = std::string{*quote_value},
        .seq = *seq_value,
        .status = response.status(),
    }};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做 try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    Stub stub;

    net_http::Client client;
    aurora::net::NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};  // 明文只点名回环：缺省拒绝 `http://` 这条纪律不为 demo 关掉
    client.policy(std::move(permissive));

    const std::string url = "http://127.0.0.1:" + std::to_string(stub.port()) + "/quote";
    // 绑成资源即起手发送；此后每次 `reload()` 重新发一次，并掐掉前一次那条在途连接。
    auto resource = net_http::bind_resource<Quote>(client, net_http::Request::get(url), project);

    auto line = std::make_shared<aurora::State<aurora::LocalizedString>>(aurora::LocalizedString{"loading…"});
    // 同一份文本既上屏又进日志：手工验证不必守着窗口，stderr 里就是现场记录。
    const auto show = [line](const std::string &text) -> void {
        line->set(aurora::LocalizedString{text});
        AURORA_LOG_INFO("net_demo", text);
    };
    // 三分支读取即订阅：资源落定（或重发回到 Loading）时本 Effect 被重新运行，只刷这块文本。
    auto render = std::make_shared<aurora::Effect>([resource, show]() -> void {
        if (resource.phase() == aurora::ResourcePhase::Loading) {
            show("loading… (reload again within this pause to cut the exchange mid-flight)");
            return;
        }
        if (const auto *value = resource.value()) {
            show("seq=" + std::to_string(value->seq) + "  status=" + std::to_string(value->status) + "  " +
                 value->text);
            return;
        }
        const auto failure = resource.error();
        if (failure) {
            show("failed: " + failure->code + " - " + failure->message);
            return;
        }
        show("no outcome in this snapshot");  // 兜底：三态之外的读取顺序不该出现
    });
    render->run();  // 首帧求值：此后由资源状态自己驱动

    aurora::Button reload{aurora::ButtonProps{.label = aurora::LocalizedString{"reload"}}};
    reload.on_click = [resource]() mutable -> void { resource.reload(); };

    aurora::Button toggle{aurora::ButtonProps{.label = aurora::LocalizedString{"stop / resume serving"}}};
    toggle.on_click = [&stub]() -> void {
        stub.serving(!stub.serving());
        AURORA_LOG_INFO("net_demo", std::string{"serving = "} + (stub.serving() ? "on" : "off"));
    };

    aurora::Node root = Card{
        GradientTitle{"Net HTTP"},
        gap(8.0F),
        aurora::Text{aurora::LocalizedString{"Loopback JSON stub · Resource reads three branches · reload cuts the "
                                             "in-flight exchange"}},
        aurora::Text{aurora::TextProps{.content = aurora::Reactive{line}}},
        aurora::Row{std::move(reload), std::move(toggle)},
    };
    return run_demo(std::move(root), "Net HTTP · Aurora Demo", 620.0F, 320.0F);
}
