/// 测试类型: integration
/// 目标单元: include/aurora/core/image.h
/// 测试说明: 内置 SVG 子集光栅化与 Image::load / load_svg 集成——viewBox/固有尺寸、rect/circle/
///           ellipse/line/polygon 光栅化、fill 与 stroke 分层（描边居中、压在填充之上，polyline
///           开链而 polygon 闭链）、颜色解析、目标尺寸缩放、内容嗅探、不支持标签降级、
///           viewBox 回退、尺寸/形状数量防御

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "aurora/core/image.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_svg_image {

namespace {

using aurora::Image;

/// @brief 在当前用例唯一临时目录写入文本文件并返回路径字符串（用例结束由框架清理）。
auto write_file(const std::string &name, const std::string &content) -> std::string {
    const std::filesystem::path dir = aurora::testing::isolation::temp_dir();
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream f(file, std::ios::binary);
    f << content;
    return file.string();
}

/// @brief 读取像素 (x,y) 的 RGBA。
auto px(const Image &img, int x, int y) -> std::array<std::uint8_t, 4> {
    const std::size_t off = ((static_cast<std::size_t>(y) * img.width) + x) * 4;
    return {img.pixels[off], img.pixels[off + 1], img.pixels[off + 2], img.pixels[off + 3]};
}

}  // namespace

AURORA_TEST_CASE(svg_rect_with_viewbox_rasterizes_fill) {
    const std::string path = write_file(
        "svg_rect.svg", R"(<svg viewBox="0 0 20 20"><rect x="5" y="5" width="10" height="10" fill="#ff0000"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // viewBox 决定固有尺寸。
    AURORA_TEST_CHECK_EQ(img.width, 20);
    AURORA_TEST_CHECK_EQ(img.height, 20);
    // 中心红色、角落透明。
    const auto center = px(img, 10, 10);
    AURORA_TEST_CHECK(center[0] == 255 && center[1] == 0 && center[2] == 0 && center[3] == 255);
    AURORA_TEST_CHECK_EQ(px(img, 1, 1)[3], 0);
}

AURORA_TEST_CASE(svg_circle_named_color) {
    const std::string path = write_file(
        "svg_circle.svg", R"(<svg width="30" height="30"><circle cx="15" cy="15" r="10" fill="blue"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    AURORA_TEST_CHECK_EQ(img.width, 30);
    // 圆心蓝、圆外透明。
    AURORA_TEST_CHECK_EQ(px(img, 15, 15)[2], 255);
    AURORA_TEST_CHECK_EQ(px(img, 2, 2)[3], 0);
}

AURORA_TEST_CASE(svg_polygon_triangle) {
    const std::string path =
        write_file("svg_poly.svg", R"(<svg viewBox="0 0 20 20"><polygon points="10,2 18,18 2,18" fill="#0f0"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 三角形内部（底部中心）绿、左上角外透明。
    AURORA_TEST_CHECK_EQ(px(img, 10, 15)[1], 255);
    AURORA_TEST_CHECK_EQ(px(img, 2, 2)[3], 0);
}

AURORA_TEST_CASE(svg_line_with_stroke) {
    const std::string path = write_file(
        "svg_line.svg",
        R"(<svg viewBox="0 0 20 20"><line x1="0" y1="10" x2="20" y2="10" stroke="black" stroke-width="4"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 线上不透明、线外透明。
    AURORA_TEST_CHECK_EQ(px(img, 10, 10)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 10, 2)[3], 0);
}

AURORA_TEST_CASE(load_svg_scales_to_target_size) {
    // 矢量放大：目标尺寸生效且纯色区域不被插值糊化。
    const std::string path = write_file(
        "svg_scale.svg", R"(<svg viewBox="0 0 10 10"><rect x="0" y="0" width="10" height="10" fill="#00f"/></svg>)");

    const auto r = aurora::Image::load_svg(path, 100, 100);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().width, 100);
    AURORA_TEST_CHECK_EQ(r.value().height, 100);
    AURORA_TEST_CHECK_EQ(px(r.value(), 50, 50)[2], 255);
}

AURORA_TEST_CASE(svg_content_sniffing_without_extension) {
    // 无扩展名但内容嗅探 "<svg" 命中：仍按 SVG 解码。
    const std::string path =
        write_file("svg_sniff.dat",
                   R"(<?xml version="1.0"?><svg viewBox="0 0 8 8"><rect width="8" height="8" fill="red"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().width, 8);
}

AURORA_TEST_CASE(svg_unsupported_path_tag_skipped) {
    // 不支持的 <path> 跳过降级：不失败、其余形状正常绘制。
    const std::string path = write_file(
        "svg_path_skip.svg",
        R"(<svg viewBox="0 0 10 10"><path d="M0 0 L10 10"/><rect width="10" height="10" fill="gray"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(px(r.value(), 5, 5)[0], 128);  // rect 仍绘制
}

AURORA_TEST_CASE(svg_non_svg_content_rejected) {
    // 非 SVG 内容（无嗅探标记 / 无法解析）：结构化错误。
    const std::string path = write_file("svg_not.svg", "this is not svg at all");
    const auto r = aurora::Image::load_svg(path);
    AURORA_TEST_CHECK_FALSE(r.ok());
}

AURORA_TEST_CASE(svg_rounded_rect_rx_clips_corner) {
    const std::string path =
        write_file("svg_rounded.svg",
                   R"(<svg viewBox="0 0 20 20"><rect x="0" y="0" width="20" height="20" rx="8" fill="black"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 中心不透明，最角落被圆角裁掉（透明）。
    AURORA_TEST_CHECK_EQ(px(img, 10, 10)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 0, 0)[3], 0);
}

AURORA_TEST_CASE(svg_document_order_later_paints_over) {
    // 后画覆盖先画：文档序 = 绘制序，蓝覆盖红。
    const std::string path = write_file(
        "svg_zorder.svg",
        R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/><rect width="10" height="10" fill="blue"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const auto c = px(r.value(), 5, 5);
    AURORA_TEST_CHECK(c[2] == 255 && c[0] == 0);
}

AURORA_TEST_CASE(svg_ellipse_anisotropic_radii) {
    const std::string path = write_file(
        "svg_ellipse.svg", R"(<svg viewBox="0 0 20 20"><ellipse cx="10" cy="10" rx="8" ry="4" fill="#ff0000"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    AURORA_TEST_CHECK(img.width == 20 && img.height == 20);
    // 中心在椭圆内 → 红色不透明。
    const auto center = px(img, 10, 10);
    AURORA_TEST_CHECK(center[0] == 255 && center[3] == 255);
    // 角落在椭圆外 → 透明。
    AURORA_TEST_CHECK_EQ(px(img, 0, 0)[3], 0);
    // 竖直方向超出 ry（y=0 距中心 10 > ry=4）→ 透明。
    AURORA_TEST_CHECK_EQ(px(img, 10, 0)[3], 0);
    // 水平方向在 rx 内（x=16 距中心 6 < rx=8）且竖直在 ry 内 → 不透明。
    const auto horiz = px(img, 16, 10);
    AURORA_TEST_CHECK(horiz[0] == 255 && horiz[3] == 255);
}

AURORA_TEST_CASE(svg_invalid_viewbox_falls_back_to_size) {
    // viewBox 为非数字：解析失败，回退到 width/height 固有尺寸。
    {
        const std::string path = write_file(
            "svg_vb_bad.svg",
            R"(<svg viewBox="not-a-number" width="30" height="22"><rect width="30" height="22" fill="green"/></svg>)");
        const auto r = aurora::Image::load(path);
        AURORA_TEST_REQUIRE_TRUE(r.ok());
        const Image &img = r.value();
        AURORA_TEST_CHECK_EQ(img.width, 30);
        AURORA_TEST_CHECK_EQ(img.height, 22);
        // 回退后整图填充 green（CSS green = #008000 → G=128）。
        AURORA_TEST_CHECK_EQ(px(img, 15, 11)[1], 128);
    }
    {
        // viewBox 仅 2 个数（<4）：仍回退到 width/height。
        const std::string path =
            write_file("svg_vb_2.svg",
                       R"(<svg viewBox="0 0" width="12" height="14"><rect width="12" height="14" fill="blue"/></svg>)");
        const auto r = aurora::Image::load(path);
        AURORA_TEST_REQUIRE_TRUE(r.ok());
        AURORA_TEST_CHECK_EQ(r.value().width, 12);
        AURORA_TEST_CHECK_EQ(r.value().height, 14);
    }
}

AURORA_TEST_CASE(load_svg_rejects_out_of_range_size) {
    // 输出尺寸越界（>8192）：拒绝进入光栅化分配。
    const std::string path =
        write_file("svg_oob.svg", R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/></svg>)");

    AURORA_TEST_CHECK_FALSE(aurora::Image::load_svg(path, 9000, 100).ok());
    AURORA_TEST_CHECK_FALSE(aurora::Image::load_svg(path, 100, 9000).ok());
    AURORA_TEST_CHECK_FALSE(aurora::Image::load_svg(path, 8193, 64).ok());  // 边界之上（+1）也拒绝
}

AURORA_TEST_CASE(svg_attr_boundary_rx_does_not_pollute_xy) {
    // 属性名词边界回归：rx 在前不得污染 x/y 的解析。
    const std::string path =
        write_file("svg_attr_boundary.svg",
                   R"(<svg viewBox="0 0 20 20"><rect rx="8" x="5" y="5" width="10" height="10" fill="red"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 正确解析（x=5,y=5,w=h=10）：rect 覆盖 [5,15)×[5,15)，圆角半径 min(rx, w/2)=5。
    // (7,12)：矩形内部 → 不透明；若 x 被污染为 10（旧缺陷），rect 覆盖 [10,20)，此点透明。
    AURORA_TEST_CHECK_EQ(px(img, 7, 12)[3], 255);
    // (17,17)：矩形外 → 透明；若 x 被污染为 10 则在旧矩形内 → 不透明。
    AURORA_TEST_CHECK_EQ(px(img, 17, 17)[3], 0);
}

AURORA_TEST_CASE(svg_too_many_shapes_rejected) {
    // 形状数量上限（>4096）：防恶意文档 DoS，直接拒绝解码。
    std::string doc = R"(<svg viewBox="0 0 4 4">)";
    doc.reserve(std::size_t{4096} * 64);
    for (int i = 0; i < 5000; ++i) {
        doc += R"(<rect width="4" height="4" fill="red"/>)";
    }
    doc += "</svg>";
    const std::string path = write_file("svg_too_many_shapes.svg", doc);
    const auto r = aurora::Image::load(path);
    AURORA_TEST_CHECK_FALSE(r.ok());
}

AURORA_TEST_CASE(svg_stroke_only_polyline_paints_open_path) {
    // TC-IMAGE-004 的回归焦点：fill="none" + stroke 的折线必须绘出，且**不得**按闭合区域填充。
    const std::string path = write_file(
        "svg_polyline_stroke.svg",
        R"(<svg viewBox="0 0 100 100"><polyline points="10,80 30,20 50,80 70,20 90,80" fill="none" stroke="black" stroke-width="4"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 段 (10,80)-(30,20) 的中点 (20,50) 落在描边带上 → 黑色不透明。
    const auto on_path = px(img, 20, 50);
    AURORA_TEST_CHECK_EQ(on_path[3], 255);
    AURORA_TEST_CHECK(on_path[0] == 0 && on_path[1] == 0 && on_path[2] == 0);
    // 折线围成的「假想闭合区」内部（离任何段都 > 半宽）必须保持透明：旧实现正是在这里误按填充绘制。
    AURORA_TEST_CHECK_EQ(px(img, 30, 70)[3], 0);
}

AURORA_TEST_CASE(svg_polyline_stroke_does_not_close_polygon_stroke_does) {
    // polyline 的描边是**开**链，polygon 是闭链：同一组顶点下，收口边只属于后者。
    const std::string open = write_file(
        "svg_open.svg",
        R"(<svg viewBox="0 0 100 100"><polyline points="50,90 10,10 90,10" fill="none" stroke="black" stroke-width="4"/></svg>)");
    const std::string closed = write_file(
        "svg_closed.svg",
        R"(<svg viewBox="0 0 100 100"><polygon points="50,90 10,10 90,10" fill="none" stroke="black" stroke-width="4"/></svg>)");

    const auto ro = aurora::Image::load(open);
    const auto rc = aurora::Image::load(closed);
    AURORA_TEST_REQUIRE_TRUE(ro.ok());
    AURORA_TEST_REQUIRE_TRUE(rc.ok());
    // 收口边 (90,10)-(50,90) 的中点 (70,50)：polygon 有墨、polyline 无墨。
    AURORA_TEST_CHECK_EQ(px(rc.value(), 70, 50)[3], 255);
    AURORA_TEST_CHECK_EQ(px(ro.value(), 70, 50)[3], 0);
    // 两者共有边 (10,10)-(90,10) 上都有墨。
    AURORA_TEST_CHECK_EQ(px(rc.value(), 50, 10)[3], 255);
    AURORA_TEST_CHECK_EQ(px(ro.value(), 50, 10)[3], 255);
}

AURORA_TEST_CASE(svg_polygon_stroke_band_straddles_outline) {
    // 描边以轮廓为中心：填充内部靠边处与轮廓外侧同样宽的一圈都被描边覆盖（描边压在填充之上）。
    const std::string path = write_file(
        "svg_poly_band.svg",
        R"(<svg viewBox="0 0 100 100"><polygon points="10,10 90,10 50,90" fill="#ff0000" stroke="black" stroke-width="6"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 顶边 y=10 上下各 3：内侧与外侧均为黑色描边。
    for (const int y : {11, 8}) {
        const auto c = px(img, 49, y);
        AURORA_TEST_CHECK(c[3] == 255 && c[0] == 0);  // 描边带在轮廓两侧对称
    }
    // 深处仍是红色填充。
    const auto center = px(img, 50, 40);
    AURORA_TEST_CHECK(center[0] == 255 && center[2] == 0);
    // 远离轮廓与填充区：透明。
    AURORA_TEST_CHECK_EQ(px(img, 5, 95)[3], 0);
}

AURORA_TEST_CASE(svg_rect_and_circle_stroke_only) {
    const std::string path = write_file(
        "svg_stroke_shapes.svg",
        R"(<svg viewBox="0 0 100 60"><rect x="10" y="10" width="30" height="30" fill="none" stroke="black" stroke-width="4"/>)"
        R"(<circle cx="70" cy="25" r="15" fill="none" stroke="blue" stroke-width="4"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // rect：边上有墨、中心留空（fill=none 不再被当成填充）。
    AURORA_TEST_CHECK_EQ(px(img, 25, 10)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 25, 25)[3], 0);
    // circle：边界带不透明（蓝），圆心透明。
    AURORA_TEST_CHECK_EQ(px(img, 85, 25)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 70, 25)[3], 0);
}

AURORA_TEST_CASE(svg_ellipse_stroke_band_follows_boundary) {
    const std::string path = write_file(
        "svg_ellipse_stroke.svg",
        R"(<svg viewBox="0 0 100 100"><ellipse cx="50" cy="50" rx="40" ry="20" fill="none" stroke="black" stroke-width="4"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    // 长轴端 (10,50) 与短轴端 (50,30) 都在边界上 → 有墨。
    AURORA_TEST_CHECK_EQ(px(img, 10, 50)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 50, 30)[3], 255);
    // 圆心与长轴中点（离边界 15）→ 透明。
    AURORA_TEST_CHECK_EQ(px(img, 50, 50)[3], 0);
    AURORA_TEST_CHECK_EQ(px(img, 25, 50)[3], 0);
}

AURORA_TEST_CASE(svg_line_stroke_unchanged_and_default_black) {
    // line 只由描边承载（无填充语义）：既有行为不得因分层绘制而回归。
    const std::string path = write_file(
        "svg_line_only.svg",
        R"(<svg viewBox="0 0 20 20"><line x1="0" y1="10" x2="20" y2="10" stroke="black" stroke-width="4"/></svg>)");

    const auto r = aurora::Image::load(path);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const Image &img = r.value();
    AURORA_TEST_CHECK_EQ(px(img, 10, 10)[3], 255);
    AURORA_TEST_CHECK_EQ(px(img, 10, 2)[3], 0);
}

}  // namespace aurora::test_cases::itest_svg_image
