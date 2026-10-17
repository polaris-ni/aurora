/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_await.h
/// 测试说明: 回调→协程等待体的竞速与交付纪律（§12「Expect: 100-continue」与重试退避的底座）。
///           `await_read_within` 面覆盖：字节先到即交 `Bytes` 且随后到期的窗口不再产生第二次交付、窗口先到
///           交 `WaitElapsed` 而迟到的字节只落失者队列（绝不触碰已随协程帧析构的等待体）、被放弃那次的回调
///           早于下一轮落地故线上次序可原样复原、0 字节（对端关写向）与到点同判、读失败交出原样错误载荷且
///           `net-cancelled` 在本层不改道、窗口非正时连一次读登记都不做、引擎拒投定时器时照样当场放行。
///           `await_sleep` 面覆盖：到点唤醒与取消唤醒互斥且恰一次、已取消令牌不进引擎队列、非正时长立即
///           放行、拒投腿也会恢复（退避中的协程绝不悬着）。`await_read` / `await_write` 面覆盖最朴素的一次
///           往返把传输计数交回协程，并证明 spans 指向的调用方存储扛过了整段挂起；另覆盖「同步完成不重入
///           挂起点」——写一登记就交付时，重试环的下一轮写绝不在上一轮写尚未返回时发起（嵌套深度恒 1）。
/// 全程零 socket、零线程：交付时机与虚拟时钟都由用例点名——「谁先到」这条竞速判据用真实套接字测不出来。
/// 异步路径的用例统一装「排队式」主线程投递器：恢复只入队不执行，由用例线程排空——恢复点由用例说了算，
/// 与真实事件循环同形，「交付恰一次」也可经队列长度逐一点验。同步完成（窗口非正、引擎拒投、已取消、
/// 非正退避）则一律不经投递器：`await_suspend` 返回 `false`，协程在挂起表达式里就地前进，故这些用例的
/// `drain()` 恒为 0——这是「同步完成不得在 await_suspend 内重入协程帧」的回归哨：无主投递器时
/// `post_to_main` 就地执行闭包，一旦同步完成改走投递器，挂起点之后的代码就会在 await_suspend 尚未返回时
/// 跑第二遍，协程经其中一条路收尾释放帧后，迟到的回调再恢复已释放的帧即成 heap-use-after-free。

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/state/async.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"

namespace m = aurora::testing::matchers;

namespace aurora::test_cases::utest_net_http_await {
namespace {

using aurora::CoroTask;
using aurora::net::Cancellation;
using aurora::net::detail::await_read;
using aurora::net::detail::await_read_within;
using aurora::net::detail::await_sleep;
using aurora::net::detail::await_write;
using aurora::net::detail::WindowedRead;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::testing::require_value;

/// @brief 排队式主线程投递器：续体只入队不执行，由用例线程排空。
/// @note Thread: 装与拆限用例线程（进程级单例投递器）
/// @note Side-effects: 改写进程级主线程投递器的指向
class PosterQueue {
  public:
    PosterQueue() {
        aurora::Task<int>::set_main_poster([this](std::function<void()> fn) -> void {
            std::scoped_lock<std::mutex> lock(mutex_);
            queued_.push_back(std::move(fn));
        });
    }
    PosterQueue(const PosterQueue &) = delete;
    auto operator=(const PosterQueue &) -> PosterQueue & = delete;
    PosterQueue(PosterQueue &&) = delete;
    auto operator=(PosterQueue &&) -> PosterQueue & = delete;
    ~PosterQueue() { aurora::Task<int>::set_main_poster(nullptr); }

    /// @brief 尚未执行的投递项数。
    /// @return 队列长度。
    [[nodiscard]] auto size() -> std::size_t {
        std::scoped_lock<std::mutex> lock(mutex_);
        return queued_.size();
    }

    /// @brief 排空队列（含恢复期间新排入的项）。
    /// @return 本次执行的闭包数。
    auto drain() -> std::size_t {
        std::size_t ran = 0;
        for (;;) {
            std::function<void()> fn;
            {
                std::scoped_lock<std::mutex> lock(mutex_);
                if (queued_.empty()) {
                    break;
                }
                fn = std::move(queued_.front());
                queued_.erase(queued_.begin());
            }
            fn();
            ++ran;
        }
        return ran;
    }

  private:
    std::mutex mutex_;  ///< 保护队列（投递器可能被任何线程调用）
    std::vector<std::function<void()>> queued_;  ///< 已排入、待执行的闭包
};

/// @brief 一条只记录请求、由用例决定何时交付的假流：读写出口与 `StreamTransport` 同构。
///
/// 本头只依赖 `read` / `write` 两枚方法，故不需要真实套接字：交付时机与载荷全由用例点名，「字节先到还是
/// 窗口先到」这条竞速判据才有逐值断言。每条请求只交付一次（用例自己写错脚本时也不会二次触发回调）。
class FakeStream {
  public:
    using Done = std::function<void(Result<std::size_t>)>;  ///< 完成回调形态，与 `ReadDone` / `WriteDone` 同构

    /// @brief 登记一次读：记下缓冲与回调，字节由 `give` / `close_read` / `fail_read` 交。
    /// @param buffer 目标缓冲（等待体持有，回调存活期内有效）。
    /// @param on_done 完成回调。
    auto read(std::span<char> buffer, Done on_done) -> void {
        read_requests_.push_back(Request<std::span<char>>{.region = buffer, .on_done = std::move(on_done)});
    }

    /// @brief 登记一次写：记下待写字节，接受与否由 `take_write` 决定；开了同步交付模式则当场交付。
    /// @param data 待写字节（调用方持有，须活过整段挂起）。
    /// @param on_done 完成回调。
    auto write(std::span<const char> data, Done on_done) -> void {
        write_requests_.push_back(Request<std::span<const char>>{.region = data, .on_done = std::move(on_done)});
        if (!inline_writes_) {
            return;  // 缺省形态：只登记，交付时机由用例点名
        }
        // 同步交付：回调在本次 write 调用内就抵达（循环套接字上写立即成功的形态）。深度跟踪让
        // 「恢复是否嵌在另一轮写尚未返回时」可被逐值断言——重入的正确形态是深度恒 1。
        ++write_depth_;
        max_write_depth_ = std::max(max_write_depth_, write_depth_);
        std::size_t accepted = data.size();
        if (inline_write_rejections_ > 0) {
            --inline_write_rejections_;
            accepted = 0;  // 同步交 0 字节：不是失败，是一次「没写完」，调用方据此重试
        }
        write_requests_.back().deliver(accepted);
        --write_depth_;
    }

    /// @brief 已登记的读请求数。
    /// @return 条数。
    [[nodiscard]] auto read_requests() const -> std::size_t { return read_requests_.size(); }

    /// @brief 已登记的写请求数。
    /// @return 条数。
    [[nodiscard]] auto write_requests() const -> std::size_t { return write_requests_.size(); }

    /// @brief 第 `index` 次写收到的线上字节（挂起之后仍可读，证明调用方存储活得足够久）。
    /// @param index 读/写登记序。
    /// @return 待写字节副本。
    [[nodiscard]] auto written_bytes(std::size_t index) const -> std::string {
        const std::span<const char> data = write_requests_[index].region;
        return std::string{data.begin(), data.end()};
    }

    /// @brief 让第 `index` 次读交出 `payload`（超出缓冲上限的部分丢弃，与真实读的口径一致）。
    /// @param index 读登记序。
    /// @param payload 交出的字节。
    auto give(std::size_t index, std::string_view payload) -> void {
        auto &request = read_requests_[index];
        const std::size_t length = std::min(payload.size(), request.region.size());
        std::copy_n(payload.begin(), static_cast<std::ptrdiff_t>(length), request.region.data());
        request.deliver(length);
    }

    /// @brief 让第 `index` 次读交出 0 字节（对端关闭写向）。
    /// @param index 读登记序。
    auto close_read(std::size_t index) -> void { read_requests_[index].deliver(std::size_t{0}); }

    /// @brief 让第 `index` 次读以失败收场。
    /// @param index 读登记序。
    /// @param code 失败错误码。
    /// @param message 失败说明。
    auto fail_read(std::size_t index, ErrorCode code, std::string_view message) -> void {
        read_requests_[index].deliver(make_error(code, std::string{message}));
    }

    /// @brief 让第 `index` 次写全盘接受。
    /// @param index 写登记序。
    auto take_write(std::size_t index) -> void {
        const std::size_t length = write_requests_[index].region.size();
        write_requests_[index].deliver(length);
    }

    /// @brief 打开写的同步交付模式：此后每次写一登记就交付，前 `rejections` 次交 0 字节（调用方重试），
    /// 之后全盘接受。用于复现「循环套接字上写立即成功」的同步完成路径。
    /// @param rejections 先交出 0 字节的次数（0 = 每次都全盘接受）。
    auto complete_writes_inline(std::size_t rejections) -> void {
        inline_writes_ = true;
        inline_write_rejections_ = rejections;
    }

    /// @brief 历史最大的写调用嵌套深度。
    /// @return 1 = 任何一次写都不曾在另一轮写尚未返回时发起（同步恢复的正确形态）；2+ = 重入。
    [[nodiscard]] auto max_write_depth() const -> std::size_t { return max_write_depth_; }

  private:
    /// @brief 一条在途请求：目标区 + 完成回调 + 已交付标记。
    /// @tparam Region 缓冲形态（读为可写 span，写为只读 span）
    template <typename Region>
    struct Request {
        Region region;  ///< 等待体给出的缓冲或待写字节
        Done on_done;  ///< 完成回调
        bool fired{false};  ///< 是否已交付

        /// @brief 交付成功结局。
        /// @param count 实际读 / 已接受的字节数。
        auto deliver(std::size_t count) -> void {
            if (fired) {
                return;
            }
            fired = true;
            on_done(Result<std::size_t>{count});
        }

        /// @brief 交付失败结局。
        /// @param error 失败载荷。
        auto deliver(Error error) -> void {
            if (fired) {
                return;
            }
            fired = true;
            on_done(Result<std::size_t>{std::move(error)});
        }
    };

    std::vector<Request<std::span<char>>> read_requests_;  ///< 按登记序排好的读请求
    std::vector<Request<std::span<const char>>> write_requests_;  ///< 按登记序排好的写请求
    bool inline_writes_{false};  ///< 写一登记就交付（同步完成路径）；缺省关，交付时机由用例点名
    std::size_t inline_write_rejections_{0};  ///< 同步交付中先交 0 字节的次数（重试脚本）
    std::size_t write_depth_{0};  ///< 当前写调用嵌套深度（同步交付里恢复协程即体现为嵌套）
    std::size_t max_write_depth_{0};  ///< 历史最大写嵌套深度
};

/// @brief 字节向量转文本，便于逐值断言线上内容。
/// @param bytes 待转换的字节。
/// @return 同字节的 `std::string`。
[[nodiscard]] auto text(const std::vector<char> &bytes) -> std::string {
    return std::string{bytes.begin(), bytes.end()};
}

/// @brief 一次窗口读的协程：把等待体交回的结局写进盒子。
/// @param stream 假流指针（对象须比协程活得久；协程形参一律不用引用）。
/// @param late 失者字节队列（同上，按值持有共享所有权）。
/// @param capacity 本次读的缓冲上限。
/// @param window 等待窗口。
/// @param out 结局落点（用例持有的栈上对象）。
/// @return 恒 1（协程值不承载判据，判据都在 `out`）。
auto windowed_read(FakeStream *stream, std::shared_ptr<std::vector<char>> late, std::size_t capacity,
                   std::chrono::milliseconds window, WindowedRead *out) -> CoroTask<int> {
    auto handed = co_await await_read_within(*stream, capacity, window, late);
    // 等待体恒以成功态交出 `WindowedRead`（失败形态在结构体里，不在 Result 里），故这里只搬值不分支。
    *out = std::move(handed.value());
    co_return 1;
}

/// @brief 连续两轮窗口读的协程：按发起侧口径把「失者队列 + 本轮结局」拼成线上次序。
/// @param stream 假流指针（对象须比协程活得久）。
/// @param late 失者字节队列（两轮共用，每轮消费后清空）。
/// @param out 两轮结局的落点（按声明序逐轮写入）。
/// @param wire 复原出的线上字节序列。
/// @return 恒 1。
auto two_windowed_reads(FakeStream *stream, std::shared_ptr<std::vector<char>> late, std::array<WindowedRead, 2> *out,
                        std::string *wire) -> CoroTask<int> {
    for (WindowedRead &slot : *out) {
        auto handed = co_await await_read_within(*stream, 16, std::chrono::milliseconds{50}, late);
        slot = std::move(handed.value());
        wire->insert(wire->end(), late->begin(), late->end());
        late->clear();
        if (slot.outcome == WindowedRead::Outcome::Bytes) {
            wire->insert(wire->end(), slot.bytes.begin(), slot.bytes.end());
        }
    }
    co_return 1;
}

/// @brief 一次写加一次读的最朴素协程：把两个计数写进盒子。
/// @param stream 假流指针（对象须比协程活得久）。
/// @param payload 待写字节（调用方存储，须活过挂起）。
/// @param buffer 读缓冲（调用方存储，须活过挂起）。
/// @param written 写结局落点。
/// @param read_count 读结局落点。
/// @return 恒 1。
auto plain_round_trip(FakeStream *stream, std::span<const char> payload, std::span<char> buffer, std::size_t *written,
                      std::size_t *read_count) -> CoroTask<int> {
    const auto wrote = co_await await_write(*stream, payload);
    if (!wrote) {
        co_return 0;  // 本用例不脚本化失败，失败即把「不该发生」交给断言侧的计数暴露出来
    }
    *written = wrote.value();
    const auto got = co_await await_read(*stream, buffer);
    if (!got) {
        co_return 0;
    }
    *read_count = got.value();
    co_return 1;
}

/// @brief 重试写协程：写不满 payload 就再写一轮，记录实际发起的轮数。
/// @param stream 假流指针（对象须比协程活得久；协程形参一律不用引用）。
/// @param payload 待写字节（调用方存储，须活过挂起）。
/// @param attempts 实际写轮数落点。
/// @return 写满即 1；本用例不脚本化失败（假流同步模式只交成功态）。
auto write_until_accepted(FakeStream *stream, std::span<const char> payload, std::size_t *attempts) -> CoroTask<int> {
    for (;;) {
        const auto wrote = co_await await_write(*stream, payload);
        ++*attempts;
        if (!wrote) {
            co_return 0;
        }
        if (wrote.value() >= payload.size()) {
            co_return 1;
        }
    }
}

/// @brief 一段退避等待的结局记录。
/// @note Thread: 单线程（协程写、用例读）
/// @note Side-effects: none
struct SleepRecord {
    bool ok{false};  ///< 等待体交出的是成功态
    bool cancelled{false};  ///< 恢复那一刻令牌的取消位
};

/// @brief 一段退避等待的协程：记下结局与当时的取消位。
/// @param token 取消令牌（按值持有，与等待体同寿）。
/// @param delay 等待时长。
/// @param out 结局落点（用例持有的栈上对象）。
/// @return 恒 1。
auto wait_then_report(Cancellation token, std::chrono::milliseconds delay, SleepRecord *out) -> CoroTask<int> {
    const auto woke = co_await await_sleep(delay, token);
    out->ok = woke.ok();
    out->cancelled = token.is_cancelled();
    co_return 1;
}

}  // namespace

AURORA_TEST_CASE(bytes_arriving_first_win_and_the_late_timer_is_dropped) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{500}, &out));
    AURORA_TEST_CHECK_FALSE(task.is_done());
    AURORA_TEST_REQUIRE_EQ(stream.read_requests(), std::size_t{1});

    stream.give(0, "HTTP/1.1 100 Continue\r\n");
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::Bytes);
    AURORA_TEST_CHECK_EQ(text(out.bytes), std::string{"HTTP/1.1 100 Continue\r\n"});
    AURORA_TEST_CHECK_FALSE(out.error.has_value());

    // 窗口随后到期：失者只该放弃交付。此刻等待体已随协程帧析构，故判据是「没有第二次交付」而非返回值。
    AURORA_TEST_CHECK_EQ(engine.advance(std::chrono::milliseconds{500}), std::size_t{1});
    AURORA_TEST_CHECK_EQ(poster.size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(late->size(), std::size_t{0});
}

AURORA_TEST_CASE(the_window_expiring_first_keeps_the_late_bytes_out_of_the_stream) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{50}, &out));
    AURORA_TEST_REQUIRE_EQ(stream.read_requests(), std::size_t{1});

    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::WaitElapsed);
    AURORA_TEST_CHECK_EQ(out.bytes.size(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(out.error.has_value());
    // 本层不撤销已经登记的那次读：它照样会落地，只是字节另有去处。
    AURORA_TEST_CHECK_EQ(stream.read_requests(), std::size_t{1});

    stream.give(0, "100");
    AURORA_TEST_CHECK_EQ(text(*late), std::string{"100"});
    AURORA_TEST_CHECK_EQ(poster.size(), std::size_t{0});  // 迟到的字节不再恢复任何协程
}

AURORA_TEST_CASE(abandoned_bytes_precede_the_next_round_on_the_wire) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    std::array<WindowedRead, 2> rounds;
    std::string wire;

    const auto task = launch(two_windowed_reads(&stream, late, &rounds, &wire));
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});  // 第一轮到点 → 立刻登记第二轮
    AURORA_TEST_REQUIRE_EQ(stream.read_requests(), std::size_t{2});

    // 传输层按 FIFO 满足读请求：被放弃那次的回调必然早于下一轮的回调落地。
    stream.give(0, "first");
    AURORA_TEST_CHECK_EQ(text(*late), std::string{"first"});
    stream.give(1, "second");
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());

    AURORA_TEST_CHECK_TRUE(rounds[0].outcome == WindowedRead::Outcome::WaitElapsed);
    AURORA_TEST_CHECK_TRUE(rounds[1].outcome == WindowedRead::Outcome::Bytes);
    AURORA_TEST_CHECK_EQ(wire, std::string{"firstsecond"});  // 线上次序原样复原，一个字节都没丢
    AURORA_TEST_CHECK_EQ(late->size(), std::size_t{0});
}

AURORA_TEST_CASE(a_closed_write_side_reads_as_the_window_elapsing) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{500}, &out));
    stream.close_read(0);
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    // 0 字节是「对端已关写向」而不是错误：与到点同判，发起侧照走「不再等」那条路。
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::WaitElapsed);
    AURORA_TEST_CHECK_FALSE(out.error.has_value());
    // 那枚窗口定时器照旧排在引擎里（本层没有撤销口）：到点后它只会被「恰好完成一次」的闸吃掉。
    AURORA_TEST_CHECK_EQ(engine.pending(), std::size_t{1});
    engine.advance(std::chrono::milliseconds{500});
    AURORA_TEST_CHECK_EQ(poster.size(), std::size_t{0});
}

AURORA_TEST_CASE(a_failing_read_is_handed_back_with_its_own_reason) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{500}, &out));
    stream.fail_read(0, ErrorCode::NetRecvFailed, "peer reset mid-read");
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::Failure);
    AURORA_TEST_REQUIRE_TRUE(out.error.has_value());
    const auto failure = require_value(out.error);
    AURORA_TEST_CHECK(failure.code_enum == ErrorCode::NetRecvFailed);
    AURORA_TEST_CHECK_THAT(failure.message, m::has_substr("peer reset mid-read"));
    AURORA_TEST_CHECK_EQ(out.bytes.size(), std::size_t{0});
}

AURORA_TEST_CASE(a_cancelled_read_reaches_the_caller_as_cancellation) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{500}, &out));
    // 令牌掐掉连接后抵达的 `net-cancelled`：本层不解释取消，也不把它改道成任何超时形态。
    stream.fail_read(0, ErrorCode::NetCancelled, "read aborted by cancellation");
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::Failure);
    AURORA_TEST_REQUIRE_TRUE(out.error.has_value());
    AURORA_TEST_CHECK(require_value(out.error).code_enum == ErrorCode::NetCancelled);
}

AURORA_TEST_CASE(a_non_positive_window_registers_neither_a_read_nor_a_timer) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    const std::array<std::int64_t, 2> widths{0, -1};

    for (const std::int64_t width : widths) {
        auto late = std::make_shared<std::vector<char>>();
        WindowedRead out;
        const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{width}, &out));
        // 同步完成就地恢复：投递器里什么都不该有（改走投递器就是在 await_suspend 内重入协程帧）。
        AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
        AURORA_TEST_CHECK_TRUE(task.is_done());
        AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::WaitElapsed);
        // 「不等待」必须连那条 FIFO 队列都不占：否则这一轮读会把下一轮的字节顶到后面。
        AURORA_TEST_CHECK_EQ(stream.read_requests(), std::size_t{0});
        AURORA_TEST_CHECK_EQ(engine.pending(), std::size_t{0});
        AURORA_TEST_CHECK_EQ(late->size(), std::size_t{0});
    }
}

AURORA_TEST_CASE(an_engine_that_refuses_work_still_releases_the_waiter) {
    FakeEngine engine;
    engine.refuse_posts(true);
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    FakeStream stream;
    auto late = std::make_shared<std::vector<char>>();
    WindowedRead out;

    const auto task = launch(windowed_read(&stream, late, 32, std::chrono::milliseconds{50}, &out));
    // 登记不成也必须放行：协程悬着比不等更糟。同步交付同样就地恢复，不经投递器。
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(out.outcome == WindowedRead::Outcome::WaitElapsed);
    AURORA_TEST_CHECK_EQ(engine.posts_refused(), 1);
    AURORA_TEST_CHECK_EQ(stream.read_requests(), std::size_t{1});  // 读已登记，字节仍会落地到失者队列
    stream.give(0, "x");
    AURORA_TEST_CHECK_EQ(text(*late), std::string{"x"});
}

AURORA_TEST_CASE(the_backoff_wakes_at_the_timer) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    Cancellation token;
    SleepRecord record;

    const auto task = launch(wait_then_report(token, std::chrono::milliseconds{30}, &record));
    AURORA_TEST_CHECK_FALSE(task.is_done());
    AURORA_TEST_REQUIRE_EQ(engine.pending(), std::size_t{1});

    engine.advance(std::chrono::milliseconds{29});
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(task.is_done());

    engine.advance(std::chrono::milliseconds{1});
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(record.ok);
    AURORA_TEST_CHECK_FALSE(record.cancelled);  // 谁结束的这一下，调用方只按取消位判
}

AURORA_TEST_CASE(cancelling_a_backoff_releases_it_once_before_the_deadline) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    Cancellation token;
    SleepRecord record;

    const auto task = launch(wait_then_report(token, std::chrono::milliseconds{30}, &record));
    token.cancel();
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(record.cancelled);

    // 到期时刻仍会到（引擎没有撤销口）：那一次必须被「恰好完成一次」的闸吃掉。
    engine.advance(std::chrono::milliseconds{30});
    AURORA_TEST_CHECK_EQ(poster.size(), std::size_t{0});
}

AURORA_TEST_CASE(an_already_cancelled_token_short_circuits_the_wait) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    Cancellation token;
    token.cancel();
    SleepRecord record;

    const auto task = launch(wait_then_report(token, std::chrono::milliseconds{5000}, &record));
    // 同步短路就地恢复：不进引擎队列，也不经投递器。
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_TRUE(record.cancelled);
    AURORA_TEST_CHECK_EQ(engine.pending(), std::size_t{0});  // 不进引擎队列，也就不占任何时限
    AURORA_TEST_CHECK_EQ(engine.tasks_run(), 0);
}

AURORA_TEST_CASE(a_non_positive_or_refused_backoff_still_resumes_the_coroutine) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    PosterQueue poster;
    Cancellation token;

    {
        SleepRecord zero;
        const auto task = launch(wait_then_report(token, std::chrono::milliseconds{0}, &zero));
        // 非正退避同步放行：就地恢复，不经投递器。
        AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
        AURORA_TEST_CHECK_TRUE(task.is_done());
        AURORA_TEST_CHECK_TRUE(zero.ok);
        AURORA_TEST_CHECK_FALSE(zero.cancelled);
        AURORA_TEST_CHECK_EQ(engine.pending(), std::size_t{0});
    }

    engine.refuse_posts(true);
    SleepRecord refused;
    {
        const auto task = launch(wait_then_report(token, std::chrono::milliseconds{40}, &refused));
        // 拒投同样同步放行：就地恢复，不经投递器。
        AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{0});
        AURORA_TEST_CHECK_TRUE(task.is_done());
        AURORA_TEST_CHECK_TRUE(refused.ok);
    }
    AURORA_TEST_CHECK_GE(engine.posts_refused(), 1);
}

AURORA_TEST_CASE(a_write_completed_inline_never_reenters_the_suspension_point) {
    // 本用例刻意不装投递器：无主时 `post_to_main` 就地执行闭包，正是重入事故的培养基。
    FakeStream stream;
    stream.complete_writes_inline(2);  // 前两轮写同步交 0 字节（没写完，重试），第三轮全盘接受
    const std::string payload = "POST /x HTTP/1.1\r\n";
    const std::span<const char> outbound{payload.data(), payload.size()};
    std::size_t attempts = 0;

    const auto task = launch(write_until_accepted(&stream, outbound, &attempts));
    // 三轮写全部在 await_suspend 内经返回值就地恢复：launch 返回时协程已收尾，轮数一轮不多。
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_EQ(attempts, std::size_t{3});
    AURORA_TEST_CHECK_EQ(stream.write_requests(), std::size_t{3});
    // 核心判据：任何一轮写都不曾在上一轮写尚未返回时发起。同步完成若改经投递器恢复，重试环的下一轮
    // 就会嵌在上一轮写的回调栈里（嵌套深度 2+），协程帧也会在嵌套栈尚未退尽时就被销毁。
    AURORA_TEST_CHECK_EQ(stream.max_write_depth(), std::size_t{1});
}

AURORA_TEST_CASE(a_plain_write_then_read_hands_back_the_transport_counts) {
    PosterQueue poster;
    FakeStream stream;
    const std::string payload = "POST /x HTTP/1.1\r\n";
    std::vector<char> buffer(8, '\0');
    std::size_t written = 0;
    std::size_t read_count = 0;

    const std::span<const char> outbound{payload.data(), payload.size()};
    const std::span<char> inbound{buffer.data(), buffer.size()};
    const auto task = launch(plain_round_trip(&stream, outbound, inbound, &written, &read_count));
    AURORA_TEST_CHECK_FALSE(task.is_done());
    AURORA_TEST_REQUIRE_EQ(stream.write_requests(), std::size_t{1});

    stream.take_write(0);
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(task.is_done());  // 写完了，读还挂着
    AURORA_TEST_REQUIRE_EQ(stream.read_requests(), std::size_t{1});
    // 挂起期间调用方的存储仍然有效：span 指向的原文此刻还读得出来。
    AURORA_TEST_CHECK_EQ(stream.written_bytes(0), payload);

    stream.give(0, "abc");
    AURORA_TEST_CHECK_EQ(poster.drain(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(task.is_done());
    AURORA_TEST_CHECK_EQ(written, payload.size());
    AURORA_TEST_CHECK_EQ(read_count, std::size_t{3});
}
}  // namespace aurora::test_cases::utest_net_http_await
