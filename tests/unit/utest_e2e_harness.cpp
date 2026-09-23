/// 测试类型: unit
/// 目标单元: tools/include/e2e/harness.h
/// 测试说明: E2E 驱动内核的自测——后端选择与 feature 宏门控的一致性、不可用后端与未建窗会话的
///           错误形状、帧推进的收敛与超时诊断、像素读回在无缓冲时的错误码。除 `Auto` 一例外全部
///           以 Headless 后端运行，不依赖真实显示环境（`Auto` 例用默认的 `Hidden` 档，即使本机
///           有显示也不把窗口推入用户视野）。

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "e2e/harness.h"
#include "framework/aurora_test.h"
#include "scenes/scene_solid_rect.h"

namespace au = aurora;
namespace e2e = aurora::e2e;

namespace aurora::test_cases::utest_e2e_harness {

namespace {

/// @brief 未实现像素读回的 Surface 桩：沿用基底默认的 `data()`（恒 nullptr）。
///
/// 这是 Release 配置下真实后端的常态（`data()` 覆写受 `AURORA_ENABLE_DEBUG` 门控），用桩来覆盖
/// 「读回能力缺失」分支，从而不必依赖真实后端或显示环境。
class NoPixelBufferSurface final : public au::Surface {
  public:
    [[nodiscard]] auto begin_frame(int width, int height) -> au::Result<bool> override {
        size_ = au::Size{.width = static_cast<float>(width), .height = static_cast<float>(height)};
        return au::Result<bool>{true};
    }
    [[nodiscard]] auto painter() -> au::Painter & override { return painter_; }
    [[nodiscard]] auto present() -> au::Result<bool> override { return au::Result<bool>{true}; }
    [[nodiscard]] auto size() const -> au::Size override { return size_; }

  private:
    au::Painter painter_;
    au::Size size_{.width = 0.0F, .height = 0.0F};
};

/// @brief 全部后端标识（枚举器无条件出现，不随 feature 宏增减，故可全量遍历）。
[[nodiscard]] auto all_backends() -> std::vector<e2e::Backend> {
    return {e2e::Backend::Auto, e2e::Backend::Headless, e2e::Backend::Win32,   e2e::Backend::D3D11,
            e2e::Backend::Glfw, e2e::Backend::X11,      e2e::Backend::Wayland, e2e::Backend::Wgpu};
}

/// @brief 错误码比对统一转 int：避免断言宏依赖枚举的流输出。
[[nodiscard]] constexpr auto code_of(au::ErrorCode code) -> int { return static_cast<int>(code); }
[[nodiscard]] auto code_of(const au::Error &error) -> int { return code_of(error.code_enum); }

}  // namespace

AURORA_TEST_CASE(backend_compiled_tracks_feature_macros) {
    // 纯编译期事实，逐后端与 feature 宏逐条比对：探测结果与构建配置必须一一对应，
    // 否则用例层按探测结果做的「跳过还是失败」裁决就会失真。
    bool headless = false;
#ifdef AURORA_BACKEND_HEADLESS
    headless = true;
#endif
    bool win32 = false;
#ifdef AURORA_BACKEND_WIN32
    win32 = true;
#endif
    bool d3d11 = false;
#ifdef AURORA_BACKEND_D3D11
    d3d11 = true;
#endif
    bool glfw = false;
#ifdef AURORA_BACKEND_GLFW
    glfw = true;
#endif
    bool x11 = false;
#ifdef AURORA_BACKEND_X11
    x11 = true;
#endif
    bool wayland = false;
#ifdef AURORA_BACKEND_WAYLAND
    wayland = true;
#endif
    bool wgpu = false;
#ifdef AURORA_BACKEND_GPU_WGPU
    wgpu = true;
#endif

    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::Headless), headless);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::Win32), win32);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::D3D11), d3d11);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::Glfw), glfw);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::X11), x11);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::Wayland), wayland);
    AURORA_TEST_CHECK_EQ(e2e::backend_compiled(e2e::Backend::Wgpu), wgpu);

    // `Auto` 交平台自动选择，永远可请求（无真实后端时回退内存帧缓冲），故恒为 true。
    AURORA_TEST_CHECK_TRUE(e2e::backend_compiled(e2e::Backend::Auto));

    // 短名是期望集声明（`AURORA_E2E_EXPECT`）与报告记账的对外契约，改名即破坏编排方。
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Auto), "auto");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Headless), "headless");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Win32), "win32");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::D3D11), "d3d11");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Glfw), "glfw");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::X11), "x11");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Wayland), "wayland");
    AURORA_TEST_CHECK_STREQ(e2e::backend_name(e2e::Backend::Wgpu), "wgpu");
}

AURORA_TEST_CASE(uncompiled_backends_fail_with_reason_not_crash) {
    // 未编译的后端在 `open()` 中必须**在建窗之前**就失败，且原因可读——用例层据此区分
    // 「本构建没有」与「环境不可用」。已编译的后端一律跳过：本用例不触碰任何 OS 窗口。
    int uncompiled = 0;
    for (const e2e::Backend backend : all_backends()) {
        if (e2e::backend_compiled(backend)) {
            continue;
        }
        ++uncompiled;
        const auto session = e2e::open(e2e::WindowSpec{.backend = backend});
        AURORA_TEST_CHECK_FALSE(session.ok());
        AURORA_TEST_CHECK_MSG(session.reason().find("not compiled") != std::string::npos,
                              std::string{"未编译后端须给出 'not compiled' 原因，实际为："} + session.reason());
        // 无窗口的会话调用读回不得崩溃，须给出可诊断错误（`ok()` 漏检的兜底）。
        AURORA_TEST_CHECK_EQ(code_of(session.read_pixels().error()), code_of(au::ErrorCode::GeneralInvalidArgument));
    }
    AURORA_TEST_CHECK_GE(uncompiled, 1);  // 本构建必然裁剪掉部分后端（至少非本平台的那些）
}

AURORA_TEST_CASE(session_without_window_reports_error_shape) {
    // 默认构造的会话 = `open()` 失败后的状态。三类帧原语都须返回 `GeneralInvalidArgument`
    // 而非空指针解引用——内核是测试基础设施，自身崩溃会让整条用例失去可归因信息。
    e2e::Session session;
    AURORA_TEST_CHECK_FALSE(session.ok());
    AURORA_TEST_CHECK_TRUE(session.reason().empty());

    const auto presented = session.present();
    AURORA_TEST_REQUIRE_FALSE(presented.ok());
    AURORA_TEST_CHECK_EQ(code_of(presented.error()), code_of(au::ErrorCode::GeneralInvalidArgument));

    const auto pumped = session.pump(3);
    AURORA_TEST_REQUIRE_FALSE(pumped.ok());
    AURORA_TEST_CHECK_EQ(code_of(pumped.error()), code_of(au::ErrorCode::GeneralInvalidArgument));

    const auto settled = session.pump_until_settled(3);
    AURORA_TEST_REQUIRE_FALSE(settled.ok());
    AURORA_TEST_CHECK_EQ(code_of(settled.error()), code_of(au::ErrorCode::GeneralInvalidArgument));

    const auto read = session.read_pixels();
    AURORA_TEST_REQUIRE_FALSE(read.ok());
    AURORA_TEST_CHECK_EQ(code_of(read.error()), code_of(au::ErrorCode::GeneralInvalidArgument));
}

AURORA_TEST_CASE(capture_frame_rejects_surface_without_pixel_buffer) {
    // 读回能力缺失不是「空帧」：须返回 unsupported 且消息沿用 `save_snapshot` 的既有措辞，
    // 使 E2E 适配层能把「不可读回」与「建窗失败」按同一口径记账。
    NoPixelBufferSurface surface;
    const auto frame = e2e::capture_frame(surface);
    AURORA_TEST_REQUIRE_FALSE(frame.ok());
    AURORA_TEST_CHECK_EQ(code_of(frame.error()), code_of(au::ErrorCode::GeneralNotSupported));
    AURORA_TEST_CHECK_MSG(frame.error().message.find("framebuffer capture unavailable") != std::string::npos,
                          "错误消息须沿用 save_snapshot 的既有措辞：" + frame.error().message);
}

AURORA_TEST_CASE(headless_session_settles_and_reads_back_pixels) {
    e2e::WindowSpec spec;
    spec.backend = e2e::Backend::Headless;
    spec.width = 320;
    spec.height = 200;
    auto session = e2e::open(spec);
    if (!session.ok()) {
        AURORA_TEST_SKIP("Headless 后端未编译进本构建：" + session.reason());
    }

    // 内存帧缓冲会话必然被判为 Headless 回退（判定方式是对实体做 dynamic_cast）。
    AURORA_TEST_CHECK_TRUE(session.fell_back_to_headless());
    AURORA_TEST_CHECK_NULL(session.surface().native_handle());

    session.mount(demo_scenes::build_solid_rect());

    // 帧预算 1：首帧必渲染（不可能是 idle 跳帧），预算内无从收敛 → 超时错误须带诊断三要素。
    const auto tight = session.pump_until_settled(1);
    AURORA_TEST_REQUIRE_FALSE(tight.ok());
    AURORA_TEST_CHECK_EQ(code_of(tight.error()), code_of(au::ErrorCode::RuntimeAsyncTimeout));
    AURORA_TEST_CHECK_MSG(tight.error().message.find("not settled") != std::string::npos,
                          "超时消息须说明未收敛：" + tight.error().message);
    AURORA_TEST_CHECK_MSG(tight.error().message.find("pending_dirty=") != std::string::npos,
                          "超时消息须含最后脏区状态：" + tight.error().message);
    AURORA_TEST_CHECK_MSG(tight.error().message.find("idle_frame=") != std::string::npos,
                          "超时消息须含 idle 帧状态：" + tight.error().message);
    AURORA_TEST_CHECK_MSG(tight.error().message.find("active_animations=") != std::string::npos,
                          "超时消息须含活跃动画状态：" + tight.error().message);

    // 正常预算：收敛并读回像素（场景契约见 scene_solid_rect.h：左红右蓝）。
    const auto settled = session.pump_until_settled(60);
    AURORA_TEST_REQUIRE(settled.ok());
    AURORA_TEST_CHECK_GE(settled.value(), 1);

    const auto read = session.read_pixels();
    AURORA_TEST_REQUIRE(read.ok());
    const e2e::Frame &frame = read.value();
    AURORA_TEST_CHECK_EQ(frame.width, spec.width);
    AURORA_TEST_CHECK_EQ(frame.height, spec.height);
    AURORA_TEST_REQUIRE_EQ(frame.pixels.size(),
                           static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height) * 4U);

    const au::Color left = e2e::pixel_at(frame, frame.width / 4, frame.height / 2);
    const au::Color right = e2e::pixel_at(frame, (frame.width * 3) / 4, frame.height / 2);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.r), 220);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.b), 40);
    AURORA_TEST_CHECK_EQ(static_cast<int>(left.a), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(right.b), 220);
    AURORA_TEST_CHECK_EQ(static_cast<int>(right.r), 40);
    AURORA_TEST_CHECK_GE(session.frame_count(), 1);
}

AURORA_TEST_CASE(auto_session_fallback_judgment_matches_entity) {
    // `Auto` 走 `create_native_window`：真实后端可用则建真实窗口，否则回退内存帧缓冲。
    // 断言「回退判定与窗口实体一致」——判为回退 ⟺ 未拿到原生句柄；不假设本机是否有显示。
    // 默认 `Hidden` 档，即使本机有显示也不把窗口推入用户视野。
    e2e::WindowSpec spec;
    spec.backend = e2e::Backend::Auto;
    spec.width = 64;
    spec.height = 48;
    auto session = e2e::open(spec);
    if (!session.ok()) {
        AURORA_TEST_SKIP("无可用真实后端且未编译 Headless 兜底：" + session.reason());
    }

    if (session.fell_back_to_headless()) {
        AURORA_TEST_CHECK_NULL(session.surface().native_handle());
    } else {
        // 真实后端：读回能力可能因 `AURORA_ENABLE_DEBUG` 未生效而缺失，须给出 unsupported 而非崩溃。
        const auto read = session.read_pixels();
        if (!read.ok()) {
            AURORA_TEST_CHECK_EQ(code_of(read.error()), code_of(au::ErrorCode::GeneralNotSupported));
        } else {
            AURORA_TEST_CHECK_GT(read.value().width, 0);
        }
    }

    // 无任何真实显示后端编译时，`auto_detect_surface()` 亦报 Headless，回退必然发生。
    if (au::auto_detect_surface() == au::SurfaceKind::Headless) {
        AURORA_TEST_CHECK_TRUE(session.fell_back_to_headless());
    }
}

}  // namespace aurora::test_cases::utest_e2e_harness
