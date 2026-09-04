// 验证 SVG 子集集成到 Image::load（用户决策：不新增 SvgImage 控件，ImageView 直接可用）。
// 覆盖：viewBox/固有尺寸、rect/circle/ellipse/line/polygon 光栅化、颜色解析、
//       目标尺寸缩放（load_svg）、扩展名与内容嗅探、不支持标签降级。
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "aurora/core/image.h"
#include "test_harness.h"

using aurora::Image;

namespace {

auto write_file(const std::string &path, const std::string &content) -> void {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

/// 读取像素 (x,y) 的 RGBA。
auto px(const Image &img, int x, int y) -> std::array<std::uint8_t, 4> {
    const std::size_t off = ((static_cast<std::size_t>(y) * img.width) + x) * 4;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    return {img.pixels[off], img.pixels[off + 1], img.pixels[off + 2], img.pixels[off + 3]};
}

}  // namespace

AURORA_TEST() {
    // 临时目录：ctest 把 CWD 设为 build/，相对路径 "build/..." 会失效；用系统临时目录保证两种跑法都可写。
    const std::string dir = (std::filesystem::temp_directory_path() / "aurora_svg_test").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // ---- 1. rect 填充 + viewBox 固有尺寸 ----
    {
        const std::string path = dir + "/svg_rect.svg";
        write_file(path, R"(<svg viewBox="0 0 20 20"><rect x="5" y="5" width="10" height="10" fill="#ff0000"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        AURORA_TEST_CHECK(img.width == 20);
        AURORA_TEST_CHECK(img.height == 20);
        // 中心红色
        auto center = px(img, 10, 10);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(center[0] == 255 && center[1] == 0 && center[2] == 0 && center[3] == 255);
        // 角落透明
        auto corner = px(img, 1, 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(corner[3] == 0);
    }

    // ---- 2. circle + 命名色 ----
    {
        const std::string path = dir + "/svg_circle.svg";
        write_file(path, R"(<svg width="30" height="30"><circle cx="15" cy="15" r="10" fill="blue"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        AURORA_TEST_CHECK(img.width == 30);
        auto center = px(img, 15, 15);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(center[2] == 255);  // 蓝
        auto outside = px(img, 2, 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(outside[3] == 0);  // 圆外透明
    }

    // ---- 3. polygon（三角形）----
    {
        const std::string path = dir + "/svg_poly.svg";
        write_file(path, R"(<svg viewBox="0 0 20 20"><polygon points="10,2 18,18 2,18" fill="#0f0"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        // 三角形内部（底部中心）
        auto inside = px(img, 10, 15);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(inside[1] == 255);
        // 三角形外（左上角）
        auto outside = px(img, 2, 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(outside[3] == 0);
    }

    // ---- 4. line + stroke ----
    {
        const std::string path = dir + "/svg_line.svg";
        write_file(
            path,
            R"(<svg viewBox="0 0 20 20"><line x1="0" y1="10" x2="20" y2="10" stroke="black" stroke-width="4"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        auto on_line = px(img, 10, 10);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(on_line[3] == 255);  // 线上不透明
        auto off_line = px(img, 10, 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(off_line[3] == 0);
    }

    // ---- 5. load_svg 目标尺寸缩放（矢量放大）----
    {
        const std::string path = dir + "/svg_scale.svg";
        write_file(path, R"(<svg viewBox="0 0 10 10"><rect x="0" y="0" width="10" height="10" fill="#00f"/></svg>)");

        auto r = Image::load_svg(path, 100, 100);
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(r.value().width == 100);
        AURORA_TEST_CHECK(r.value().height == 100);
        auto c = px(r.value(), 50, 50);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(c[2] == 255);  // 放大后仍纯色（无插值糊化）
    }

    // ---- 6. 无扩展名但内容嗅探 <svg ----
    {
        const std::string path = dir + "/svg_sniff.dat";
        write_file(path,
                   R"(<?xml version="1.0"?><svg viewBox="0 0 8 8"><rect width="8" height="8" fill="red"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(r.value().width == 8);
    }

    // ---- 7. 不支持的 <path> 跳过降级（不失败、其余形状正常）----
    {
        const std::string path = dir + "/svg_path_skip.svg";
        write_file(
            path,
            R"(<svg viewBox="0 0 10 10"><path d="M0 0 L10 10"/><rect width="10" height="10" fill="gray"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        auto c = px(r.value(), 5, 5);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(c[0] == 128);  // rect 仍绘制
    }

    // ---- 8. 非 SVG 内容报错 ----
    {
        const std::string path = dir + "/svg_not.svg";
        write_file(path, "this is not svg at all");
        auto r = Image::load_svg(path);
        AURORA_TEST_CHECK(!r.ok());
    }

    // ---- 9. 圆角 rect（rx）----
    {
        const std::string path = dir + "/svg_rounded.svg";
        write_file(path,
                   R"(<svg viewBox="0 0 20 20"><rect x="0" y="0" width="20" height="20" rx="8" fill="black"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        // 中心不透明
        AURORA_TEST_CHECK(px(img, 10, 10).at(3) == 255);
        // 最角落被圆角裁掉（透明）
        AURORA_TEST_CHECK(px(img, 0, 0).at(3) == 0);
    }

    // ---- 10. 后画覆盖先画（文档序）----
    {
        const std::string path = dir + "/svg_zorder.svg";
        write_file(
            path,
            R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/><rect width="10" height="10" fill="blue"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        auto c = px(r.value(), 5, 5);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(c[2] == 255 && c[0] == 0);  // 蓝覆盖红
    }

    // ---- 11. ellipse 光栅化（各向异性 rx/ry）----
    {
        const std::string path = dir + "/svg_ellipse.svg";
        write_file(path, R"(<svg viewBox="0 0 20 20"><ellipse cx="10" cy="10" rx="8" ry="4" fill="#ff0000"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        AURORA_TEST_CHECK(img.width == 20 && img.height == 20);
        // 中心在椭圆内 → 红色不透明
        auto center = px(img, 10, 10);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(center[0] == 255 && center[3] == 255);
        // 角落在椭圆外 → 透明
        AURORA_TEST_CHECK(px(img, 0, 0).at(3) == 0);
        // 竖直方向超出 ry（y=0 距中心 10 > ry=4）→ 透明
        AURORA_TEST_CHECK(px(img, 10, 0).at(3) == 0);
        // 水平方向在 rx 内、竖直在 ry 内（x=16 距中心 6 < rx=8）→ 不透明
        auto horiz = px(img, 16, 10);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(horiz[0] == 255 && horiz[3] == 255);
    }

    // ---- 12. viewBox 无效 → 回退 width/height 固有尺寸 ----
    {
        // viewBox 为非数字：解析失败，回退到 width/height
        const std::string path = dir + "/svg_vb_bad.svg";
        write_file(
            path,
            R"(<svg viewBox="not-a-number" width="30" height="22"><rect width="30" height="22" fill="green"/></svg>)");
        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        AURORA_TEST_CHECK(img.width == 30);
        AURORA_TEST_CHECK(img.height == 22);
        auto c = px(img, 15, 11);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(c[1] == 128);  // 回退后整图填充 green（CSS green = #008000 → G=128）
    }
    {
        // viewBox 仅 2 个数（<4）：仍回退到 width/height
        const std::string path = dir + "/svg_vb_2.svg";
        write_file(path,
                   R"(<svg viewBox="0 0" width="12" height="14"><rect width="12" height="14" fill="blue"/></svg>)");
        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(r.value().width == 12);
        AURORA_TEST_CHECK(r.value().height == 14);
    }

    // ---- 13. 输出尺寸越界（>8192）----
    {
        const std::string path = dir + "/svg_oob.svg";
        write_file(path, R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/></svg>)");

        // 宽度超过上限 → 错误（不进入光栅化分配）
        auto rw = Image::load_svg(path, 9000, 100);
        AURORA_TEST_CHECK(!rw.ok());
        // 高度超过上限 → 错误
        auto rh = Image::load_svg(path, 100, 9000);
        AURORA_TEST_CHECK(!rh.ok());
        // 边界之上（+1）也拒绝
        auto r8193 = Image::load_svg(path, 8193, 64);
        AURORA_TEST_CHECK(!r8193.ok());
    }

    // ---- 14. 属性名词边界：rx 在前不得污染 x/y（回归：attr_of 曾让查 "x" 命中 rx=" 内部）----
    {
        const std::string path = dir + "/svg_attr_boundary.svg";
        write_file(path,
                   R"(<svg viewBox="0 0 20 20"><rect rx="8" x="5" y="5" width="10" height="10" fill="red"/></svg>)");

        auto r = Image::load(path);
        AURORA_TEST_CHECK(r.ok());
        const Image &img = r.value();
        // 正确解析（x=5,y=5,w=h=10）：rect 覆盖 [5,15)×[5,15)，圆角半径 min(rx, w/2)=5。
        // (7,12)：新矩形内部（距左上角圆心 (10,10) 的 dx=3,dy=2，13<=25 圆内）→ 不透明。
        //         若 x 被 rx 污染为 10（旧缺陷），rect 覆盖 [10,20)，此点透明。
        AURORA_TEST_CHECK(px(img, 7, 12).at(3) == 255);
        // (17,17)：新矩形外 → 透明；若 x 被污染为 10 则在旧矩形内 → 不透明。
        AURORA_TEST_CHECK(px(img, 17, 17).at(3) == 0);
    }

    // ---- 15. 形状数量上限（防恶意文档 DoS）----
    {
        std::string doc = R"(<svg viewBox="0 0 4 4">)";
        doc.reserve(std::size_t{4096} * 64);
        for (int i = 0; i < 5000; ++i) {
            doc += R"(<rect width="4" height="4" fill="red"/>)";
        }
        doc += "</svg>";
        const std::string path = dir + "/svg_too_many_shapes.svg";
        write_file(path, doc);
        auto r = Image::load(path);
        AURORA_TEST_CHECK(!r.ok());  // >4096 形状直接拒绝解码
    }
}