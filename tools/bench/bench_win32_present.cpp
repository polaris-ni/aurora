// Win32 上屏诊断基准（非 CTest）：拆分「拖选帧」的 paint 与 present(GDI blit) 成本。
//
// 背景：全屏拖选高亮不跟手——字符命中 O(n²) 与逐像素白扫修掉后，剩余的面积级成本
// 候选是 present() 的整窗 SetDIBitsToDevice（脏区再小也推整个帧缓冲）。本工具在真实
// Win32 窗口上分别计时：① 整窗 blit；② 仅段落脏区的 paint-only present_root 整帧，
// 用数据决定是否值得做「脏区带状 blit」。
// 用法：./bench_win32_present（无 Win32 环境直接跳过，返回 0）
#include "aurora/aurora.h"

#include "bench_common.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif
#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::create_window;
using aurora::enable_dpi_awareness;
using aurora::Font;
using aurora::LocalizedString;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Text;
using aurora::TextAlign;
using aurora::TextProps;
using aurora::Win32Options;
using aurora::Win32Surface;
using aurora::WindowOptions;
using aurora::bench::AURORA_BENCH_DISCLAIMER;
using aurora::bench::bench_row;
using aurora::bench::ffmt;
using aurora::bench::time_ms;

// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
#ifndef AURORA_BACKEND_WIN32
    AURORA_LOG_RAW("bench", "bench_win32_present: no Win32 backend, skip\n");
    return 0;
#else
    enable_dpi_awareness();

    // 贴近全屏：请求超大逻辑尺寸，实际被钳到屏幕（begin_frame 按真实客户区分配缓冲）。
    WindowOptions wopts;
    wopts.size = Size{ .width = 4000.0f, .height = 3000.0f };
    wopts.title = "bench_win32_present";
    auto win_res = create_window(Win32Options{ wopts });
    if (!win_res) {
        AURORA_LOG_RAW("bench", "bench_win32_present: window creation failed (", win_res.error().message, "), skip\n");
        return 0;
    }
    auto win = std::move(win_res.value());

    // 与 demo_text 同构的树：长段落（Justify，多行）+ 若干短文本。
    const LocalizedString k_para = "The quick brown fox jumps over the lazy dog while a silent river flows "
                                   "beyond the quiet hills and the pale moon rises above the sleeping town. "
                                   "The quick brown fox jumps over the lazy dog while a silent river flows "
                                   "beyond the quiet hills and the pale moon rises above the sleeping town.";
    auto para = std::make_shared<Text>(TextProps{
        .content = k_para, .font = Font{ .size_pt = 15.0f }, .text_align = TextAlign::Justify, .soft_wrap = true });
    std::vector<Node> kids;
    kids.reserve(9);
    for (int i = 0; i < 8; ++i) {
        kids.emplace_back(std::make_shared<Text>(
            TextProps{ .content = LocalizedString{ "line " + std::to_string(i) + " · 文本行内容示例" },
                       .font = Font{ .size_pt = 16.0f } }));
    }
    kids.emplace_back(para);
    Node root{ std::make_shared<Column>(ColumnProps{ .children = std::move(kids) }) };

    (void)win->present_root(root); // 首帧全绘（mount + layout + paint + present）
    const Size logical = win->size();
    const float scale = win->surface().scale_factor();
    AURORA_LOG_RAW("bench", "window: ", ffmt(0, logical.width), "x", ffmt(0, logical.height), " dp @ ", ffmt(2, scale),
                   "x = ", static_cast<int>(logical.width * scale), "x", static_cast<int>(logical.height * scale),
                   " px\n");

    // ① 纯 blit：present() 整窗 SetDIBitsToDevice（不重绘）。
    bench_row("present_full_blit", time_ms([&]() -> void { (void)win->present(); }, 3, 30));

    // ② 拖选帧端到端：段落标脏 → present_root（脏区裁剪 paint + 整窗 blit）。
    bench_row("drag_frame_paint+blit", time_ms(
                                           [&]() -> void {
                                               win->mark_dirty(para->paint_bounds());
                                               (void)win->present_root(root);
                                           },
                                           3, 30));

    // ③ 全量重绘帧（对照）：force_full_redraw → present_root。
    bench_row("full_redraw_frame", time_ms(
                                       [&]() -> void {
                                           win->force_full_redraw();
                                           (void)win->present_root(root);
                                       },
                                       3, 30));

    // ④ 实验：常驻 DIB section + BitBlt 能否替代 SetDIBitsToDevice 全量路径——
    // 最大化/resize 必走全量 blit（~130ms），若 DIB section 路径显著更快则改造 present()。
    // 变体 A：BI_RGB(BGRA 原生序) section，CPU 做 RGBA→BGRA swizzle 后 BitBlt；
    // 变体 B：BI_BITFIELDS(RGBA 掩码) section，memcpy 后 BitBlt（swizzle 交给 GDI）。
    {
        auto *ws = dynamic_cast<Win32Surface *>(&win->surface());
        Painter &pt = win->surface().painter();
        if (ws != nullptr && pt.data() != nullptr) {
            const int w = pt.width();
            const int h = pt.height();
            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            HWND hwnd = static_cast<HWND>(ws->hwnd());
            HDC wdc = GetDC(hwnd);

            // 变体 A：BGRA 原生 section + swizzle
            HDC mdc_a = CreateCompatibleDC(wdc);
            BITMAPINFO bi_a{};
            bi_a.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi_a.bmiHeader.biWidth = w;
            bi_a.bmiHeader.biHeight = -h;
            bi_a.bmiHeader.biPlanes = 1;
            bi_a.bmiHeader.biBitCount = 32;
            bi_a.bmiHeader.biCompression = BI_RGB;
            void *bits_a = nullptr;
            HBITMAP dib_a = CreateDIBSection(wdc, &bi_a, DIB_RGB_COLORS, &bits_a, nullptr, 0);
            HGDIOBJ old_a = SelectObject(mdc_a, dib_a);
            // NOLINTBEGIN(bugprone-casting-through-void)
            const std::span<const std::uint32_t> src(
                static_cast<const std::uint32_t *>(static_cast<const void *>(pt.data())), n);
            const std::span<std::uint32_t> dst_a(static_cast<std::uint32_t *>(bits_a), n);
            // NOLINTEND(bugprone-casting-through-void)
            bench_row("dibA_swizzle_only", time_ms(
                                               [&]() -> void {
                                                   for (std::size_t i = 0; i < n; ++i) {
                                                       const std::uint32_t px = src[i];
                                                       dst_a[i] = (px & 0xFF00FF00u) | ((px & 0xFFu) << 16U) |
                                                                  ((px >> 16U) & 0xFFu);
                                                   }
                                               },
                                               2, 10));
            bench_row("dibA_bitblt_only", time_ms(
                                              [&]() -> void {
                                                  BitBlt(wdc, 0, 0, w, h, mdc_a, 0, 0, SRCCOPY);
                                                  GdiFlush();
                                              },
                                              2, 10));

            // 变体 B：RGBA 掩码 section + memcpy（swizzle 由 GDI 在 BitBlt 时做）
            HDC mdc_b = CreateCompatibleDC(wdc);
            struct Bmi {
                BITMAPINFOHEADER hdr;
                std::array<DWORD, 3> masks;
            } bi_b{};
            bi_b.hdr.biSize = sizeof(BITMAPINFOHEADER);
            bi_b.hdr.biWidth = w;
            bi_b.hdr.biHeight = -h;
            bi_b.hdr.biPlanes = 1;
            bi_b.hdr.biBitCount = 32;
            bi_b.hdr.biCompression = BI_BITFIELDS;
            bi_b.masks[0] = 0x000000FFu;
            bi_b.masks[1] = 0x0000FF00u;
            bi_b.masks[2] = 0x00FF0000u;
            void *bits_b = nullptr;
            HBITMAP dib_b = CreateDIBSection(
                wdc,
                static_cast<BITMAPINFO *>(static_cast<void *>(&bi_b)), // NOLINT(bugprone-casting-through-void)
                DIB_RGB_COLORS, &bits_b, nullptr, 0);
            if (dib_b != nullptr) {
                HGDIOBJ old_b = SelectObject(mdc_b, dib_b);
                bench_row("dibB_memcpy_only",
                          time_ms([&]() -> void { std::memcpy(bits_b, pt.data(), n * 4u); }, 2, 10));
                bench_row("dibB_bitblt_only", time_ms(
                                                  [&]() -> void {
                                                      BitBlt(wdc, 0, 0, w, h, mdc_b, 0, 0, SRCCOPY);
                                                      GdiFlush();
                                                  },
                                                  2, 10));
                SelectObject(mdc_b, old_b);
                DeleteObject(dib_b);
            } else {
                AURORA_LOG_RAW("bench", "| dibB (BITFIELDS section) | create failed |\n");
            }
            DeleteDC(mdc_b);
            SelectObject(mdc_a, old_a);
            DeleteObject(dib_a);
            DeleteDC(mdc_a);
            ReleaseDC(hwnd, wdc);
        }
    }

    // ⑤ D3D11 变体（仅 AURORA_BACKEND_D3D11=ON 时）：同尺寸窗口下对比
    // GPU 上屏的全量上传 present 与脏矩形增量上传 present（量化 GDI → D3D11 收益）。
#ifdef AURORA_BACKEND_D3D11
    {
        using aurora::D3D11Surface;
        auto gpu = std::make_unique<D3D11Surface>(static_cast<int>(logical.width), static_cast<int>(logical.height),
                                                  "bench d3d11", aurora::WindowStyleOptions{});
        if (gpu->is_available()) {
            gpu->set_vsync(false); // 关 vsync：计时反映上传+绘制成本，不含 vblank 等待
            const int gw = static_cast<int>(logical.width);
            const int gh = static_cast<int>(logical.height);
            (void)gpu->begin_frame(gw, gh);
            gpu->painter().fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = logical },
                                     Color{ 230, 230, 235, 255 });
            // 全量上传 + 呈现（脏区空 = 整帧）。
            bench_row("d3d11_present_full", time_ms(
                                                [&]() -> void {
                                                    gpu->set_present_dirty({});
                                                    (void)gpu->present();
                                                },
                                                3, 30));
            // 增量上传：仅一条段落大小的脏带（与拖选帧同量级）。
            const float s = gpu->scale_factor();
            const Rect band{ .origin = Point{ .x = 0.0f, .y = 100.0f * s },
                             .size = Size{ .width = logical.width * s, .height = 160.0f * s } };
            bench_row("d3d11_present_dirty_band", time_ms(
                                                      [&]() -> void {
                                                          gpu->set_present_dirty({ band });
                                                          (void)gpu->present();
                                                      },
                                                      3, 30));
        } else {
            AURORA_LOG_RAW("bench", "| d3d11 | no adapter, skip |\n");
        }
    }
#endif // AURORA_BACKEND_D3D11

    AURORA_LOG_RAW("bench", "\n", AURORA_BENCH_DISCLAIMER, "\n");
    return 0;
#endif
}
