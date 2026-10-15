/// 测试类型: unit
/// 目标单元: include/aurora/net/http_client.h + src/aurora/net/http_client.cpp
/// 测试说明: HTTP 发起侧 send 核心闭环的离线回归（CHG-010 §6 / §12）。以 ScriptedPeer 在回环上按脚本
///           应答真实字节，覆盖 Content-Length 与 Chunked 两种分帧的物化、302 重定向跟随（跨到第二个对端）
///           与超限落 `http-redirect-limit`；整请求超时用 FakeEngine 脚本化时钟（连上但静默的对端 + 推进到
///           时限）验 `http-timeout`。内容编码接入（§12「内容编码」的 HTTP 侧）验 `Accept-Encoding` 由
///           缺省补齐（调用方已自带则原样保留、不覆盖不重复追加）、已声明环节的透明还原（gzip 与 `deflate`
///           的 zlib→裸流回落、`gzip, deflate` 链反序解）、`max_decompressed_bytes()` 越限落
///           `http-body-too-large` 且 detail 点名该旋钮、`br` 落 `http-content-encoding-unsupported`、HEAD
///           应答无体时不校验编码名；能力开关缺席的腿验「连宣告都不发」并把编码应答落 unsupported。进度节流
///           （§12「进度节流」）验上传按 64 KiB 分块推进且末值恰一次、注入帧号源后每方向收敛到「首发 + 末值
///           两笔」、无请求体不发上传、chunked 下载把总量如实报成未知，并以对端收到的请求全文证明「挂了回调
///           才分块写、线上字节序列与一次写完逐字节相同」以及「没挂回调者零行为改动」。重试（§12「重试策略」）
///           在虚拟时钟下逐值验：退避确实在 `base` 之后才回头、`Retry-After` 的秒数压过算式并被 `max_backoff`
///           夹住、尝试耗尽交出的仍是最后那次 503 应答而不是新错误、非幂等方法与表上标 `retryable=false` 的失败
///           一律不重放（`accepts()` 恰一次），协议档失败也不重放；`Timeouts::total` 逐尝试重新计时（三扇窗口
///           跑满 150 ms 虚拟时长）而 `RetryPolicy::budget` 是跨尝试的一堵墙（到点改报 `http-timeout` 并点名
///           budget）；越界的 `RetryPolicy` 起手即落 `http-request-invalid`，一条连接都不发起。缓存（§12「条件
///           请求与缓存校验」）验开关兑现的那一条：新鲜副本命中时一条连接都不发起（`accepts()` 不变）且
///           `from_cache()` 为真，过期条目带着表里的校验子先问对端、拿到 304 后交出的是并过头部的已存 200
///           （调用方看不见那枚中间状态），刚验证过的一份再度获得新鲜度；响应侧 `no-store`、请求侧
///           `Request::no_cache()`（既不读也不写）、提交类方法的应答、开关未开这四条各自把交换挡回线上。
///           这一组用真实时钟——新鲜度算的是 `system_clock` 的账，推虚拟时钟推不动它，而「触没触网」由对端的
///           `accepts()` 直接读出。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_client {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::http::CacheOptions;
using aurora::net::http::Client;
using aurora::net::http::error_class_of;
using aurora::net::http::HttpErrorClass;
using aurora::net::http::Progress;
using aurora::net::http::ProgressPhase;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::RetryPolicy;
using aurora::net::http::Timeouts;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::silent;
using aurora::test::scripts::write_then_half_close;
using aurora::test::scripts::write_then_hold;
using aurora::test::scripts::write_then_reset;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 取协程结果的内存活 Response 的 Result 层（协程本身不抛，外层恒成功）。
[[nodiscard]] auto inner(CoroTask<Result<Response>> &task) -> Result<Response> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 把无符号字节表转成消息体缓冲（压缩字节常 ≥ 0x80，写成 char 字面量会踩实现定义转换）。
/// @tparam N 表长。
/// @param table 字节表。
/// @return 等长缓冲。
template <std::size_t N>
[[nodiscard]] auto bytes(const unsigned char (&table)[N]) -> std::vector<char> {
    std::vector<char> out;
    out.reserve(N);
    for (const unsigned char unit : table) {
        out.push_back(static_cast<char>(unit));
    }
    return out;
}

// `hello aurora` 的金样包装：gzip（RFC 1952）、裸 DEFLATE（RFC 1951）、以及先 gzip 再 zlib 的两层链。
const unsigned char AURORA_GZIP_HELLO[] = {31, 139, 8,  0,   0,  0,  0, 0, 2,   255, 203, 72,  205, 201, 201, 87,
                                           72, 44,  45, 202, 47, 74, 4, 0, 214, 230, 105, 148, 12,  0,   0,   0};
#ifdef AURORA_ENABLE_COMPRESSION
const unsigned char AURORA_RAW_HELLO[] = {203, 72, 205, 201, 201, 87, 72, 44, 45, 202, 47, 74, 4, 0};
const unsigned char AURORA_GZIP_THEN_ZLIB[] = {120, 156, 147, 239, 230, 96,  0,   1,  166, 255, 167, 61,  206,
                                               158, 60,  25,  238, 161, 163, 123, 74, 223, 139, 133, 225, 218,
                                               179, 204, 41,  60,  12,  12,  12,  0,  162, 234, 10,  42};
#endif

/// @brief 拼一份 `Content-Length` 分帧的 200 应答：头部字段 + 原样字节体（压缩字节按长度分帧最省事）。
/// @param fields 头部字段行（每行以 `\\r\\n` 结尾，可空）。
/// @param body 消息体字节。
/// @return 可直接喂给对端脚本的应答字节。
[[nodiscard]] auto length_framed(std::string_view fields, const std::vector<char> &body) -> std::string {
    std::string out = "HTTP/1.1 200 OK\r\n";
    out += fields;
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out.append(body.data(), body.size());
    return out;
}

/// @brief 把一段文本转成 `length_framed()` 要的字节体（缓存用例的载荷都是可读短词，用不上字节表）。
/// @param text 文本。
/// @return 等长缓冲。
[[nodiscard]] auto body_of(std::string_view text) -> std::vector<char> {
    return std::vector<char>{text.begin(), text.end()};
}

/// @brief 取出已交付应答的体（缓存判据问的是「交出的到底是哪一份载荷」，读原始字节最直白）。
/// @param response 应答。
/// @return 体的文本副本。
[[nodiscard]] auto as_body_text(const Response &response) -> std::string {
    const auto &body = response.body();
    return std::string{body.begin(), body.end()};
}

/// @brief 记录被写出的请求字节、随后按给定应答收摊的对端脚本。
///
/// 宣告 `Accept-Encoding` 与「不覆盖调用方自带字段」的判据在**写出的字节**上，光看应答侧结果证明不了；
/// 故这里把请求头部块留一份。脚本线程写、用例线程读，用互斥量交接（用例在 send 结束后才取，天然错开，
/// 锁只为把这件事写成可见的同步而不是默认成立）。
/// @param recorded 交接缓冲（脚本线程追加）。
/// @param mutex_for_recorded 交接缓冲的锁。
/// @param payload 读满一个头部块后写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_then_reply(std::string &recorded, std::mutex &guard, std::string payload) -> PeerScript {
    return [&recorded, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                             PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;  // 头部块到位即应答（GET 无体，再读只会挂到超时）
            }
        }
        {
            const std::scoped_lock held(guard);
            recorded = seen;
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 取记录到的请求字节（脚本线程写入，用例线程在 send 收尾后读取）。
/// @param recorded 交接缓冲。
/// @param guard 交接缓冲的锁。
/// @return 请求字节副本。
[[nodiscard]] auto recorded_request(const std::string &recorded, std::mutex &guard) -> std::string {
    const std::scoped_lock held(guard);
    return recorded;
}

/// @brief 统计子串出现次数：宣告字段「只补一次、不重复追加」的判据要用它。
/// @param haystack 待查文本。
/// @param needle 子串（大小写敏感，测试里的拼写都是发送侧原样）。
/// @return 出现次数。
[[nodiscard]] auto count_occurrences(std::string_view haystack, std::string_view needle) -> std::size_t {
    std::size_t hits = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++hits;
    }
    return hits;
}

/// @brief 把文本里的字节按 ASCII 小写化（比对头部字段名时不依赖大小写）。
/// @param text 原文。
/// @return 小写副本。
[[nodiscard]] auto lowered(std::string_view text) -> std::string {
    std::string out{text};
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(static_cast<unsigned char>(c) + 32);
        }
    }
    return out;
}

/// @brief 取出某一方向的发射序列（上传与下载共用一枚回调，频次判据必须分方向看）。
/// @param seen 全部发射。
/// @param phase 方向。
/// @return 该方向的发射副本（保持发射次序）。
[[nodiscard]] auto phases_of(const std::vector<Progress> &seen, ProgressPhase phase) -> std::vector<Progress> {
    std::vector<Progress> out;
    for (const Progress &progress : seen) {
        if (progress.phase == phase) {
            out.push_back(progress);
        }
    }
    return out;
}

/// @brief 数一带「收摊」事实的发射（末值恒发一次的判据：不多发也不缺发）。
/// @param seen 全部发射。
/// @return `finished` 为真的枚数。
[[nodiscard]] auto settled_count(const std::vector<Progress> &seen) -> std::size_t {
    std::size_t hits = 0;
    for (const Progress &progress : seen) {
        if (progress.finished) {
            ++hits;
        }
    }
    return hits;
}

/// @brief 该方向的累计字节是否单调不降（合流不能把进度倒回去，倒回去的进度条是坏掉的进度条）。
/// @param seen 单方向发射序列。
/// @return 单调不降为真。
[[nodiscard]] auto monotonic(const std::vector<Progress> &seen) -> bool {
    for (std::size_t i = 1; i < seen.size(); ++i) {
        if (seen.at(i).transferred_bytes < seen.at(i - 1).transferred_bytes) {
            return false;
        }
    }
    return true;
}

/// @brief 从已收到的请求字节里取 `Content-Length`（大小写不敏感；缺失或畸形一律按「无体」处理）。
/// @param head_block 到头部块结尾（含 `\r\n\r\n`）为止的文本。
/// @return 请求自己声明的消息体字节数。
[[nodiscard]] auto declared_body_bytes(std::string_view head_block) -> std::size_t {
    const std::string folded = lowered(head_block);
    constexpr std::string_view key = "content-length:";
    const auto at = folded.find(std::string{key});
    if (at == std::string::npos) {
        return 0;
    }
    std::size_t pos = at + key.size();
    while (pos < folded.size() && (folded[pos] == ' ' || folded[pos] == '\t')) {
        ++pos;
    }
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (pos + digits < folded.size()) {
        const char digit = folded[pos + digits];
        if (digit < '0' || digit > '9') {
            break;  // 值后面的 `\r` 是分隔符而非畸形：取已读到的数字即算成功
        }
        value = (value * 10) + static_cast<std::uint64_t>(digit - '0');
        ++digits;
    }
    if (digits == 0) {
        return 0;
    }
    return static_cast<std::size_t>(value);
}

/// @brief 读满一整条请求（长度取请求自己声明的 `Content-Length`）、把收到的字节留档，然后应答。
///
/// 「挂了回调才分块写、线上字节序列与一次写完逐字节相同」这条只能看对端收到的字节：分块影响的是本库内部
/// 的完成次数，对端无从分辨。期望长度必须从请求头里读而不是用例另算——回环端口是五位数字，任何按占位 URL
/// 算出的头部长度都与真实请求差几位，差出来的那段会让脚本一直读到 I/O 超时。脚本线程写、用例线程读，用
/// 互斥量交接。
/// @param payload 读满请求后写出的应答字节。
/// @param recorded 交接缓冲（脚本线程写入）。
/// @param guard 交接缓冲的锁。
/// @return 对端脚本。
[[nodiscard]] auto record_request_then_reply(std::string payload, std::string &recorded, std::mutex &guard)
    -> PeerScript {
    return [&recorded, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                             PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{500});
        std::vector<char> buffer(4096);
        std::string seen;
        std::size_t want = 0;
        bool sized = false;
        while (!stop.load() && (!sized || seen.size() < want)) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (!sized) {
                const auto end = seen.find("\r\n\r\n");
                if (end != std::string::npos) {
                    want = end + 4 + declared_body_bytes(std::string_view{seen}.substr(0, end + 4));
                    sized = true;
                }
            }
        }
        {
            const std::scoped_lock held(guard);
            recorded = seen;
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 拼一份「服务器明说现在不行」的 503 应答（重试环的状态码分岔只认这一族）。
/// @param fields 额外的头部字段（每行以 `\\r\\n` 结尾，可空；`Retry-After` 由此注入）。
/// @return 带 4 字节体 `busy` 的应答字节。
[[nodiscard]] auto service_busy(std::string_view fields = {}) -> std::string {
    return "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n" + std::string{fields} + "\r\nbusy";
}

/// @brief 拼一份 200 应答：重试终于成功那一侧的对照。
/// @return 带 2 字节体 `ok` 的应答字节。
[[nodiscard]] auto service_ok() -> std::string { return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"; }

/// @brief 在脚本化时钟下把 `send` 泵到收尾：每轮推 10 ms 虚拟时钟、再给宿主一次跑事件的机会。
///
/// 退避等待登记在 `post_after` 上，不推时钟就永远不会到期；轮数上限把「排空不了」变成一次可见失败而不是挂死。
/// 调用一律配 `AURORA_TEST_REQUIRE`：`CoroTask::result()` 在未收尾时读的是帧外内存，非致命的 CHECK 会继续
/// 走到取结果那一步，于是一场「没排空」显示为一次段错误而不是一条失败判据。
/// @param engine 脚本化时钟。
/// @param task 待收尾的协程。
/// @param rounds 轮数上限。
/// @return 协程已收尾为真。
[[nodiscard]] auto pump_send(FakeEngine &engine, CoroTask<Result<Response>> &task, int rounds = 40) -> bool {
    for (int round = 0; round < rounds && !task.is_done(); ++round) {
        (void)engine.advance(std::chrono::milliseconds{10});
        aurora::test::net::pump_idle(std::chrono::milliseconds{5});
    }
    return task.is_done();
}

/// @brief 在真实时钟下完成一次交换并交出结果（缓存套件的判据用不上替身时钟）。
///
/// 新鲜度算的是 `system_clock` 的账：推虚拟时钟推不动它，而本套件要问的两件事——「触没触网」（对端的
/// `accepts()`）与「载荷出自哪一份」（`from_cache()` 加体内容）——都不依赖时序精度，用真实时钟反而少一层
/// 「时钟是脚本推的」的间接性。`HostDriven` 由用例自己声明，本函数只负责泵与取结果。
/// @param client 客户端。
/// @param request 请求（按值接收以适配 `send()` 的形参形态）。
/// @return 应答的 Result 层（协程未收尾时由 `inner()` 的 REQUIRE 收场）。
[[nodiscard]] auto send_and_drive(const Client &client, Request request) -> Result<Response> {
    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    return inner(task);
}

/// @brief 时间与宿主一起推进到谓词成立，或虚拟时长用完；返回时 `engine.now()` 即「实际烧掉的虚拟时长」。
///
/// 为什么不用 `drive_until` 配一次 `advance`：对端「接受连接」发生在客户读出应答**之前**，先把时钟推到位
/// 再去等第二次接受，等于拿「退避还没登记」那一瞬当判据——单机侥幸通过、换台机器就红（本仓实测过这种红）。
/// 本函数每步先推进再泵宿主，登记与否都兜得住；「等了多久」由返回时已推进的虚拟时长读出，而不是靠窗口观察。
/// @param engine 脚本化时钟。
/// @param pred 目标条件。
/// @param virtual_ms 最多推进的虚拟时长（按 10 ms 步进）。
/// @return 谓词成立为真。
[[nodiscard]] auto pump_until(FakeEngine &engine, const std::function<bool()> &pred, std::int64_t virtual_ms) -> bool {
    for (std::int64_t spent = 0; spent < virtual_ms; spent += 10) {
        (void)engine.advance(std::chrono::milliseconds{10});
        aurora::test::net::pump_idle(std::chrono::milliseconds{2});
        if (pred()) {
            return true;
        }
    }
    return pred();
}

/// @brief 错误 detail 里是否点名了某一枚旋钮（同一条 `http-timeout` 在三档时限之间仍可分辨）。
/// @param haystack 错误消息原文。
/// @param needle 待找的字段名。
/// @return 找到为真。
[[nodiscard]] auto names_in(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

AURORA_TEST_CASE(send_materializes_content_length) {
    Client client;
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello");
}

AURORA_TEST_CASE(send_materializes_chunked_body) {
    Client client;
    ScriptedPeer peer(reply_after_reading(
        1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello world");
}

AURORA_TEST_CASE(send_follows_redirect_across_peers) {
    Client client;
    ScriptedPeer target(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nZ"));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer origin(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(origin.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(origin.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "Z");
    AURORA_TEST_CHECK_EQ(origin.accepts(), 1);
    AURORA_TEST_CHECK_EQ(target.accepts(), 1);
}

AURORA_TEST_CASE(send_stops_at_redirect_limit) {
    Client client;
    // 每跳都回 302 指向自身路径：跟随必然撞到上限而不是收敛。
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port(), "/loop")).max_redirects(2));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpRedirectLimit);
    // 初始一跳 + 两跳跟随 = 三次连接，其后因超限而止。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);
}

AURORA_TEST_CASE(send_times_out_whole_request) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;

    Client client;
    ScriptedPeer peer(silent());  // 连得上，但永不回字节：读侧一直挂着。
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(Request::get(loopback_url(peer.port())).timeout(std::chrono::milliseconds{50}));
    // 先泵到连接与写出落地（对端已 accept、读请求挂起）。
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    // 推进脚本化时钟到整请求时限：到期取消令牌，挂起的读以 net-cancelled 收尾，收尾据 timed_out 改道。
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_read_idle) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    // 对端只写头部（声明 100 字节体却不再给）后静默：读体阶段挂起，read_idle 到点。
    ScriptedPeer peer(write_then_hold("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.read_idle = std::chrono::milliseconds{50}}));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_connect) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    // 不 pump：让 connect 始终不落地，直接推进时钟触发 connect 档。
    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.connect = std::chrono::milliseconds{50}}));
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

#ifdef AURORA_ENABLE_COMPRESSION

/// @brief 拼一份 chunked 分帧的 200 应答：单块容纳整个体，再跟终止块。
/// @param fields 头部字段行（每行以 `\\r\\n` 结尾，可空）。
/// @param body 消息体字节（分块解码后应交出的字节）。
/// @return 应答字节。
[[nodiscard]] auto chunked_framed(std::string_view fields, const std::vector<char> &body) -> std::string {
    std::string size_text;
    for (std::size_t remaining = body.size(); remaining > 0; remaining /= 16) {
        // 分块长度是十六进制小写（`RFC 9110 §7.1`）：直接算字符而不查表，省一枚下标越界面。
        const int digit = static_cast<int>(remaining % 16);
        const char glyph = digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
        size_text.insert(size_text.begin(), glyph);
    }
    if (size_text.empty()) {
        size_text = "0";
    }
    std::string out = "HTTP/1.1 200 OK\r\n";
    out += fields;
    out += "Transfer-Encoding: chunked\r\n\r\n";
    out += size_text;
    out += "\r\n";
    out.append(body.data(), body.size());
    out += "\r\n0\r\n\r\n";
    return out;
}

AURORA_TEST_CASE(send_decompresses_a_gzip_body) {
    Client client;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
    // 头部按对端原样保留：体已是还原后的字节，但「对端声明了什么」仍是可审计的事实。
    AURORA_TEST_CHECK_EQ(response.value().header("Content-Encoding").value_or(""), "gzip");
}

AURORA_TEST_CASE(send_falls_back_to_a_raw_deflate_stream) {
    Client client;
    // 对端把裸 DEFLATE（RFC 1951）称作 `deflate`：zlib 包装那条路先拒，回落路径接手；分帧走 chunked。
    ScriptedPeer peer(reply_after_reading(1, chunked_framed("Content-Encoding: deflate\r\n", bytes(AURORA_RAW_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
}

AURORA_TEST_CASE(send_undoes_a_multi_stage_chain_in_reverse) {
    Client client;
    // 书写次序即应用次序：载荷先经 gzip 再经 zlib，故还原必须 deflate 先、gzip 后。
    ScriptedPeer peer(
        reply_after_reading(1, length_framed("Content-Encoding: gzip, deflate\r\n", bytes(AURORA_GZIP_THEN_ZLIB))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
}

AURORA_TEST_CASE(send_enforces_the_decompressed_ceiling) {
    const auto wire = length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO));
    ScriptedPeer peer(reply_after_reading(1, wire));  // 线上 32 字节，还原后 12 字节
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    Client tight;
    tight.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{4}});
    auto limited = tight.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("limited done", [&limited] { return limited.is_done(); }).satisfied);
    const auto refused = inner(limited);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    // detail 点名越的是哪一枚旋钮：指错地方，调用方就会去调不管用的那一枚。
    AURORA_TEST_CHECK(refused.error().message.find("max_decompressed_bytes") != std::string::npos);

    // 同一条应答在放宽后放行：证明判据管的是产物字节数而不是「压缩即拒绝」。
    Client loose;
    loose.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{12}});
    auto allowed = loose.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("allowed done", [&allowed] { return allowed.is_done(); }).satisfied);
    const auto delivered = inner(allowed);
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    const auto text = delivered.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(send_advertises_the_supported_codings_by_default) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(record_then_reply(recorded, guard, length_framed("", {})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: gzip, deflate\r\n") != std::string::npos);
}

AURORA_TEST_CASE(send_keeps_the_callers_accept_encoding) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(record_then_reply(recorded, guard, length_framed("", {})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    // 协商是调用方的表达：已自带该字段即不覆盖，也不重复补一条。
    auto task = client.send(Request::get(loopback_url(peer.port())).header("Accept-Encoding", "identity"));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: identity\r\n") != std::string::npos);
}

AURORA_TEST_CASE(build_advertises_and_decodes_only_when_compression_is_compiled_in) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(
        record_then_reply(recorded, guard, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    // 宣告与解码同源于「本构建真的带解码器」：这一腿里两者都必须在场。
    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: gzip, deflate\r\n") != std::string::npos);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto &plain = response.value().body();
    AURORA_TEST_CHECK_EQ(std::string(plain.begin(), plain.end()), "hello aurora");
}

#else

// 能力缺席的腿按 TEST-R6 纪律把只在解码器在场时才成立的用例登记成 SKIP；两腿同名，用例数逐名可比。
AURORA_TEST_CASE(send_decompresses_a_gzip_body) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_falls_back_to_a_raw_deflate_stream) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_undoes_a_multi_stage_chain_in_reverse) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_enforces_the_decompressed_ceiling) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_advertises_the_supported_codings_by_default) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_keeps_the_callers_accept_encoding) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(build_advertises_and_decodes_only_when_compression_is_compiled_in) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(
        record_then_reply(recorded, guard, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    // 不发宣告，也不把压缩字节当明文交出：落 unsupported 而不是交出一段乱码。
    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{0});
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
}

#endif

AURORA_TEST_CASE(send_refuses_a_coding_outside_the_advertised_set) {
    Client client;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: br\r\n", {'u', 'n', 'k'})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(response.error().message.find("br") != std::string::npos);
}

AURORA_TEST_CASE(send_refuses_a_body_that_fails_its_declared_coding) {
    Client client;
    // 分帧干净（长度对得上）但字节过不了 gzip：判消息体损坏，而不是回吐压缩字节。
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", {'N', 'O', 'T', '-'})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
#ifdef AURORA_ENABLE_COMPRESSION
    // 有解码器：分帧干净而字节不合流，成因是流损坏。
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
#else
    // 无解码器：同一份拒绝姿态、另一种成因——环节本身就解不了。两腿都绝不回吐压缩字节。
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
#endif
}

AURORA_TEST_CASE(send_skips_coding_validation_for_a_head_response) {
    Client client;
    // HEAD 应答照抄 GET 的头部（这里连 `br` 都带上了），但本就没有可解的载荷：无体形态不校验编码名。
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 4096\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::head(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().body().size(), std::size_t{0});
}

AURORA_TEST_CASE(progress_reports_upload_bytes_and_always_ends_with_a_settled_value) {
    // 上传侧分块写的节拍判据（64 KiB 一块，200000 字节 = 三整块 + 一尾块）：进度必须不止一笔、按字节单调
    // 不降、末值恰发一次且给出「总量已知」。同时验「分块只为节拍、线上字节序列与一次写完逐字节相同」——
    // 判据落在对端收到的请求全文上，那是本库外部唯一能看的事实。
    Client client;
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer(record_request_then_reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", recorded, guard));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    const std::string payload_body(200000, 'a');
    auto request = Request::post(loopback_url(peer.port()));
    request.header("Accept-Encoding", "identity");  // 显式设定即锁定头部字节，缺省宣告不再注入
    request.body(std::vector<char>{payload_body.begin(), payload_body.end()});
    const auto rendered = request.render_head();
    AURORA_TEST_REQUIRE(bool(rendered));

    std::vector<Progress> seen;  // 回调在续体线程（宿主驱动即用例线程）上被调用，无需加锁
    request.on_progress([&seen](const Progress &progress) -> void { seen.push_back(progress); });

    auto task = client.send(request);  // 传副本：原请求留着算出该出现在线上的字节
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{4000})
                          .satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    const auto uploads = phases_of(seen, ProgressPhase::Upload);
    AURORA_TEST_REQUIRE_GT(uploads.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(monotonic(uploads));
    AURORA_TEST_CHECK_EQ(settled_count(uploads), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(uploads.back().finished);
    AURORA_TEST_CHECK_EQ(uploads.back().transferred_bytes, static_cast<std::uint64_t>(payload_body.size()));
    AURORA_TEST_CHECK_TRUE(uploads.back().total_known);
    AURORA_TEST_CHECK_EQ(uploads.back().total_bytes, static_cast<std::uint64_t>(payload_body.size()));
    AURORA_TEST_CHECK_EQ(uploads.front().transferred_bytes, static_cast<std::uint64_t>(65536));

    const auto downloads = phases_of(seen, ProgressPhase::Download);
    AURORA_TEST_REQUIRE(!downloads.empty());
    AURORA_TEST_CHECK_EQ(settled_count(downloads), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(downloads.back().finished);
    AURORA_TEST_CHECK_EQ(downloads.back().transferred_bytes, std::uint64_t{5});

    const auto received = recorded_request(recorded, guard);
    const std::string expected = rendered.value() + payload_body;
    AURORA_TEST_CHECK_EQ(received.size(), expected.size());
    AURORA_TEST_CHECK_TRUE(received == expected);  // 比布尔值而非比串：失败时不该把 200 KB 打进报告
}

AURORA_TEST_CASE(a_frame_gate_holds_progress_to_one_emission_per_frame_plus_the_last) {
    // 帧号恒定 = 整趟交换只算一帧：此时每方向至多「首发 + 末值」两笔。回合闸下同样的载荷会有多笔首发，
    // 这条判据把「合流发生在源头」写实，而不是留给调用方自己去限频。
    Client client;
    std::uint64_t frame = 1;
    client.progress_frame_source([&frame]() -> std::uint64_t { return frame; });

    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer(record_request_then_reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", recorded, guard));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    const std::string payload_body(300000, 'b');  // 五块：4 × 64 KiB + 一尾块
    auto request = Request::post(loopback_url(peer.port()));
    request.header("Accept-Encoding", "identity");
    request.body(std::vector<char>{payload_body.begin(), payload_body.end()});
    std::vector<Progress> seen;
    request.on_progress([&seen](const Progress &progress) -> void { seen.push_back(progress); });

    auto task = client.send(request);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{4000})
                          .satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    const auto uploads = phases_of(seen, ProgressPhase::Upload);
    AURORA_TEST_REQUIRE_EQ(uploads.size(), std::size_t{2});  // 首发 + 末值，中间三块并进了末值
    AURORA_TEST_CHECK_FALSE(uploads.front().finished);
    AURORA_TEST_CHECK_EQ(uploads.front().transferred_bytes, static_cast<std::uint64_t>(65536));
    AURORA_TEST_CHECK_TRUE(uploads.back().finished);
    AURORA_TEST_CHECK_EQ(uploads.back().transferred_bytes, static_cast<std::uint64_t>(payload_body.size()));
    // 闸读的就是注入的那枚帧号源：换帧即换机会，不是「整趟只许两笔」的写死额度。
    frame = 2;
    AURORA_TEST_CHECK_EQ(client.progress_frame_source()(), std::uint64_t{2});
    // 线上字节仍与一次写完同型（分块只改本库内部的完成次数）。
    const auto rendered = request.render_head();
    AURORA_TEST_REQUIRE(bool(rendered));
    const auto received = recorded_request(recorded, guard);
    AURORA_TEST_CHECK_EQ(received.size(), rendered.value().size() + payload_body.size());
}

AURORA_TEST_CASE(an_unarmed_request_delivers_the_same_wire_bytes_as_an_armed_one) {
    // 没挂回调的请求不走分块写路径，线上字节仍须与挂了回调的那条逐字节相同——判据落在对端收到的全文上，
    // 那是「观测口对无需求者零行为改动」这条口径唯一的外部证据。
    Client client;
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer(record_request_then_reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", recorded, guard));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    const std::string payload_body(200000, 'a');
    auto request = Request::post(loopback_url(peer.port()));
    request.header("Accept-Encoding", "identity");
    request.body(std::vector<char>{payload_body.begin(), payload_body.end()});
    const auto rendered = request.render_head();
    AURORA_TEST_REQUIRE(bool(rendered));
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(request.on_progress()));  // 未挂回调：闸整个不工作

    auto task = client.send(request);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{4000})
                          .satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    const auto received = recorded_request(recorded, guard);
    const std::string expected = rendered.value() + payload_body;
    AURORA_TEST_CHECK_EQ(received.size(), expected.size());
    AURORA_TEST_CHECK_TRUE(received == expected);
}

AURORA_TEST_CASE(a_get_without_a_request_body_reports_no_upload_progress) {
    // 无上传体即不报上传：进度是「这一趟真搬了多少字节」的事实，凑一笔 0/0 只是让调用方多写一个分支。
    Client client;
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto request = Request::get(loopback_url(peer.port()));
    std::vector<Progress> seen;
    request.on_progress([&seen](const Progress &progress) -> void { seen.push_back(progress); });

    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    AURORA_TEST_CHECK_EQ(phases_of(seen, ProgressPhase::Upload).size(), std::size_t{0});
    const auto downloads = phases_of(seen, ProgressPhase::Download);
    AURORA_TEST_REQUIRE(!downloads.empty());
    AURORA_TEST_CHECK_TRUE(downloads.back().finished);
    AURORA_TEST_CHECK_EQ(downloads.back().transferred_bytes, std::uint64_t{5});
}

AURORA_TEST_CASE(a_chunked_body_reports_download_progress_without_a_known_total) {
    // chunked 分帧给不出体总量：进度必须诚实报「未知」，而不是把已到达的字节数当总量冒充 100%。
    Client client;
    ScriptedPeer peer(reply_after_reading(
        1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto request = Request::get(loopback_url(peer.port()));
    std::vector<Progress> seen;
    request.on_progress([&seen](const Progress &progress) -> void { seen.push_back(progress); });

    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);

    const auto downloads = phases_of(seen, ProgressPhase::Download);
    AURORA_TEST_REQUIRE(!downloads.empty());
    for (const Progress &progress : downloads) {
        AURORA_TEST_CHECK_FALSE(progress.total_known);
        AURORA_TEST_CHECK_EQ(progress.total_bytes, std::uint64_t{0});
    }
    AURORA_TEST_CHECK_TRUE(downloads.back().finished);
    AURORA_TEST_CHECK_EQ(downloads.back().transferred_bytes, std::uint64_t{11});
}

AURORA_TEST_CASE(send_replays_a_transient_status_until_the_peer_agrees) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    // 同一条监听口给两种说法：第一条连接回 503，第二条回 200。「换了目标」证不出重放，故不用两个对端。
    ScriptedPeer peer(per_connection({reply_after_reading(1, service_busy()), reply_after_reading(1, service_ok())}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // `jitter = 0` 让「等了就是算得那个数」成为可陈述的事实，而不是一个大概率事件。
    client.retry_policy(RetryPolicy{.attempts = 3,
                                    .base = std::chrono::milliseconds{50},
                                    .max_backoff = std::chrono::milliseconds{60},
                                    .jitter = 0.0});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("first attempt answered", [&peer] { return peer.accepts() > 0; }).satisfied);

    // 退避登记在脚本化时钟上：没推够 `base` 就不该有第二次连接，推过了就必须有。两问都看虚拟时长，
    // 不看真实窗口的长短——后者只能证明「这段时间里没发生」，换个机器就成了假判据。
    AURORA_TEST_CHECK_FALSE(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 40));
    AURORA_TEST_CHECK(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 300));
    AURORA_TEST_CHECK_GE(engine.now().count(), 50);
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    // 只打了两次：第二次成功就不再往下打，`attempts` 是上限而不是次数。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(send_hands_over_the_last_response_when_retries_are_exhausted) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_busy()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 2, .base = std::chrono::milliseconds{10}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    // 非 2xx 是成功的 Result：耗尽时交出的是**服务器最后说的那一句**，本件不折成「重试失败」这种新码。
    const auto response = inner(task);
    AURORA_TEST_REQUIRE(bool(response));
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{503});
    AURORA_TEST_CHECK_EQ(response.value().body().size(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(send_waits_the_retry_after_the_peer_asked_for) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(per_connection(
        {reply_after_reading(1, service_busy("Retry-After: 2\r\n")), reply_after_reading(1, service_ok())}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // `base` 只有 50 ms（上限用缺省的 30 s，不去夹 `Retry-After`）：若对端给的数字没被当回事，
    // 第二次连接会在推进到 50 ms 时就出现。
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{50}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("first attempt answered", [&peer] { return peer.accepts() > 0; }).satisfied);

    // 服务器给的数字压过了指数退避：200 ms 内不该回头，2 s 之后必须回头。
    AURORA_TEST_CHECK_FALSE(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 200));
    AURORA_TEST_CHECK(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 2600));
    AURORA_TEST_CHECK_GE(engine.now().count(), 2000);
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
}

AURORA_TEST_CASE(send_clamps_a_retry_after_to_the_backoff_ceiling) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(per_connection(
        {reply_after_reading(1, service_busy("Retry-After: 3600\r\n")), reply_after_reading(1, service_ok())}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // 对端要我们等一小时，本库的单次等待上限是 100 ms：夹到上限就该看见第二次连接（而不是等一小时的
    // 虚拟时钟），但也不能短到退回 `base`——两问分别由「推进 500 ms 内必到」与「90 ms 内不到」读出。
    client.retry_policy(RetryPolicy{
        .attempts = 3, .base = std::chrono::milliseconds{10}, .max_backoff = std::chrono::milliseconds{100}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("first attempt answered", [&peer] { return peer.accepts() > 0; }).satisfied);
    AURORA_TEST_CHECK_FALSE(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 90));
    AURORA_TEST_CHECK(pump_until(engine, [&peer] { return peer.accepts() > 1; }, 500));
    AURORA_TEST_CHECK_GE(engine.now().count(), 100);
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
}

AURORA_TEST_CASE(send_never_replays_a_non_idempotent_method) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_busy()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    // 状态码那一族本身可重试，但方法是 `POST`：重放可能把一次提交变成两次，而这份代价在失败现场无从判定。
    auto task = client.send(Request::post(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE(bool(response));
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{503});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(send_replays_nothing_while_the_policy_is_off) {
    ScriptedPeer peer(reply_after_reading(1, service_busy()));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    // 缺省策略 = 一次都不重放：这条是「装了环但环关着」与「本件从没打过第二次」的区分口。
    Client client;
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE(bool(response));
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{503});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(send_refuses_to_replay_a_protocol_error) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    // 状态码只有两位：头部本身就坏了，落 `Protocol` 档。重放一万次也是同一个错，故一条连接就收摊。
    ScriptedPeer peer(write_then_half_close("HTTP/1.1 20 OK\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpProtocolError);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(send_refuses_to_replay_a_failure_the_table_marks_non_retryable) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    // 声明 100 字节体却只给两字节就**异常断开**（`SO_LINGER` 时限 0）：读侧拿到的是真实套接口错误，落在
    // `Unreachable` 档，而表把 `net-recv-failed` / `net-send-failed` 标成不可重试。这条正是「只读档位」与
    // 「只读标志」两种错法的区分口——判据取的是交集。
    //
    // 用 `write_then_reset()` 而不是「写完直接 close」：后者交给读侧的是 FIN 还是 RST 并不由剧本决定，
    // 于是「体没读全」会时而落 `net-recv-failed`、时而落 `http-protocol-error`（协议档同样不可重试，用例
    // 看着像过了，实则换了判据来源）。关闭形态钉成 RST 后这条才是稳定的。
    // 结局码随「先撞上读还是先撞上写」在两枚之间取一，档位与标志恒同，故断言钉档位 + 标志 + 不重放。
    ScriptedPeer peer(write_then_reset("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nab"));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(error_class_of(response.error().code_enum), HttpErrorClass::Unreachable);
    AURORA_TEST_CHECK_FALSE(response.error().retryable);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(send_gives_the_total_window_a_fresh_clock_per_attempt) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());  // 连得上、永不回字节；后续尝试连进 backlog 即可，不靠脚本线程放行
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    // `total` 是**每一次尝试**的墙：三次尝试各烧掉一份 50 ms，而不是「整序列只剩 0 ms」。
    auto task =
        client.send(Request::get(loopback_url(peer.port())).timeouts(Timeouts{.total = std::chrono::milliseconds{50}}));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK(names_in(response.error().message, "total"));
    // 三扇墙 + 两段退避 = 170 ms 虚拟时间；若时限是全序列共用的，这里只会烧掉一扇（≈60 ms）。
    // 本例不看 `accepts()`：那条脚本线程卡在第一条连接上永不放行，连接落在 backlog 里同样能把时限跑满。
    AURORA_TEST_CHECK_GE(engine.now().count(), 150);
}

AURORA_TEST_CASE(send_stops_the_retry_sequence_at_the_budget) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // 跨全部尝试的一堵墙（80 ms）比任何单档时限都短：到点即掐掉在途的那一次，剩余尝试不再发起。
    client.retry_policy(
        RetryPolicy{.attempts = 5, .base = std::chrono::milliseconds{10}, .budget = std::chrono::milliseconds{80}});
    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_REQUIRE(pump_send(engine, task));

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    // 被墙掐掉的尝试以 `net-cancelled` 形态抵达：那既不是对端说的也不是本库上限，改报 `http-timeout` 并点名 budget。
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK(names_in(response.error().message, "budget"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(send_refuses_an_out_of_range_policy_before_any_connection) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, service_ok()));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    // 越界形态在起手就挡下（`http-request-invalid`），不进入任何一次尝试：连一条连接都不该发出去。
    auto task = client.send(
        Request::get(loopback_url(peer.port())).retry_policy(RetryPolicy{.base = std::chrono::milliseconds{-1}}));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(names_in(response.error().message, "base"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
}

AURORA_TEST_CASE(a_fresh_cache_hit_never_reaches_the_peer) {
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body_of("first"))));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());

    const auto first = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(first));
    AURORA_TEST_CHECK_FALSE(first.value().from_cache());  // 第一次确实是从对端取回的
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    // 开关兑现的就是这一条：命中新鲜副本时一条连接都不发起，载荷与头部都出自表里那一份。
    const auto second = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(second));
    AURORA_TEST_CHECK(second.value().from_cache());
    AURORA_TEST_CHECK_EQ(second.value().status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(as_body_text(second.value()), std::string{"first"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
}

AURORA_TEST_CASE(a_stale_entry_asks_before_it_delivers) {
    std::string first_request;
    std::string second_request;
    std::mutex guard;
    const aurora::test::net::HostDriven host;
    // `max-age=0` + `ETag`：进得了表，但抵达即过期，于是第二次交换必须先问对端。
    const auto initial = length_framed("Cache-Control: max-age=0\r\nETag: \"v1\"\r\n", body_of("stored"));
    const auto *const not_modified = "HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=600\r\nETag: \"v2\"\r\n\r\n";
    ScriptedPeer peer(per_connection(
        {record_then_reply(first_request, guard, initial), record_then_reply(second_request, guard, not_modified)}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::get(url))));

    const auto second = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(second));
    // 304 不是调用方该看见的结局：并回已存的头部块与体，交出的仍是那份 200。
    AURORA_TEST_CHECK_EQ(second.value().status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(as_body_text(second.value()), std::string{"stored"});
    AURORA_TEST_CHECK(second.value().from_cache());
    AURORA_TEST_CHECK_EQ(second.value().header("ETag").value_or(std::string{}), std::string{"\"v2\""});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);

    // 条件头只挂在本库发起的那次重验证上：第一次请求不该带它，第二次必须带且带着表里那枚值。
    AURORA_TEST_CHECK_EQ(recorded_request(first_request, guard).find("If-None-Match"), std::string::npos);
    const auto sent = recorded_request(second_request, guard);
    AURORA_TEST_CHECK_NE(sent.find("If-None-Match: \"v1\""), std::string::npos);
    // 刚验证过的那份重新获得新鲜度：第三次不必再触网。
    const auto third = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(third));
    AURORA_TEST_CHECK(third.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(the_peer_can_forbid_the_table) {
    const aurora::test::net::HostDriven host;
    // `no-store` 是硬闸：既不存也不复用，两次请求都得到对端去。
    ScriptedPeer peer(
        reply_after_reading(1, length_framed("Cache-Control: no-store, max-age=600\r\n", body_of("now"))));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::get(url))));
    const auto second = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(second));
    AURORA_TEST_CHECK_FALSE(second.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(a_request_may_bypass_the_table) {
    const aurora::test::net::HostDriven host;
    // 两条连接给两种内容：绕过那次的应答若被写进表，第三次普通请求就该读出 `two`。
    ScriptedPeer peer(
        per_connection({reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body_of("one"))),
                        reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body_of("two")))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::get(url))));

    // `Request::no_cache()` 取的是「这次要此刻的真相」：既不查表，这次的结果也不写进表。
    const auto bypass = send_and_drive(client, Request::get(url).no_cache(true));
    AURORA_TEST_REQUIRE(bool(bypass));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
    AURORA_TEST_CHECK_FALSE(bypass.value().from_cache());
    AURORA_TEST_CHECK_EQ(as_body_text(bypass.value()), std::string{"two"});

    // 第三次读回的是**第一次**那份：绕过只读不写，那条硬约定才立得住（若这次交出 `two`，就是写侧漏了闸）。
    const auto after = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(after));
    AURORA_TEST_CHECK(after.value().from_cache());
    AURORA_TEST_CHECK_EQ(as_body_text(after.value()), std::string{"one"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(the_table_is_off_until_it_is_turned_on) {
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body_of("now"))));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;  // 缺省即关：同一 URL 两次请求照旧两次触网，`from_cache()` 恒假
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::get(url))));
    const auto second = send_and_drive(client, Request::get(url));
    AURORA_TEST_REQUIRE(bool(second));
    AURORA_TEST_CHECK_FALSE(second.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(a_write_response_is_never_stored) {
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body_of("created"))));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::post(url))));
    const auto second = send_and_drive(client, Request::post(url));
    AURORA_TEST_REQUIRE(bool(second));
    // 提交类方法的应答描述的是「这次改动的结果」，留在表里等于把一次写的后效反复交付。
    AURORA_TEST_CHECK_FALSE(second.value().from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(vary_makes_the_second_request_a_different_request) {
    std::string first_request;
    std::string second_request;
    std::mutex guard;
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(per_connection(
        {record_then_reply(first_request, guard,
                           length_framed("Cache-Control: max-age=600\r\nVary: Accept\r\n", body_of("json"))),
         record_then_reply(second_request, guard, length_framed("Cache-Control: max-age=600\r\n", body_of("xml")))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());

    // 同一枚 `Accept` 的第二次请求命中：`Vary` 点名的头同名同值才算同一个请求。
    AURORA_TEST_REQUIRE(bool(send_and_drive(client, Request::get(url).header("Accept", "application/json"))));
    const auto same = send_and_drive(client, Request::get(url).header("Accept", "application/json"));
    AURORA_TEST_REQUIRE(bool(same));
    AURORA_TEST_CHECK(same.value().from_cache());
    AURORA_TEST_CHECK_EQ(as_body_text(same.value()), std::string{"json"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    // 换了 `Accept` 就是另一种请求：它必须回到对端，且不能挤掉表里那份 JSON。
    const auto other = send_and_drive(client, Request::get(url).header("Accept", "application/xml"));
    AURORA_TEST_REQUIRE(bool(other));
    AURORA_TEST_CHECK_FALSE(other.value().from_cache());
    AURORA_TEST_CHECK_EQ(as_body_text(other.value()), std::string{"xml"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
    const auto again = send_and_drive(client, Request::get(url).header("Accept", "application/json"));
    AURORA_TEST_REQUIRE(bool(again));
    AURORA_TEST_CHECK(again.value().from_cache());
    AURORA_TEST_CHECK_EQ(as_body_text(again.value()), std::string{"json"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);

    // 两次请求都带着自己那枚 `Accept`：命中与否的差别只在表里的 `Vary` 比对，不在线上。
    AURORA_TEST_CHECK_NE(recorded_request(first_request, guard).find("Accept: application/json"), std::string::npos);
    AURORA_TEST_CHECK_NE(recorded_request(second_request, guard).find("Accept: application/xml"), std::string::npos);
}

}  // namespace aurora::test_cases::utest_net_http_client
