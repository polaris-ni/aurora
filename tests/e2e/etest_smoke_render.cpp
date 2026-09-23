/// 测试类型: e2e
/// 目标单元: tools/include/e2e/harness.h
/// 测试说明: 真实后端冒烟（按后端矩阵展开）——对每个目标后端建**真实窗口**、渲染同一场景一帧、
///           经 `Surface::data()` 读回像素，对纯色区域中心断言期望 RGBA。这是「完整窗口生命周期
///           + 上屏链路」的端到端用例：与 `itest_gl_golden` 那类 RHI 离屏重放层不同，此处窗口
///           真实存在、present 走上屏路径。后端不可用时按 `AURORA_E2E_EXPECT` 记账——期望集内
///           判失败（编排方声明了期望而环境没兑现，必须修），期望集外跳过（设计上的不适用）。

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "e2e/harness.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"
#include "scenes/scene_solid_rect.h"

namespace au = aurora;
namespace e2e = aurora::e2e;

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

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(real_windows, RealWindowBackends, smoke_backend_values(), smoke_backend_name);

}  // namespace aurora::test_cases::etest_smoke_render
