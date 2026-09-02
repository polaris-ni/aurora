// 目标源单元：debug/debug_backend.h + src/aurora/debug/debug_backend.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_debug_capture.cpp
//   - test_debug_capture_win32.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <filesystem>
#include <string>

#include "aurora/aurora.h"
#include "aurora/window/win32_surface.h"

#include "test_harness.h"

namespace aurora::tests::sec_debug_capture {

namespace {

// 取本测试专用的临时输出目录（避免污染仓库/默认 aurora_debug）。
auto temp_out_dir() -> std::string { return (std::filesystem::temp_directory_path() / "aurora_debug_test").string(); }

void cleanup_temp() {
    std::error_code ec;
    std::filesystem::remove_all(temp_out_dir(), ec);
}

} // namespace

static void run() {
    // ---- 1. 输出目录 API ----
    {
        debug::set_output_directory(""); // 复位为默认
        const std::string def = debug::output_directory();
        AURORA_TEST_CHECK(!def.empty());
        AURORA_TEST_CHECK(def.find("aurora_debug") != std::string::npos);

        debug::set_output_directory(temp_out_dir());
        AURORA_TEST_CHECK(debug::output_directory() == temp_out_dir());
        debug::set_output_directory(""); // 复位
    }

    // ---- 2. resolve_output_path 语义 ----
    {
        debug::set_output_directory("C:/tmp/aurora_out");
        // 纯文件名 → 落入输出目录（std::filesystem 用本机分隔符，故期望用 path 构造而非硬编码）。
        AURORA_TEST_CHECK(debug::resolve_output_path("shot.png") ==
                          (std::filesystem::path("C:/tmp/aurora_out") / "shot.png").string());
        AURORA_TEST_CHECK(debug::resolve_output_path("sub/deep.png") == "sub/deep.png");       // 相对带目录 → 原样
        AURORA_TEST_CHECK(debug::resolve_output_path("C:/abs/shot.png") == "C:/abs/shot.png"); // 绝对 → 原样
        AURORA_TEST_CHECK(debug::resolve_output_path("") == "C:/tmp/aurora_out");              // 空串 → 仅目录
        debug::set_output_directory("");
    }

    // ---- 3. Headless 帧缓冲 capture + 字节一致 ----
    {
        cleanup_temp();
        debug::set_output_directory(temp_out_dir());

        HeadlessSurface surf("", Size{ .width = 64.0f, .height = 48.0f });
        auto bf = surf.begin_frame(64, 48);
        AURORA_TEST_CHECK(bf.ok());
        auto &p = surf.painter();
        p.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 64.0f, .height = 48.0f } },
                    Color{ 10, 20, 30, 255 });

        auto cap = debug::capture(surf, "headless_shot.png");
        AURORA_TEST_CHECK(surf.data() != nullptr); // 帧缓冲已分配

#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(cap.ok());
        const std::string expected = (std::filesystem::path(temp_out_dir()) / "headless_shot.png").string();
        AURORA_TEST_CHECK(std::filesystem::exists(expected));

        auto img = Image::load(expected);
        AURORA_TEST_CHECK(img.ok());
        AURORA_TEST_CHECK(img.value().width == 64);
        AURORA_TEST_CHECK(img.value().height == 48);
        // 解码像素须与绘制意图逐字节一致：整幅为 (10,20,30,255)。
        const auto &px = img.value().pixels;
        bool identical = (px.size() == static_cast<std::size_t>(64 * 48 * 4));
        for (std::size_t i = 0; identical && i < px.size(); i += 4) {
            if (px[i + 0] != 10 || px[i + 1] != 20 || px[i + 2] != 30 || px[i + 3] != 255) {
                identical = false;
            }
        }
        AURORA_TEST_CHECK(identical);
#else
        // Release（未开 DEBUG）：capture 应返回 disabled 错误，而非崩溃或写出文件。
        AURORA_TEST_CHECK(!cap.ok());
        AURORA_TEST_CHECK(cap.error().code_enum == aurora::ErrorCode::GeneralNotSupported);
        AURORA_TEST_CHECK(
            !std::filesystem::exists((std::filesystem::path(temp_out_dir()) / "headless_shot.png").string()));
#endif
        debug::set_output_directory("");
        cleanup_temp();
    }

    // ---- 3b. Win32 真实后端帧缓冲 capture（物理像素）----
#ifdef AURORA_BACKEND_WIN32
    {
        cleanup_temp();
        debug::set_output_directory(temp_out_dir());
        Win32Surface surf(100, 80, "DbgCapWin32");
        auto bf = surf.begin_frame(100, 80); // 逻辑 dp，paint 缓冲按物理像素分配
        AURORA_TEST_CHECK(bf.ok());
        surf.painter().fill_rect(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 640.0f, .height = 480.0f } },
            Color{ 40, 50, 60, 255 });
        auto cap = debug::capture(surf, "win32_shot.png");
#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(surf.data() != nullptr); // DEBUG 下 Win32 data() 返回 painter 缓冲
        AURORA_TEST_CHECK(cap.ok());
        auto img = Image::load((std::filesystem::path(temp_out_dir()) / "win32_shot.png").string());
        AURORA_TEST_CHECK(img.ok());
        // PNG 尺寸须等于后端报告的物理帧缓冲尺寸（缩放比≠1 时 ≠ 逻辑 size()）。
        const auto fb = surf.framebuffer_size();
        AURORA_TEST_CHECK(img.value().width == static_cast<int>(fb.width));
        AURORA_TEST_CHECK(img.value().height == static_cast<int>(fb.height));
#else
        AURORA_TEST_CHECK(!cap.ok()); // Release 下 data() 回落 nullptr → disabled
#endif
        debug::set_output_directory("");
        cleanup_temp();
    }
#endif

    // ---- 4. Headless 上 OnScreenWindow 截图 → unsupported ----
    {
        HeadlessSurface surf("", Size{ .width = 32.0f, .height = 32.0f });
        (void)surf.begin_frame(32, 32);
        auto cap = debug::capture(surf, "win.png", debug::CaptureSource::OnScreenWindow);
        AURORA_TEST_CHECK(!cap.ok()); // Headless 无 OS 窗口
        AURORA_TEST_CHECK(cap.error().code_enum == aurora::ErrorCode::GeneralNotSupported);
    }

    // ---- 5. surface_state ----
    {
        HeadlessSurface surf("", Size{ .width = 100.0f, .height = 80.0f });
        (void)surf.begin_frame(100, 80);
        const aurora::Json st = debug::surface_state(surf);
#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(st["available"].get<bool>() == true);
        AURORA_TEST_CHECK(st["width"].get<int>() == 100);
        AURORA_TEST_CHECK(st["height"].get<int>() == 80);
        AURORA_TEST_CHECK(st["frame_count"].get<int>() == 0);
        AURORA_TEST_CHECK(st["has_native_window"].get<bool>() == false);
#else
        AURORA_TEST_CHECK(st["available"].get<bool>() == false);
#endif
    }
}
} // namespace aurora::tests::sec_debug_capture

#ifdef AURORA_BACKEND_WIN32
#include <windows.h>
#endif // AURORA_BACKEND_WIN32

namespace aurora::tests::sec_debug_capture_win32 {
#ifdef AURORA_BACKEND_WIN32

static void run() {
    // 真实窗口截图：绝对路径落盘（resolve_output_path 对绝对路径原样返回）。
    const std::string out = (std::filesystem::temp_directory_path() / "aurora_dbg_win32.png").string();
    {
        // 先清掉上一配置（Debug/Release）遗留的临时文件，避免存在性断言失真。
        std::error_code ec;
        std::filesystem::remove(out, ec);

        Win32Surface surf(160, 120, "DbgWinCapture");
        auto bf = surf.begin_frame(160, 120);
        AURORA_TEST_CHECK(bf.ok());
        surf.painter().fill_rect(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 160.0f, .height = 120.0f } },
            Color{ 30, 40, 50, 255 });
        auto pr = surf.present();
        AURORA_TEST_CHECK(pr.ok());

        auto cap = debug::capture(surf, out, debug::CaptureSource::OnScreenWindow);
#ifdef AURORA_ENABLE_DEBUG
        AURORA_TEST_CHECK(cap.ok()); // 真实窗口截图成功（含非客户区）
        auto img = Image::load(out);
        AURORA_TEST_CHECK(img.ok());
        // PNG 尺寸须等于含非客户区（标题栏/边框）的完整窗口外接矩形。
        auto *hwnd = static_cast<HWND>(surf.native_handle());
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        const int exp_w = wr.right - wr.left;
        const int exp_h = wr.bottom - wr.top;
        AURORA_TEST_CHECK(img.value().width == exp_w);
        AURORA_TEST_CHECK(img.value().height == exp_h);
        // 含非客户区：PNG 尺寸须大于纯逻辑客户区（160×120），证明标题栏/边框被纳入。
        AURORA_TEST_CHECK(img.value().width >= 160 && img.value().height >= 120);
        std::filesystem::remove(out, ec); // 清理临时文件
#else
        // Release（未开 DEBUG）：capture 应返回 disabled 错误，而非崩溃或写出文件。
        AURORA_TEST_CHECK(!cap.ok());
        AURORA_TEST_CHECK(cap.error().code_enum == ErrorCode::GeneralNotSupported);
        AURORA_TEST_CHECK(!std::filesystem::exists(out));
#endif
    }
}
#else
void run() { AURORA_TEST_PRINTF("skip: AURORA_BACKEND_WIN32 not compiled into this build"); }
#endif
} // namespace aurora::tests::sec_debug_capture_win32

AURORA_TEST() {
    aurora::tests::sec_debug_capture::run();
    aurora::tests::sec_debug_capture_win32::run();
}
