// 状态订阅登记、数组差分与 schema 取值的微基准。
//
// 三段读数各自对应一处重复开销：
// - 订阅：同一来源上反复建立 / 撤销 K 条订阅（等价页面重建时的全量重订阅），看单次订阅的耗时；
// - 差分：两侧数组只差一个元素，看 `diff()` 的耗时与分配数（旧实现为每个元素拼一条下标路径）；
// - schema：缓存命中的按值取值与只读视图取值，看单次调用的耗时与分配字节数。
//
// 说明：
// - 这是基准 / 诊断工具，不接入 CTest（耗时会受机器负载影响）。输出走 `AURORA_LOG_RAW`（程序产物通道）。
// - 堆分配数由本文件替换全局 `operator new` / `delete` 统计，只在测量窗口内累加；分配数是稳定判据，
//   毫秒列只作同机相对参考。
// - 基线取法：暂存实现后用同一份基准重跑，比对同一场景的读数。
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/widget/serialization.h"
#include "bench_common.h"

namespace {

// 场景规模：订阅数与重建轮数、差分数组长度、schema 迭代次数。
constexpr int AURORA_BENCH_SUBSCRIBERS = 400;
constexpr int AURORA_BENCH_SUB_ROUNDS = 60;
constexpr int AURORA_BENCH_DIFF_ELEMENTS = 4096;
constexpr int AURORA_BENCH_DIFF_ITERS = 40;
constexpr int AURORA_BENCH_SCHEMA_ITERS = 4000;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_instrumented{false};
std::atomic<std::size_t> g_alloc_count{0};
std::atomic<std::size_t> g_alloc_bytes{0};
std::size_t g_sink = 0;  // 消费位：防止被测调用被整体消除
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto note_alloc(std::size_t n) -> void {
    if (g_instrumented.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
    }
}

}  // namespace

// 全局替换版 operator new / delete 必须直连 malloc / free（替换版回头调用 ::operator new 即递归），
// 且标准不允许在命名空间内声明，故就地豁免。
// NOLINTBEGIN(cppcoreguidelines-no-malloc,clang-analyzer-unix.MismatchedDeallocator)
void *operator new(std::size_t n) {
    note_alloc(n);
    void *p = std::malloc(n != 0U ? n : 1U);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void *p) noexcept { std::free(p); }

void operator delete(void *p, [[maybe_unused]] std::size_t size) noexcept {
    std::free(p);  // sized delete 的形参按标准必须在签名里，本实现只按指针释放。
}

void *operator new[](std::size_t n) {
    note_alloc(n);
    void *p = std::malloc(n != 0U ? n : 1U);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete[](void *p) noexcept { std::free(p); }

void operator delete[](void *p, [[maybe_unused]] std::size_t size) noexcept { std::free(p); }
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-no-malloc,clang-analyzer-unix.MismatchedDeallocator)

namespace {

// 一次测量窗口内的堆分配读数。
struct AllocSample {
    std::size_t count = 0;
    std::size_t bytes = 0;
};

auto sample_allocs(const std::function<void()> &fn) -> AllocSample {
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_instrumented.store(true, std::memory_order_relaxed);
    fn();
    g_instrumented.store(false, std::memory_order_relaxed);
    return AllocSample{.count = g_alloc_count.load(std::memory_order_relaxed),
                       .bytes = g_alloc_bytes.load(std::memory_order_relaxed)};
}

// 一段场景的读数行。
void report_row(const std::string &scene, double ms, double per_unit_ns, const AllocSample &allocs,
                const std::string &unit) {
    const std::string key = "bench_state_serial/" + scene;
    aurora::bench::process_result().metric(key + "/ms", ms);
    aurora::bench::process_result().metric(key + "/per_unit_ns", per_unit_ns, "ns");
    aurora::bench::process_result().metric(key + "/allocs", static_cast<double>(allocs.count), "count");
    aurora::bench::process_result().metric(key + "/alloc_bytes", static_cast<double>(allocs.bytes), "bytes");
    AURORA_LOG_RAW("bench", "| ", scene, " | ", aurora::bench::ffmt(3, ms), " | ", aurora::bench::ffmt(1, per_unit_ns),
                   " | ", allocs.count, " | ", allocs.bytes, " | ", unit, " |\n");
}

// ---------------------------------------------------------------------------
// 场景一：同一来源上全量重订阅
// ---------------------------------------------------------------------------
auto bench_subscribe(aurora::State<int> &state) -> void {
    std::vector<aurora::Subscription> subs;
    subs.reserve(static_cast<std::size_t>(AURORA_BENCH_SUBSCRIBERS));
    for (int i = 0; i < AURORA_BENCH_SUBSCRIBERS; ++i) {
        subs.push_back(state.subscribe([](const int &value) { g_sink += static_cast<std::size_t>(value & 1); }));
    }
    // 撤销同样在计时区间内：它也是「重建一轮」的一部分（本轮不改撤销路径，两版等价）。
    subs.clear();
}

void run_subscribe() {
    aurora::State<int> state{0};
    const auto ms = aurora::bench::time_ms([&]() -> void { bench_subscribe(state); }, 3, AURORA_BENCH_SUB_ROUNDS);
    const double per_sub_ns = ms * 1.0E6 / static_cast<double>(AURORA_BENCH_SUBSCRIBERS);
    const auto allocs = sample_allocs([&]() -> void { bench_subscribe(state); });
    const auto per_alloc = static_cast<double>(allocs.count) / static_cast<double>(AURORA_BENCH_SUBSCRIBERS);
    const auto per_bytes = static_cast<double>(allocs.bytes) / static_cast<double>(AURORA_BENCH_SUBSCRIBERS);
    report_row("subscribe_round", ms, per_sub_ns, AllocSample{.count = allocs.count, .bytes = allocs.bytes},
               "per-round");
    AURORA_LOG_RAW("bench", "subscribe per-subscription: ns=", aurora::bench::ffmt(1, per_sub_ns),
                   " allocs=", aurora::bench::ffmt(2, per_alloc), " bytes=", aurora::bench::ffmt(1, per_bytes), "\n");
}

// ---------------------------------------------------------------------------
// 场景二：大数组差分（只差一个元素）
// ---------------------------------------------------------------------------
auto make_nodes(int count) -> aurora::Json {
    aurora::Json arr = aurora::Json::array();
    for (int i = 0; i < count; ++i) {
        aurora::Json node = aurora::Json::object();
        node.set("type", std::string{"Row"});
        node.set("id", static_cast<std::int64_t>(i));
        node.set("label", std::string{"cell-"} + std::to_string(i));
        arr.push_back(std::move(node));
    }
    return arr;
}

void run_diff() {
    const aurora::Json before = make_nodes(AURORA_BENCH_DIFF_ELEMENTS);
    aurora::Json after = before;
    // 改一个元素的一处键：补丁只应产出 1 条 replace，其余元素仍须被比较。
    after.at(static_cast<std::size_t>(AURORA_BENCH_DIFF_ELEMENTS / 2))->set("label", std::string{"changed"});

    std::size_t patches = 0;
    const auto ms = aurora::bench::time_ms(
        [&]() -> void { patches = aurora::serialization::diff(before, after).size(); }, 2, AURORA_BENCH_DIFF_ITERS);
    const auto allocs = sample_allocs([&]() -> void { patches = aurora::serialization::diff(before, after).size(); });
    g_sink += patches;

    const std::string key = "bench_state_serial/diff_array";
    aurora::bench::process_result().metric(key + "/ms", ms);
    aurora::bench::process_result().metric(key + "/patches", static_cast<double>(patches), "count");
    aurora::bench::process_result().metric(key + "/allocs", static_cast<double>(allocs.count), "count");
    aurora::bench::process_result().metric(key + "/alloc_bytes", static_cast<double>(allocs.bytes), "bytes");
    AURORA_LOG_RAW("bench", "| diff_array(", AURORA_BENCH_DIFF_ELEMENTS, ") | ", aurora::bench::ffmt(3, ms), " | ",
                   patches, " | ", allocs.count, " | ", allocs.bytes, " | per-call |\n");
}

// ---------------------------------------------------------------------------
// 场景三：schema 取值（按值 vs 只读视图）
// ---------------------------------------------------------------------------
void run_schema() {
    const std::string type = "Button";

    std::size_t sink = 0;
    const auto ms_value = aurora::bench::time_ms(
        [&]() -> void {
            for (int i = 0; i < AURORA_BENCH_SCHEMA_ITERS; ++i) {
                const aurora::Json schema = aurora::describe_component(type);
                sink += schema.size();
            }
        },
        2, 3);
    const auto alloc_value = sample_allocs([&]() -> void {
        for (int i = 0; i < AURORA_BENCH_SCHEMA_ITERS; ++i) {
            const aurora::Json schema = aurora::describe_component(type);
            sink += schema.size();
        }
    });

    const auto ms_view = aurora::bench::time_ms(
        [&]() -> void {
            for (int i = 0; i < AURORA_BENCH_SCHEMA_ITERS; ++i) {
                const auto view = aurora::describe_component_view(type);
                sink += view->size();
            }
        },
        2, 3);
    const auto alloc_view = sample_allocs([&]() -> void {
        for (int i = 0; i < AURORA_BENCH_SCHEMA_ITERS; ++i) {
            const auto view = aurora::describe_component_view(type);
            sink += view->size();
        }
    });
    g_sink += sink;

    const auto iters = static_cast<double>(AURORA_BENCH_SCHEMA_ITERS);
    report_row("schema_value", ms_value, ms_value * 1.0E6 / iters,
               AllocSample{.count = alloc_value.count, .bytes = alloc_value.bytes}, "per-call");
    report_row("schema_view", ms_view, ms_view * 1.0E6 / iters,
               AllocSample{.count = alloc_view.count, .bytes = alloc_view.bytes}, "per-call");

    // 全量列表：按值逐个深拷贝 vs 只取视图。
    std::size_t types = 0;
    const auto list_ms_value = aurora::bench::time_ms(
        [&]() -> void {
            const auto all = aurora::list_all_schemas();
            types = all.size();
        },
        2, 5);
    const auto list_alloc_value = sample_allocs([&]() -> void {
        const auto all = aurora::list_all_schemas();
        types = all.size();
    });
    const auto list_ms_view = aurora::bench::time_ms(
        [&]() -> void {
            const auto all = aurora::list_all_schema_views();
            types = all.size();
        },
        2, 5);
    const auto list_alloc_view = sample_allocs([&]() -> void {
        const auto all = aurora::list_all_schema_views();
        types = all.size();
    });
    g_sink += types;

    report_row("list_all_value", list_ms_value, list_ms_value,
               AllocSample{.count = list_alloc_value.count, .bytes = list_alloc_value.bytes}, "per-call");
    report_row("list_all_views", list_ms_view, list_ms_view,
               AllocSample{.count = list_alloc_view.count, .bytes = list_alloc_view.bytes}, "per-call");
    AURORA_LOG_RAW("bench", "registered types = ", types, "\n");
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），基准工具不做 try/catch 包装。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    aurora::bench::process_result().identify("bench_state_serial", "state-serial");
    AURORA_LOG_RAW("bench", "| scene | ms | per-unit ns | allocs | bytes | unit |\n");
    AURORA_LOG_RAW("bench", "|---|---|---|---|---|---|\n");

    run_subscribe();
    run_diff();
    run_schema();

    AURORA_LOG_RAW("bench", "\n", aurora::bench::AURORA_BENCH_DISCLAIMER, "\n");
    return aurora::bench::finalize_process("bench_state_serial", "state-serial") ? 0 : 1;
}
