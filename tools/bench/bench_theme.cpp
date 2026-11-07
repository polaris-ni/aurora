// Theme read-face benchmark: measure what one `inherit_theme(ctx)` read actually costs, in the
// before/after shapes of the same read, so the "reference instead of whole-value copy" claim is a
// number rather than an argument.
//
// Notes:
// - This program is a "benchmark/diagnostic" tool, not a unit test; it is not wired into CTest
//   (timings are affected by environment jitter). Output goes to stdout via AURORA_LOG_RAW
//   (project hard rule #8: program product output uses the raw channel).
// - The "before" shape is reproduced at the read point as `aurora::Theme{inherit_theme(ctx)}`: the
//   pre-change signature returned `Theme` by value, i.e. exactly one whole-value construction per
//   read, which is what that expression costs today. The comparison is therefore like-for-like, not
//   an estimate.
// - Every read happens inside a widget's `on_paint`, behind a virtual call and followed by a store
//   to that widget's member. That is deliberate: a tight loop around a single read is a hoisting
//   target for the optimizer, a per-widget virtual call is not. Reads per frame are counted (not
//   inferred from the widget count) by the same instrumented pass.
// - Heap allocations are counted by replacing the global `operator new` / `operator delete` for this
//   process (see the two functions below). The counter is off outside the measured window, so the
//   logger and the fixture setup are not charged; third-party C code that calls `malloc` directly is
//   likewise invisible. Deltas between the three read forms isolate the read-side allocations from
//   everything else the frame does.
// - The copy is measured on two family-name axes, because `Font::family` is a `std::string`: the
//   Material baseline family ("sans-serif", 10 bytes) fits libstdc++'s small-string buffer, so a
//   whole-value copy of an explicitly injected `Theme::light()` allocates *nothing*; a family longer
//   than that capacity makes each of the 15 fonts allocate. Reporting only the baseline axis would
//   read as "a by-value face is free", which is true only by accident -- and since the style-axis
//   batch it is not even the default: `Window::prepare_context` injects the *platform* theme every
//   frame, and on Windows that family is "Segoe UI Variable Text" (22 bytes, past SSO), so an app
//   that never mentions themes is the allocating case. The scene without a `ThemeProvider` is
//   therefore labelled `win_root_platform_family`; `injected_800_long_family` is the explicit second
//   axis. Allocation counts are bit-identical across reruns while the millisecond columns jitter
//   with machine load, so ratios are the conclusion.
// - Requires a Headless build (`AURORA_BACKEND_HEADLESS`); usage: ./bench_theme
//
// Scenes: a `ThemeProvider`-rooted grid of probe widgets (rows x cols, one probe per cell), in
// three read forms -- `none` (probe reads nothing: the frame baseline), `by_value` (before shape),
// `by_reference` (landed shape) -- plus the same grid presented without a provider, where each read
// picks up the theme the window injects at the root (the platform style on this machine), not the
// in-library Material fallback.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/widget.h"
#include "bench_common.h"

#ifdef AURORA_BACKEND_HEADLESS

namespace {

// 场景口径：1280x720 逻辑尺寸、scale 1.0；探针数两档；计时前的热身帧与插桩帧数。
// 计时取「批均值的最小值」，故分批数 x 每批帧数决定抖动下限：帧耗时只会被系统噪声抬高、不会凭空变快，
// 最小批均值是对本机本树真实成本最不发散的估计；三种读取形态的差值只有几十纳秒，必须有比差值更小的抖动。
constexpr int AURORA_BENCH_WINDOW_W = 1280;
constexpr int AURORA_BENCH_WINDOW_H = 720;
constexpr float AURORA_BENCH_SCALE = 1.0F;
constexpr int AURORA_BENCH_WARMUP = 5;
constexpr int AURORA_BENCH_BATCHES = 7;
constexpr int AURORA_BENCH_FRAME_ITERS = 25;
constexpr int AURORA_BENCH_SWAP_ITERS = 6;
constexpr int AURORA_BENCH_COUNTED_FRAMES = 9;
// SSO 溢出场景用的长字体族名：`Font::family` 是 `std::string`，Material 基线档的 "sans-serif"（10 字节）
// 落在小串优化里、拷贝零分配；越过 SSO 容量后每枚 Font 各一次分配，整值拷贝的分配成本才显形。
// 注意缺省配置已是越界档：`Window` 每帧注入本机风格档，Windows 上族名 "Segoe UI Variable Text"（22 字节）。
constexpr const char *AURORA_BENCH_LONG_FAMILY = "Aurora Benchmark Family Name For Sso Overflow";

// ---------------------------------------------------------------------------
// 堆计账（仅本工具）：替换全局 operator new / delete，只在测量窗口内累加。
// 计数状态留在匿名命名空间，但 `operator new` / `operator delete` 是**全局替换版**，
// 标准不允许在命名空间内声明（[free.store]），故钩子前后各关闭 / 重开一次匿名段。
// 计数器必须是进程内唯一的文件作用量：钩子是全进程替换版、拿不到任何上下文，故这条形态约束
// 无法用「改局部量 + 传参」消解，只能就地豁免。
// ---------------------------------------------------------------------------
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_instrumented{false};
std::atomic<std::size_t> g_paint_calls{0};
std::atomic<std::size_t> g_alloc_count{0};
std::atomic<std::size_t> g_alloc_bytes{0};
std::atomic<std::size_t> g_theme_reads{0};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto note_alloc(std::size_t n) -> void {
    if (g_instrumented.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
    }
}

}  // namespace

// 四枚钩子必须直连 malloc / free：替换版 `operator new` 若回头调用 `::operator new` 即无限递归，
// 唯一的对等分配器就是 C 的 malloc / free（[free.store] 允许替换版自选分配方式）。故 no-malloc
// 与「分配器错配」两条在此无法用 RAII 消解，就地豁免。
// NOLINTBEGIN(cppcoreguidelines-no-malloc,clang-analyzer-unix.MismatchedDeallocator)
void *operator new(std::size_t n) {
    note_alloc(n);
    void *p = std::malloc(n != 0U ? n : 1U);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

// GCC 的 -Wmismatched-new-delete 只看得到「本 TU 里 operator new 取的是 malloc 内存」这一半，
// 于是把配对的 free 判成错配分配器。[free.store] 允许替换版用任意分配方式（libstdc++ 自己的实现同样
// 走 malloc/free），故这是假告警；仅对下面四枚 delete 局部豁免，不改 new/delete 的实现口径。
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

void operator delete[](void *p, [[maybe_unused]] std::size_t size) noexcept {
    std::free(p);  // 同上：数组版也只按指针释放。
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-no-malloc,clang-analyzer-unix.MismatchedDeallocator)

namespace {

// ---------------------------------------------------------------------------
// 读面探针控件
// ---------------------------------------------------------------------------

// 一次读取的三种形态：不读（帧基线）、整值拷贝（改动前的按值读面）、按引用（落地形态）。
enum class ReadForm : std::uint8_t { None, ByValue, ByReference };

// 主题读取探针：每次读取落在「虚函数调用 → 读取 → 成员写入」这条独立链上，编译器无法把读取
// 提出控件之间，也无法把按值形态的整值构造折叠掉（构造带 std::string 成员的对象是可观察副作用）。
class ThemeProbe final : public aurora::Widget {
  public:
    ThemeProbe(ReadForm form, float w, float h) : form_(form), w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeProbe"; }

  protected:
    auto on_layout(const aurora::Constraints &c, const aurora::BuildContext & /*ctx*/) -> aurora::Size override {
        return c.constrain(aurora::Size{.width = w_, .height = h_});
    }

    auto on_paint(aurora::Painter &p, const aurora::Rect &bounds, const aurora::BuildContext &ctx) -> void override {
        g_paint_calls.fetch_add(1, std::memory_order_relaxed);
        switch (form_) {
            case ReadForm::None:
                break;
            case ReadForm::ByValue:
                // 改动前读面形态：原签名按值返回，一次读取即一份 984 字节整值拷贝（含 15 枚 Font 的 family
                // 串：Material 基线档落在小串容量内零分配，越界档每枚各一次 —— 两档都由本基准分列跑出）。
                consume(aurora::Theme{inherit_theme(ctx)});
                break;
            case ReadForm::ByReference:
                // 落地形态：引用注入存储本身，零拷贝、零分配。
                consume(inherit_theme(ctx));
                break;
        }
        (void)p;
        (void)bounds;
    }

  private:
    auto consume(const aurora::Theme &t) -> void {
        // 真实消费点：角色与字阶都读进成员，读取不可省。异或进 sink_ 只为防死代码消除，不参与判据。
        sink_ ^= static_cast<std::uint64_t>(t.roles.primary.r) << 24U;
        sink_ ^= static_cast<std::uint64_t>(t.roles.surface.g) << 16U;
        sink_ ^= static_cast<std::uint64_t>(t.type_scale.body_medium.size_pt * 1000.0F);
        if (g_instrumented.load(std::memory_order_relaxed)) {
            g_theme_reads.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ReadForm form_;
    float w_;
    float h_;
    std::uint64_t sink_ = 0;  // 消费位：读取结果写进成员，故不可被死代码消除。
};

// ---------------------------------------------------------------------------
// 场景构建与测量
// ---------------------------------------------------------------------------

// 固定尺寸无头窗口（先 set_scale 再 begin_frame：物理缓冲 = 逻辑尺寸 x scale）。
auto make_window(int w, int h, float scale) -> aurora::Window {
    auto surface = std::make_unique<aurora::HeadlessSurface>();
    surface->painter().set_scale(scale);
    (void)surface->begin_frame(w, h);
    return aurora::Window{std::move(surface)};
}

// 一个测量场景：根节点 + 主题来源（无注入场景没有来源）+ 探针数 + 族名档位（换肤时同档往返）。
struct Scene {
    aurora::Node root;
    std::shared_ptr<aurora::State<aurora::Theme>> theme_state;
    int probes = 0;
    bool long_family = false;
};

// count 枚探针铺成近似方阵（2dp 单元格，整片落在视口内，不因裁剪而少画探针）。
auto build_probes(ReadForm form, int count) -> std::vector<aurora::Node> {
    const auto cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const auto rows = (count + cols - 1) / cols;
    std::vector<aurora::Node> out;
    out.reserve(static_cast<std::size_t>(rows));
    int placed = 0;
    for (int r = 0; r < rows; ++r) {
        std::vector<aurora::Node> cells;
        for (int c = 0; c < cols && placed < count; ++c, ++placed) {
            cells.emplace_back(std::make_shared<ThemeProbe>(form, 2.0F, 2.0F));
        }
        out.emplace_back(std::make_shared<aurora::Row>(std::move(cells)));
    }
    return out;
}

// 字阶 15 档的成员指针表：把「每枚 Font 的族名」统一改写成长名，用于 SSO 溢出那一档对照。
constexpr std::array<aurora::Font aurora::TypeScale::*, 15> AURORA_BENCH_TYPE_FIELDS = {
    &aurora::TypeScale::display_large,  &aurora::TypeScale::display_medium,  &aurora::TypeScale::display_small,
    &aurora::TypeScale::headline_large, &aurora::TypeScale::headline_medium, &aurora::TypeScale::headline_small,
    &aurora::TypeScale::title_large,    &aurora::TypeScale::title_medium,    &aurora::TypeScale::title_small,
    &aurora::TypeScale::body_large,     &aurora::TypeScale::body_medium,     &aurora::TypeScale::body_small,
    &aurora::TypeScale::label_large,    &aurora::TypeScale::label_medium,    &aurora::TypeScale::label_small,
};

// 把 15 档族名全部换成长名（字号 / 字重原值不动）：整值拷贝因此每枚 Font 各一次堆分配。
auto with_long_family(aurora::Theme t) -> aurora::Theme {
    for (auto field : AURORA_BENCH_TYPE_FIELDS) {
        (t.type_scale.*field).family = AURORA_BENCH_LONG_FAMILY;
    }
    return t;
}

auto theme_with_long_family() -> aurora::Theme { return with_long_family(aurora::Theme::light()); }

// 注入场景：ThemeProvider(State<Theme>) 包住探针方阵 —— 换肤即写这枚 State。
// long_family 决定注入主题的族名档位：false 用 Material 基线档短族名（落在 SSO 内，拷贝零分配），true 用越界长族名。
auto build_scene(ReadForm form, int count, bool long_family) -> Scene {
    auto col = std::make_shared<aurora::Column>(build_probes(form, count));
    auto theme_state =
        std::make_shared<aurora::State<aurora::Theme>>(long_family ? theme_with_long_family() : aurora::Theme::light());
    auto provider = std::make_shared<aurora::ThemeProvider>(*theme_state, aurora::Node{std::move(col)});
    return Scene{
        .root = aurora::Node{std::move(provider)},
        .theme_state = std::move(theme_state),
        .probes = count,
        .long_family = long_family,
    };
}

// 应用根无 `ThemeProvider` 的场景：探针方阵直接作根，不经 `Provider` 重建子环境。注意它**不是**「无注入」
// ——`Window::prepare_context` 每帧仍把本机风格档注入根环境，故这里读到的是平台档（Windows 上族名越 SSO）。
auto build_fallback_scene(ReadForm form, int count) -> Scene {
    auto col = std::make_shared<aurora::Column>(build_probes(form, count));
    return Scene{
        .root = aurora::Node{std::move(col)},
        .theme_state = nullptr,
        .probes = count,
        .long_family = false,
    };
}

// 一帧：先把**呈现根**（窗口的私有内容壳）整子树的布局 / Display List 缓存清零，再整帧重排重绘。
// 必须从壳起：`present_root` 把应用根 adopt 进 `detail::ContentInsetRoot` 壳、帧管线统一以壳为根，
// 只清应用根子树时壳自身的两级缓存仍然有效，整树在壳处短路 —— `on_layout` / `on_paint` 一次也不会
// 进入，读数全是噪声。少了整句清零，稳态帧同样命中各控件的 DL 缓存直接回放。
// 这对应的真实场景是「每帧都重绘」（换肤、动画、自驱动滚动），不是静态界面 idle 跳帧：真实控件靠
// 自身 `mark_needs_*` 沿布局父链上溯来清掉祖先缓存，本基准的探针不持有任何状态，故显式发起一次向下清零。
auto present_frame(aurora::Window &win, aurora::Node &root) -> void {
    win.hit_root().widget().invalidate_subtree_caches();
    win.force_full_redraw();
    (void)win.present_root(root);
}

// 一帧的实测读数：主题解析次数、堆分配次数与字节数（仅本次遍历，插桩窗口外的都不计入），
// 外加「一帧进入 on_paint 的探针数」—— 这是整树确实被走到的自证：探针数 == 绘制数 == 解析数，
// 任一不相等即说明本帧被缓存短路、当行读数作废（表内以 BAD 标出，不会再静默给出噪声数字）。
struct FrameCounters {
    std::size_t reads = 0;
    std::size_t allocs = 0;
    std::size_t bytes = 0;
    std::size_t paints = 0;
};

auto count_frames(aurora::Window &win, aurora::Node &root, int frames) -> FrameCounters {
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_theme_reads.store(0, std::memory_order_relaxed);
    const auto paint0 = g_paint_calls.load(std::memory_order_relaxed);
    g_instrumented.store(true, std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        present_frame(win, root);
    }
    g_instrumented.store(false, std::memory_order_relaxed);
    const auto f = static_cast<double>(frames);
    return FrameCounters{
        .reads = static_cast<std::size_t>(static_cast<double>(g_theme_reads.load()) / f),
        .allocs = static_cast<std::size_t>(static_cast<double>(g_alloc_count.load()) / f),
        .bytes = static_cast<std::size_t>(static_cast<double>(g_alloc_bytes.load()) / f),
        .paints = static_cast<std::size_t>(static_cast<double>(g_paint_calls.load() - paint0) / f),
    };
}

const char *form_label(ReadForm form) {
    switch (form) {
        case ReadForm::None:
            return "none(baseline)";
        case ReadForm::ByValue:
            return "by_value(before)";
        case ReadForm::ByReference:
            return "by_reference(after)";
    }
    return "unknown";
}

// 读面一行的所有判据：探针数、一帧解析次数、每帧堆分配、单次解析的纳秒与分配数、稳态帧耗时。
// `ns/read` 用「同一树、同一次运行里读面形态之间的稳态帧耗时差」除以一帧解析次数得到：帧成本对三形态
// 同量（同一棵树、同样的清缓存动作），差值只归因于读取本身；不写死探针数，故裁剪或缓存导致少画时自动失真。
auto report_read_row(const std::string &scene_label, ReadForm form, int probes, double frame_ms,
                     const FrameCounters &counters, const FrameCounters &baseline, double baseline_ms) -> void {
    // 自证：读取数须等于绘制数（不读那一档两者皆 0），绘制数须等于探针数——少一个即整树被短路。
    // 探针数先落成同宽度无符号量再比，避免在比较式里混符号。
    const auto probe_count = static_cast<std::size_t>(probes);
    const bool traversal_ok =
        counters.paints == probe_count && counters.reads == (form == ReadForm::None ? std::size_t{0} : counters.paints);
    const auto reads = static_cast<double>(counters.reads != 0U ? counters.reads : 1U);
    const auto ns_per_read = (frame_ms - baseline_ms) * 1.0E6 / reads;
    const auto allocs_per_read = (static_cast<double>(counters.allocs) - static_cast<double>(baseline.allocs)) /
                                 (form == ReadForm::None ? 1.0 : reads);
    AURORA_LOG_RAW("bench", "| ", scene_label, " | ", form_label(form), " | ", probes, " | ", counters.reads, " | ",
                   counters.paints, " | ", counters.allocs, " | ", counters.bytes, " | ",
                   aurora::bench::ffmt(2, allocs_per_read), " | ", aurora::bench::ffmt(1, ns_per_read), " | ",
                   aurora::bench::ffmt(3, frame_ms), " | ", traversal_ok ? "ok" : "BAD", " |\n");
}

// 换肤一帧：写主题 + 整帧呈现；与稳态帧、纯写入三段并列，另记插桩分配。
struct SwapCounters {
    double steady_ms = 0.0;
    double swap_ms = 0.0;
    double set_ms = 0.0;
    std::size_t swap_allocs = 0;
};

// 稳健计时：分批后取「批均值的最小值」。帧耗时只会被系统噪声抬高、不会凭空变快，故最小批均值是对
// 本机本树真实成本最不发散的估计；直接取总均值会被偶发长尾污染，而读面形态之间的差值只有几十纳秒，
// 测量抖动必须显著小于待测差值，否则差值没有意义。
auto time_ms_min_batch(const std::function<void()> &fn, int warmup, int batches, int iters) -> double {
    for (int i = 0; i < warmup; ++i) {
        fn();
    }
    double best = std::numeric_limits<double>::max();
    for (int b = 0; b < batches; ++b) {
        best = std::min(best, aurora::bench::time_ms(fn, 0, iters));
    }
    return best;
}

auto measure_swap(aurora::Window &win, Scene &scene, double steady_ms) -> SwapCounters {
    bool flip = false;
    // 换肤按本场景同档往返：SSO 溢出档必须继续带长族名，否则写回 `dark()` 把 15 档族名换成短名，
    // 那一帧的分配数就不再反映本场景的拷贝成本。
    auto next_theme = [&]() -> aurora::Theme {
        flip = !flip;
        auto t = flip ? aurora::Theme::dark() : aurora::Theme::light();
        return scene.long_family ? with_long_family(std::move(t)) : t;
    };
    const auto swap_ms = time_ms_min_batch(
        [&]() -> void {
            scene.theme_state->set(next_theme());
            present_frame(win, scene.root);
        },
        2, AURORA_BENCH_BATCHES, AURORA_BENCH_SWAP_ITERS);
    const auto set_ms = aurora::bench::time_ms([&]() -> void { scene.theme_state->set(next_theme()); }, 2, 20);
    // 插桩一帧：换肤那一帧的堆分配（Provider 重建子环境的整值拷贝与父映射堆拷贝，加各探针的读取拷贝）。
    present_frame(win, scene.root);
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_instrumented.store(true, std::memory_order_relaxed);
    scene.theme_state->set(next_theme());
    present_frame(win, scene.root);
    g_instrumented.store(false, std::memory_order_relaxed);
    return SwapCounters{
        .steady_ms = steady_ms,
        .swap_ms = swap_ms,
        .set_ms = set_ms,
        .swap_allocs = g_alloc_count.load(std::memory_order_relaxed),
    };
}

auto report_swap_row(const std::string &scene_label, ReadForm form, int probes, const SwapCounters &c) -> void {
    AURORA_LOG_RAW("bench", "| ", scene_label, " | ", form_label(form), " | ", probes, " | ",
                   aurora::bench::ffmt(3, c.steady_ms), " | ", aurora::bench::ffmt(3, c.swap_ms), " | ",
                   aurora::bench::ffmt(3, c.set_ms), " | ", c.swap_allocs, " |\n");
}

// 读面身份自检（非计时，一条文本行）：按引用读取返回的是注入存储本身；无注入时是同一枚静态兜底。
// 判据落在「地址是否同一对象」上，与耗时无关，故不参与上面的对照表。
void report_identity_checks() {
    aurora::Environment env;
    env.set_local(aurora::Theme::dark());
    const aurora::BuildContext ctx{.env = &env};
    const bool injected_is_storage = &inherit_theme(ctx) == ctx.environment<aurora::Theme>();

    const aurora::BuildContext empty_ctx{};
    const bool fallback_same_object = &inherit_theme(empty_ctx) == &inherit_theme(empty_ctx);
    const bool fallback_is_light = inherit_theme(empty_ctx) == aurora::Theme::light();

    AURORA_LOG_RAW("bench", "\nidentity checks: read returns injected storage = ", injected_is_storage ? "yes" : "no",
                   "; fallback is one stable object = ", fallback_same_object ? "yes" : "no",
                   "; fallback equals Theme::light() = ", fallback_is_light ? "yes" : "no", "\n");
}

// 一个场景的实测读数：稳态帧耗时 + 插桩帧计数，注入场景另附换肤一帧（同一棵树、同一个窗口跑完，
// 避免「换一棵树再测」把挂载成本混进对照）。
struct SceneRun {
    double ms = 0.0;
    FrameCounters counters{};
    SwapCounters swap{};
};

auto run_scene(Scene &scene, bool with_swap) -> SceneRun {
    aurora::Window win = make_window(AURORA_BENCH_WINDOW_W, AURORA_BENCH_WINDOW_H, AURORA_BENCH_SCALE);
    SceneRun run;
    run.ms = time_ms_min_batch([&]() -> void { present_frame(win, scene.root); }, AURORA_BENCH_WARMUP,
                               AURORA_BENCH_BATCHES, AURORA_BENCH_FRAME_ITERS);
    run.counters = count_frames(win, scene.root, AURORA_BENCH_COUNTED_FRAMES);
    if (with_swap) {
        run.swap = measure_swap(win, scene, run.ms);
    }
    return run;
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），基准工具不做 try/catch 包装。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    AURORA_LOG_RAW("bench", "sizeof(Theme) = ", sizeof(aurora::Theme),
                   " bytes; sizeof(ColorRoles) = ", sizeof(aurora::ColorRoles),
                   "; sizeof(TypeScale) = ", sizeof(aurora::TypeScale), "\n\n");

    AURORA_LOG_RAW("bench", "## read-face migration (same tree, three read forms)\n\n");
    AURORA_LOG_RAW("bench",
                   "| scene | read form | probes | reads/frame | paints/frame | allocs/frame | "
                   "alloc bytes/frame | allocs/read | ns/read | steady frame ms | counts check |\n");
    AURORA_LOG_RAW("bench", "|---|---|---|---|---|---|---|---|---|---|---|\n");

    const std::vector<ReadForm> forms = {ReadForm::None, ReadForm::ByValue, ReadForm::ByReference};
    // 换肤行按「标签 / 形态 / 探针数 / 计数」暂存，读面表打完后统一输出。
    std::vector<std::tuple<std::string, ReadForm, int, SwapCounters>> swap_rows;

    // 场景轴：探针数两档（SSO 短族名）+ 一档大探针数长族名（越界后每枚 Font 各一次分配）。
    struct SceneSpec {
        int probes;
        bool long_family;
        const char *label;
    };
    const std::vector<SceneSpec> specs = {
        SceneSpec{.probes = 200, .long_family = false, .label = "injected_200_sso"},
        SceneSpec{.probes = 800, .long_family = false, .label = "injected_800_sso"},
        SceneSpec{.probes = 800, .long_family = true, .label = "injected_800_long_family"},
    };

    for (const SceneSpec &spec : specs) {
        const std::string label = spec.label;
        SceneRun baseline;
        for (const ReadForm form : forms) {
            Scene scene = build_scene(form, spec.probes, spec.long_family);
            const bool with_swap = form != ReadForm::None;
            const SceneRun run = run_scene(scene, with_swap);
            if (form == ReadForm::None) {
                baseline = run;
            }
            report_read_row(label, form, scene.probes, run.ms, run.counters, baseline.counters, baseline.ms);
            if (with_swap) {
                swap_rows.emplace_back(label, form, scene.probes, run.swap);
            }
        }
    }

    // 无注入通路：读到的是函数内静态兜底，不经映射查表，也不触发 Provider 重建。
    SceneRun fb_baseline;
    for (const ReadForm form : forms) {
        Scene scene = build_fallback_scene(form, specs.front().probes);
        const SceneRun run = run_scene(scene, false);
        if (form == ReadForm::None) {
            fb_baseline = run;
        }
        report_read_row("win_root_platform_family", form, scene.probes, run.ms, run.counters, fb_baseline.counters,
                        fb_baseline.ms);
    }

    report_identity_checks();

    AURORA_LOG_RAW("bench", "\n## theme-swap frame (write Theme into State, then full present)\n\n");
    AURORA_LOG_RAW("bench",
                   "| scene | read form | probes | steady frame ms | swap frame ms | set-only ms | "
                   "swap allocs |\n");
    AURORA_LOG_RAW("bench", "|---|---|---|---|---|---|---|\n");
    for (const auto &[label, form, probes, counters] : swap_rows) {
        report_swap_row(label, form, probes, counters);
    }

    AURORA_LOG_RAW("bench", "\n", aurora::bench::AURORA_BENCH_DISCLAIMER, "\n");
    return 0;
}

#else

auto main() -> int { return 0; }

#endif  // AURORA_BACKEND_HEADLESS
