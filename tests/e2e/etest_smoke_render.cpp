/// 测试类型: e2e
/// 目标单元: tools/include/e2e/harness.h
/// 测试说明: 真实后端冒烟与 golden 容差层（按场景 × 后端矩阵展开）。两组用例：
///
///   1. `reads_back_scene_pixels` —— 采样点冒烟：对每个目标后端建**真实窗口**、渲染同一纯色
///      场景一帧、经 `Surface::data()` 读回，对纯色区域中心断言期望 RGBA（按比例采样，DPI 无关）。
///   2. `golden_within_tolerance_of_software_baseline` —— 真实后端 golden 容差层：四形态代表性
///      场景（纯绘制叶 / 容器型 / 滚动 / 含 Application 手势链路）经真实窗口渲染到静止态，与
///      `scene_tool --render` 生成的软件 SSOT 基线按场景级申报的容差 + 差异像素预算比对
///      （判据单源 `golden::compare_gpu_tolerance`，软件读回路径与基线同一条 Painter 链路、
///      漂移恒 0，GPU 路径按预算吸收跨驱动 AA 差异）。
///
/// 后端不可用时按 `AURORA_E2E_EXPECT` 记账——期望集内判失败（编排方声明了期望而环境没兑现，
/// 必须修），期望集外跳过（设计上的不适用）。
///
/// 尺寸口径（实测约束）：
///   - 帧尺寸维度：读回帧尺寸是帧缓冲物理像素。runner 进程未声明 DPI 感知（`Win32Host::dpi_scale()`
///     恒 1.0），帧尺寸应等于请求的逻辑尺寸；若实测不等属环境口径而非缺陷，记 SKIP——逐位
///     golden 比对跨缩放无定义；帧尺寸等于逻辑尺寸但与基线 PNG 不一致才是缺陷，直接以尺寸
///     错误失败、不进入像素 diff。
///   - 内容域维度（即使帧尺寸一致也存在）：经物理域离屏缓冲的控件（如 Scroll 的滑动窗口缓冲，
///     按 `ctx.scale_factor` 高清录制、composite 下采样回逻辑缓冲）在 scale != 1 环境下字形
///     光栅与 scale=1 基线不同（如竖笔画 2px 硬边 vs 1px+AA），逐位口径不适用。此类场景按
///     `requires_scale_one` 申报，surface scale != 1 时记 SKIP（CI 100% DPI 环境全跑，本地高
///     DPI 环境诚实跳过）。

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "e2e/harness.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/isolation.h"
#include "scenes/scene_registry.h"

namespace au = aurora;
namespace e2e = aurora::e2e;
namespace golden = aurora::testing::golden;

namespace aurora::test_cases::etest_smoke_render {

namespace {

/// @brief 场景契约：左右两块纯色的期望 RGBA（与 scene_solid_rect.h 的构建常量一致）。
constexpr int AURORA_LEFT_R = 220;
constexpr int AURORA_LEFT_G = 40;
constexpr int AURORA_LEFT_B = 40;
constexpr int AURORA_RIGHT_R = 40;
constexpr int AURORA_RIGHT_G = 40;
constexpr int AURORA_RIGHT_B = 220;

/// @brief 把帧尺寸与采样点实测值记入诊断笔记（`--verbose` 与 JSON 报告可见），
///        使像素类用例的度量常态可见，便于跨驱动漂移的校准。
auto note_metrics(const e2e::Frame &frame, const au::Color &left, const au::Color &right) -> void {
    if (auto *ctx = testing::current_context(); ctx != nullptr) {
        ctx->add_note("frame=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) + " left=(" +
                      std::to_string(left.r) + "," + std::to_string(left.g) + "," + std::to_string(left.b) + ")" +
                      " right=(" + std::to_string(right.r) + "," + std::to_string(right.g) + "," +
                      std::to_string(right.b) + ")");
    }
}

/// @brief 矩阵取值表：全部真实窗口后端。
///
/// 未编译进本构建的后端也留在表内（`open()` 会给出「not compiled」原因）——它们是
/// 「期望集外 → 跳过」这一分支的常态样本，也是编排方声明错期望集时的红灯来源。
/// `Headless` 不在表内：本用例断言的是**真实窗口 + 上屏链路**，无头帧缓冲属内核自测范围。
[[nodiscard]] auto smoke_backend_values() -> std::vector<e2e::Backend> {
    return {e2e::Backend::Win32, e2e::Backend::D3D11,   e2e::Backend::Glfw,
            e2e::Backend::X11,   e2e::Backend::Wayland, e2e::Backend::Wgpu};
}

/// @brief 取值名生成器：直接用后端短名，使报告里一眼看出失败的是哪个后端。
[[nodiscard]] auto smoke_backend_name(const e2e::Backend &backend) -> std::string { return e2e::backend_name(backend); }

/// @brief 后端矩阵 fixture：取值即本用例要建窗的目标后端。
class RealWindowBackends : public testing::TestWithParam<e2e::Backend> {};

// ============================================================
// golden 容差层（场景 × 后端矩阵）
// ============================================================

/// @brief 单通道容差（逐像素判据词汇与 itest_gl_golden / itest_wgpu_golden 同源）。
constexpr int AURORA_E2E_GOLDEN_TOL = 48;

/// @brief 场景级差异像素预算：写在场景旁而非全局旋钮（不读 `AURORA_GOLDEN_MAX_*`）。
///
/// 预算为保守初版，待 CI metrics artifact 实测量级后校准收紧（软件读回路径与基线同一条
/// Painter 链路、漂移恒 0，预算只为 GPU 路径的跨驱动 AA 差异买单）：
///   - `solid_rect`：纯色轴对齐矩形无 AA 斜边，实测 0 漂移；留少量余量防上屏偏移取整。
///   - 其余场景均含文本（见 scene_registry 注释）——字形边缘 AA 与次像素定位是跨驱动漂移
///     主因，先给宽松预算。
/// `requires_scale_one`：场景含物理域离屏缓冲等重采样路径（如 Scroll 滑动窗口按
/// `ctx.scale_factor` 高清录制、composite 下采样回逻辑缓冲），scale != 1 环境下字形光栅与
/// scale=1 基线不同（见文件头「内容域维度」说明），此类环境记 SKIP 而非计入预算。
struct SceneGoldenBudget {
    std::string_view id;
    std::size_t budget;
    std::string_view shape;  ///< 该基线覆盖的控件形态（防范围膨胀的显式申报）
    bool requires_scale_one;
};

inline constexpr SceneGoldenBudget AURORA_SCENE_BUDGETS[] = {
    {.id = "solid_rect", .budget = 64, .shape = "纯绘制叶控件（纯色轴对齐矩形，无文本）", .requires_scale_one = false},
    {.id = "column",
     .budget = 30000,
     .shape = "容器型（Column 布局 + 卡片 + 渐变标题 + 文本）",
     .requires_scale_one = false},
    {.id = "scroll",
     .budget = 30000,
     .shape = "滚动容器（列表视口裁剪 + 文本，物理域离屏缓冲）",
     .requires_scale_one = true},
    {.id = "dismissible",
     .budget = 30000,
     .shape = "含 Application 手势链路（滑动关闭 + 文本）",
     .requires_scale_one = false},
};

/// @brief 查场景预算与形态申报。
[[nodiscard]] auto scene_budget(std::string_view id) -> const SceneGoldenBudget & {
    for (const auto &entry : AURORA_SCENE_BUDGETS) {
        if (entry.id == id) {
            return entry;
        }
    }
    // 注册表内场景必在预算表中（两表同源维护，registry 校验兜底）；不可达路径给保守值
    // （requires_scale_one=true：未知场景在缩放环境下按不适用处理）。
    static const SceneGoldenBudget FALLBACK{.id = "", .budget = 0, .shape = "", .requires_scale_one = true};
    return FALLBACK;
}

/// @brief golden 矩阵取值：场景 × 后端。场景元数据指向注册表静态表（地址稳定，值拷贝安全）。
struct GoldenCase {
    const au::demo_scenes::SceneEntry *scene;
    e2e::Backend backend;
};

/// @brief 取值名生成器：`<场景>-<后端>`，报告里一眼定位失败组合。
[[nodiscard]] auto golden_case_name(const GoldenCase &value) -> std::string {
    return std::string{value.scene->id} + "-" + e2e::backend_name(value.backend);
}

/// @brief golden 矩阵 fixture。
class GoldenBackends : public testing::TestWithParam<GoldenCase> {};

/// @brief 度量 artifact：`AURORA_E2E_METRICS_FILE` 置定时逐用例追加一行 JSONL（3.5）。
///
/// 内容覆盖「差异像素数 / 最大单通道差 / 帧尺寸 / 失败帧 PNG 路径」，供 CI 上传与容差校准；
/// 未置或写失败均不影响用例结果（artifact 是辅助通道，诊断笔记才是 runner 内的常态可见面）。
auto append_metrics_line(const std::string &case_name, const GoldenCase &value, const e2e::Frame &frame,
                         const au::Image &baseline, const au::SnapshotDiff &diff, std::size_t budget,
                         const std::string &fail_png) -> void {
    const char *path = std::getenv("AURORA_E2E_METRICS_FILE");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
        return;
    }
    out << R"({"suite":"etest_smoke_render","case":")" << case_name << R"(","scene":")" << value.scene->id
        << R"(","backend":")" << e2e::backend_name(value.backend) << R"(","frame":)" << frame.width << "x"
        << frame.height << R"(,"baseline":)" << baseline.width << "x" << baseline.height << R"(,"diff_pixels":)"
        << diff.pixel_diff_count << R"(,"max_delta":)" << diff.max_color_delta << R"(,"tol":)" << AURORA_E2E_GOLDEN_TOL
        << R"(,"budget":)" << budget << R"(,"within_budget":)" << (diff.pixel_diff_count <= budget ? "true" : "false")
        << R"(,"fail_png":")" << fail_png << R"("})" << '\n';
}

/// @brief 失败产物落盘（3.6）：比对超预算时把实际帧 PNG 写入 `build/e2e-failures/`，返回路径。
///
/// 预判与判据分离：这里只调 `compare_snapshots` 做落盘决策，**判据仍以
/// `golden::compare_gpu_tolerance` 为单源**（其断言消息含完整差异报告与控件归因）。
[[nodiscard]] auto dump_fail_png_if_over_budget(const std::string &stem, const au::Image &baseline,
                                                const e2e::Frame &frame, std::size_t budget, int tol) -> std::string {
    au::Image current;
    current.width = frame.width;
    current.height = frame.height;
    current.pixels = frame.pixels;
    const au::SnapshotDiff preview = au::compare_snapshots(baseline, current, tol);
    if (preview.pixel_diff_count <= budget) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(testing::isolation::repo_root()) / "build" / "e2e-failures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / (stem + ".png");
    if (const auto written = au::write_png(path.string().c_str(), frame.width, frame.height, frame.pixels.data());
        !written) {
        return {};
    }
    return path.string();
}

}  // namespace

AURORA_TEST_P(RealWindowBackends, reads_back_scene_pixels) {
    const e2e::Backend backend = param();

    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 200;
    spec.title = std::string{"etest_smoke_render:"} + e2e::backend_name(backend);
    // E2E 在无人值守环境运行，默认不把窗口推入用户视野。
    spec.visibility = au::WindowVisibility::Hidden;

    // 建窗失败：期望集内判失败、集外跳过（本函数不返回）。
    auto session = e2e::open(spec);
    if (!session.ok()) {
        e2e::account_unavailable(backend, session.reason());
    }

    au::Node root = demo_scenes::build_solid_rect();
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(root)));

    // 读回能力缺失（如 Win32 的 `data()` 受 `AURORA_ENABLE_DEBUG` 门控而本构建未生效）同样是
    // 「后端不可用」，与建窗失败同口径记账——这正是期望集语义为「期望**可读回**」的落点。
    const auto read = session.read_pixels();
    if (!read) {
        e2e::account_unavailable(backend, read.error().message);
    }
    const e2e::Frame &frame = read.value();

    // 帧尺寸为帧缓冲物理像素：先断言与请求尺寸一致，再做像素断言（否则采样点无意义）。
    AURORA_TEST_CHECK_EQ(frame.width, spec.width);
    AURORA_TEST_CHECK_EQ(frame.height, spec.height);
    AURORA_TEST_REQUIRE_EQ(frame.pixels.size(),
                           static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U);

    // 采样点取各半区中心（按比例，不写死绝对坐标），避免 DPI 缩放带来的偏移误判。
    const au::Color left = e2e::pixel_at(frame, frame.width / 4, frame.height / 2);
    const au::Color right = e2e::pixel_at(frame, (frame.width * 3) / 4, frame.height / 2);
    note_metrics(frame, left, right);

    AURORA_TEST_CHECK_EQ(static_cast<int>(left.r), AURORA_LEFT_R);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.g), AURORA_LEFT_G);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.b), AURORA_LEFT_B);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.a), 255);

    AURORA_TEST_CHECK_EQ(static_cast<int>(right.r), AURORA_RIGHT_R);
    AURORA_TEST_CHECK_EQ(static_cast<int>(right.g), AURORA_RIGHT_G);
    AURORA_TEST_CHECK_EQ(static_cast<int>(right.b), AURORA_RIGHT_B);
    AURORA_TEST_CHECK_EQ(static_cast<int>(right.a), 255);
}

AURORA_TEST_P(GoldenBackends, golden_within_tolerance_of_software_baseline) {
    const GoldenCase value = param();
    const au::demo_scenes::SceneEntry &scene = *value.scene;
    const e2e::Backend backend = value.backend;
    const std::string base_name = std::string{"e2e_"} + scene.id;
    const SceneGoldenBudget &budget_entry = scene_budget(scene.id);

    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = static_cast<int>(scene.width);
    spec.height = static_cast<int>(scene.height);
    spec.title = std::string{"etest_smoke_render:golden:"} + scene.id + ":" + e2e::backend_name(backend);
    spec.visibility = au::WindowVisibility::Hidden;

    auto session = e2e::open(spec);
    if (!session.ok()) {
        e2e::account_unavailable(backend, session.reason());
    }

    // 经物理域离屏缓冲的场景（requires_scale_one）在 scale != 1 环境下经 composite 下采样
    // 读回，字形光栅与 scale=1 基线不同（见文件头「内容域维度」）——诚实跳过，不烧预算。
    if (budget_entry.requires_scale_one && session.surface().scale_factor() != 1.0F) {
        AURORA_TEST_SKIP("场景经物理域离屏缓冲，surface scale " + std::to_string(session.surface().scale_factor()) +
                         " != 1 的内容域缩放环境下逐位 golden 口径不适用");
    }

    au::Node root = scene.build();
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(root)));

    // 场景确定性契约（scene_registry.h）：构建后静止，无待收敛动画——推进到静止态再取帧。
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));

    const auto read = session.read_pixels();
    if (!read) {
        e2e::account_unavailable(backend, read.error().message);
    }
    const e2e::Frame &frame = read.value();

    // 帧尺寸 != 请求逻辑尺寸 ⇒ DPI 缩放生效（环境口径，见文件头说明），逐位比对不适用，跳过。
    if (frame.width != spec.width || frame.height != spec.height) {
        AURORA_TEST_SKIP("DPI 缩放环境下帧尺寸 (" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                         ") != 逻辑尺寸，逐位 golden 口径不适用");
    }

    // 比对前先断言帧尺寸 == 基线尺寸（3.2）：不一致直接尺寸错误失败，不进入像素 diff。
    const auto baseline = au::Image::load((golden::dir() / (base_name + ".png")).string());
    AURORA_TEST_REQUIRE_MSG(baseline.ok(), base_name + ".png missing or undecodable");
    AURORA_TEST_REQUIRE_MSG(frame.width == baseline.value().width && frame.height == baseline.value().height,
                            "frame size mismatch vs baseline: frame " + std::to_string(frame.width) + "x" +
                                std::to_string(frame.height) + ", baseline " + std::to_string(baseline.value().width) +
                                "x" + std::to_string(baseline.value().height));

    au::Image current;
    current.width = frame.width;
    current.height = frame.height;
    current.pixels = frame.pixels;

    const std::string fail_png =
        dump_fail_png_if_over_budget("etest_smoke_render_" + std::string{scene.id} + "-" + e2e::backend_name(backend),
                                     baseline.value(), frame, budget_entry.budget, AURORA_E2E_GOLDEN_TOL);
    const au::SnapshotDiff measured = au::compare_snapshots(baseline.value(), current, AURORA_E2E_GOLDEN_TOL);

    if (auto *ctx = testing::current_context(); ctx != nullptr) {
        std::string note = "e2e golden " + std::string{scene.id} + " on " + e2e::backend_name(backend) + ": " +
                           std::to_string(measured.pixel_diff_count) + " px / budget " +
                           std::to_string(budget_entry.budget) + ", tol " + std::to_string(AURORA_E2E_GOLDEN_TOL) +
                           ", max delta " + std::to_string(measured.max_color_delta) + " [" +
                           std::string{budget_entry.shape} + "]";
        if (!fail_png.empty()) {
            note += " fail_png=" + fail_png;
        }
        ctx->add_note(note);
    }
    append_metrics_line("golden_within_tolerance_of_software_baseline", value, frame, baseline.value(), measured,
                        budget_entry.budget, fail_png);

    // 判据单源：差异像素数 <= 场景预算即通过，失败消息含差异区域与控件归因（root 参与归因）。
    golden::compare_gpu_tolerance(base_name, current, AURORA_E2E_GOLDEN_TOL, budget_entry.budget, &root);
}

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(real_windows, RealWindowBackends, smoke_backend_values(), smoke_backend_name);

/// @brief golden 矩阵取值：注册表全部场景 × 全部真实窗口后端（注册表是场景面的单一事实来源）。
[[nodiscard]] auto golden_case_values() -> std::vector<GoldenCase> {
    std::vector<GoldenCase> values;
    for (const au::demo_scenes::SceneEntry &scene : au::demo_scenes::scene_registry()) {
        for (const e2e::Backend backend : smoke_backend_values()) {
            values.push_back(GoldenCase{.scene = &scene, .backend = backend});
        }
    }
    return values;
}

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(golden_matrix, GoldenBackends, golden_case_values(), golden_case_name);

}  // namespace aurora::test_cases::etest_smoke_render
