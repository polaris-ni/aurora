/// 测试类型: integration
/// 目标单元: src/aurora/render/gpu/gpu_gl_rhi.cpp + tests/framework/golden.h
/// 测试说明: GPU 容差 golden 层的 GL 侧（与 itest_wgpu_golden 同族、同判据词汇）：同一场景的
///           DisplayList 经 GpuGlRhi 重放并 read_pixels 读回，与软件 SSOT 基线 PNG 按场景级申报的
///           单通道容差 + 差异像素预算比对。场景与帧装配取自 tests/support/gpu_golden_scenes.h，
///           与 wgpu 侧共用同一张基线 PNG——两条 GPU 实路径对同一软件参考点的残差各自申报，
///           补齐「软件逐位 SSOT + GPU 容差」双层策略的后端对称性（GL 侧此前仅由真机探针承担）。
///           GL 读回行序自底向上（帧缓冲原序），由共享帧装配翻转归一。上下文经隐形 GLFW 窗口
///           取得：`GpuGlRhi` 的 MSAA/resolve 帧缓冲自持且不触默认帧缓冲，故窗口不参与渲染。
///           `AURORA_BACKEND_GLFW` / `AURORA_ENABLE_GLFW_GPU_GL` 未开启，或无显示环境 / GL 3.3
///           core 装载失败时按惯例落 skip 桩。

#include <cstddef>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_BACKEND_GLFW) && defined(AURORA_ENABLE_GLFW_GPU_GL)

#include <GLFW/glfw3.h>

#include "aurora/render/rhi/gpu_gl_rhi.h"
#include "framework/golden.h"
#include "support/gpu_golden_scenes.h"

namespace golden = aurora::testing::golden;
namespace scenes = aurora::testing::gpu_golden;

namespace aurora::test_cases::itest_gl_golden {
namespace {

// ---- 场景级容差带（Windows OpenGL 3.3 与 WSLg vGPU 双侧实测，预算取 ≈2.3× 余量）----
// 实测 tol 48 口径：polyline 139px / sector 109px / column 870px / chart_bar 5437px——与 wgpu 侧
// 同值到个位。两条 GPU 实路径（GLSL + 自持 MSAA FBO / WGSL + wgpu MSAA）吃同一份 Painter 三角化
// 顶点、同一套规范化的 4× 多重采样位与同样的 resolve+blend 公式，故对软件参考点的残差是同一个
// 确定性量；差异像素 >0 本身即证明未静默回退软件（回退软件则漂移为 0）。仍按后端各自申报，
// 不共享常量：一旦某侧管线换 AA 方案，只有该侧预算需要重校准。
constexpr int AURORA_GEOMETRY_TOL = 48;  // 单通道差 > tol 才算差异像素（与 wgpu 侧同词汇）
constexpr std::size_t AURORA_POLYLINE_BUDGET = 320;  // 64×64 ≈ 8% 画布（描边周长占比较高）
constexpr std::size_t AURORA_SECTOR_BUDGET = 256;  // 64×64 曲线边界同理
constexpr std::size_t AURORA_TEXT_BUDGET = 2048;  // 240×120 字形边缘 AA 与次像素定位抖动
constexpr std::size_t AURORA_CHART_BUDGET = 12800;  // 320×200 网格细线 + 小字号文本复合

/// @brief 隐形 3.3 core 上下文宿主：只为 `load_gl` 提供 current 上下文；本 TU 不走 present
///        路径（帧缓冲与读回全在 `GpuGlRhi` 自持 FBO 内完成），窗口尺寸取场景上界以免
///        `end_frame` 的默认帧缓冲 blit 越界。
class HiddenGlContext {
  public:
    HiddenGlContext() {
        // 无显示环境的失败由 GLFW 写 stderr；本 TU 以 SKIP 如实申报，无需重复播报。
        glfwSetErrorCallback([](int, const char *) {});
        if (glfwInit() == GLFW_FALSE) {
            return;
        }
        inited_ = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_SAMPLES, 4);
        window_ = glfwCreateWindow(320, 200, "itest_gl_golden", nullptr, nullptr);
        if (window_ == nullptr) {
            glfwTerminate();
            inited_ = false;
            return;
        }
        glfwMakeContextCurrent(window_);
    }
    HiddenGlContext(const HiddenGlContext &) = delete;
    auto operator=(const HiddenGlContext &) -> HiddenGlContext & = delete;
    ~HiddenGlContext() {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
        }
        if (inited_) {
            glfwTerminate();
        }
    }

    /// @brief 上下文是否就绪（false = 无显示环境，开窗失败）。
    [[nodiscard]] auto ok() const -> bool { return window_ != nullptr; }

  private:
    GLFWwindow *window_ = nullptr;
    bool inited_ = false;
};

/// @brief 逐例就地装载 GL 函数表并构造后端（`GpuGlRhi` 禁拷贝/搬移）；装载失败即 invalid。
[[nodiscard]] auto load_rhi() -> au::rhi::GpuGlRhi {
    return au::rhi::GpuGlRhi{au::rhi::load_gl(reinterpret_cast<void *(*)(const char *)>(&glfwGetProcAddress))};
}

}  // namespace

#define ITEST_GL_GPU_OR_SKIP(ctx, gpu)                                                          \
    HiddenGlContext ctx;                                                                        \
    if (!ctx.ok()) {                                                                            \
        AURORA_TEST_SKIP("显示环境不可用（隐形开窗失败），GL 容差 golden 跳过");                \
    }                                                                                           \
    au::rhi::GpuGlRhi gpu = load_rhi();                                                         \
    if (!gpu.valid()) {                                                                         \
        AURORA_TEST_SKIP("GL 3.3 core 装载失败（驱动过老 / 函数表缺项），GL 容差 golden 跳过"); \
    }

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    ITEST_GL_GPU_OR_SKIP(ctx, gpu);
    const auto current = scenes::render_display_list(gpu, 64, 64, scenes::draw_polyline, true);
    golden::compare_gpu_tolerance("painter_polyline", current, AURORA_GEOMETRY_TOL, AURORA_POLYLINE_BUDGET);
}

AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) {
    ITEST_GL_GPU_OR_SKIP(ctx, gpu);
    const auto current = scenes::render_display_list(gpu, 64, 64, scenes::draw_sector, true);
    golden::compare_gpu_tolerance("painter_sector", current, AURORA_GEOMETRY_TOL, AURORA_SECTOR_BUDGET);
}

AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    ITEST_GL_GPU_OR_SKIP(ctx, gpu);
    au::Node root = scenes::build_text_column();
    const auto current = scenes::render_tree(gpu, root, 240, 120, true);
    golden::compare_gpu_tolerance("golden_basic_column", current, AURORA_GEOMETRY_TOL, AURORA_TEXT_BUDGET, &root);
}

AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    ITEST_GL_GPU_OR_SKIP(ctx, gpu);
    au::Node root = scenes::build_bar_chart();
    const auto current = scenes::render_tree(gpu, root, 320, 200, true);
    golden::compare_gpu_tolerance("chart_bar", current, AURORA_GEOMETRY_TOL, AURORA_CHART_BUDGET, &root);
}

}  // namespace aurora::test_cases::itest_gl_golden

#else  // 后端未编译

namespace aurora::test_cases::itest_gl_golden {

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW / AURORA_ENABLE_GLFW_GPU_GL 未开启，GL 栅格通路不可用");
}
AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW / AURORA_ENABLE_GLFW_GPU_GL 未开启");
}
AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW / AURORA_ENABLE_GLFW_GPU_GL 未开启");
}
AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW / AURORA_ENABLE_GLFW_GPU_GL 未开启");
}

}  // namespace aurora::test_cases::itest_gl_golden

#endif  // AURORA_BACKEND_GLFW && AURORA_ENABLE_GLFW_GPU_GL
