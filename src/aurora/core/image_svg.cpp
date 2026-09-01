// 内置 SVG 子集光栅化（集成到 Image::load / ImageView）。
//
// 支持子集（图标级 SVG）：
// - 形状：<rect>(含 rx 圆角近似)、<circle>、<ellipse>、<line>、<polygon>、<polyline>
// - 颜色：#rgb / #rrggbb / 常用命名色 / none；fill 与 stroke(+stroke-width)
// - 画布：viewBox 或 width/height 推导固有尺寸（缺省 64x64）；目标尺寸缩放光栅化
// - 不支持：<path>、渐变、变换、分组样式继承（遇到未知元素跳过，降级不失败）
//
// 设计：逐像素点内测试（图标尺寸下开销可忽略），零三方依赖，与 stb_image 同风格。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora::detail {

namespace {

struct SvgColor {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    bool none = true; ///< fill="none" 或缺省
};

/// @brief 将十六进制字符转为 0-15 的整数（非法字符返回 0）。
[[nodiscard]] auto to_hex_digit(char ch) -> int {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return 0;
}

/// @brief 解析命名色：命中则写入 c 的通道并置 none=false；未知色名将 none 置为 true（降级）。
auto apply_named_color(SvgColor &c, const std::string &s) -> void {
    if (s == "black") {
        c.r = c.g = c.b = 0;
        c.none = false;
    } else if (s == "white") {
        c.r = c.g = c.b = 255;
        c.none = false;
    } else if (s == "red") {
        c.r = 255;
        c.none = false;
    } else if (s == "green") {
        c.g = 128;
        c.none = false;
    } else if (s == "lime") {
        c.g = 255;
        c.none = false;
    } else if (s == "blue") {
        c.b = 255;
        c.none = false;
    } else if (s == "yellow") {
        c.r = 255;
        c.g = 255;
        c.none = false;
    } else if (s == "gray" || s == "grey") {
        c.r = c.g = c.b = 128;
        c.none = false;
    } else if (s == "orange") {
        c.r = 255;
        c.g = 165;
        c.none = false;
    } else if (s == "purple") {
        c.r = 128;
        c.b = 128;
        c.none = false;
    } else {
        c.none = true; // 未知色名视为 none（降级）
    }
}

/// @brief 解析颜色字符串（#rgb/#rrggbb/命名色/none）。
[[nodiscard]] auto parse_color(const std::string &s) -> SvgColor {
    SvgColor c;
    if (s.empty() || s == "none" || s == "transparent") {
        return c;
    }
    c.none = false;
    if (s.at(0) == '#') {
        if (s.size() >= 7) {
            c.r = static_cast<std::uint8_t>((to_hex_digit(s.at(1)) * 16) + to_hex_digit(s.at(2)));
            c.g = static_cast<std::uint8_t>((to_hex_digit(s.at(3)) * 16) + to_hex_digit(s.at(4)));
            c.b = static_cast<std::uint8_t>((to_hex_digit(s.at(5)) * 16) + to_hex_digit(s.at(6)));
        } else if (s.size() >= 4) {
            c.r = static_cast<std::uint8_t>(to_hex_digit(s.at(1)) * 17);
            c.g = static_cast<std::uint8_t>(to_hex_digit(s.at(2)) * 17);
            c.b = static_cast<std::uint8_t>(to_hex_digit(s.at(3)) * 17);
        }
        return c;
    }
    // 常用命名色
    apply_named_color(c, s);
    return c;
}

/// @brief 提取标签内属性值：attr="..."。找不到返回空串。
/// @note 属性名必须出现在「词边界」（前一字符为空白或 '<'）：朴素子串查找会让
///       查 "x" 命中 rx=" 内部、查 "width" 命中 stroke-width=" 内部，属性值错乱
///       （如 <rect rx="10" x="5"> 会把 10 当作 x）。
[[nodiscard]] auto attr_of(const std::string &tag, const std::string &name) -> std::string {
    const std::string needle = name + "=\"";
    std::string::size_type pos = 0;
    while ((pos = tag.find(needle, pos)) != std::string::npos) {
        const char prev = (pos == 0) ? '\0' : tag.at(pos - 1);
        if (prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' || prev == '<') {
            const auto start = pos + needle.size();
            const auto end = tag.find('"', start);
            if (end == std::string::npos) {
                return {};
            }
            return tag.substr(start, end - start);
        }
        ++pos;
    }
    return {};
}

[[nodiscard]] auto attr_f(const std::string &tag, const std::string &name, float fallback = 0.0f) -> float {
    const std::string v = attr_of(tag, name);
    if (v.empty()) {
        return fallback;
    }
    try {
        return std::stof(v);
    } catch (...) {
        return fallback;
    }
}

/// @brief 单个已解析形状（统一以点内测试光栅化）。
struct Shape {
    enum class Kind : std::uint8_t { Rect, Circle, Ellipse, Line, Polygon } kind = Kind::Rect;
    float x = 0; // rect
    float y = 0;
    float w = 0;
    float h = 0;
    float rx = 0;
    float cx = 0; // circle/ellipse
    float cy = 0;
    float r = 0;
    float ry = 0;
    float x1 = 0; // line
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float sw = 1;
    std::vector<float> pts; // polygon 顶点（x,y 交替）
    SvgColor fill;
    SvgColor stroke;
};

/// @brief 点内测试：<rect>（含 rx 圆角近似）。
[[nodiscard]] auto hit_rect(const Shape &s, float px, float py) -> bool {
    if (px < s.x || px > s.x + s.w || py < s.y || py > s.y + s.h) {
        return false;
    }
    if (s.rx > 0.0f) {
        // 圆角：四角区域做圆内测试
        const float rx = std::min(s.rx, std::min(s.w, s.h) * 0.5f);
        const float lx = s.x + rx;
        const float rxx = s.x + s.w - rx;
        const float ty = s.y + rx;
        const float by = s.y + s.h - rx;
        float dx = 0.0f;
        float dy = 0.0f;
        if (px < lx) {
            dx = lx - px;
        } else if (px > rxx) {
            dx = px - rxx;
        }
        if (py < ty) {
            dy = ty - py;
        } else if (py > by) {
            dy = py - by;
        }
        return ((dx * dx) + (dy * dy)) <= (rx * rx);
    }
    return true;
}

/// @brief 点内测试：<circle>。
[[nodiscard]] auto hit_circle(const Shape &s, float px, float py) -> bool {
    const float dx = px - s.cx;
    const float dy = py - s.cy;
    return ((dx * dx) + (dy * dy)) <= (s.r * s.r);
}

/// @brief 点内测试：<ellipse>。
[[nodiscard]] auto hit_ellipse(const Shape &s, float px, float py) -> bool {
    if (s.r <= 0.0f || s.ry <= 0.0f) {
        return false;
    }
    const float dx = (px - s.cx) / s.r;
    const float dy = (py - s.cy) / s.ry;
    return ((dx * dx) + (dy * dy)) <= 1.0f;
}

/// @brief 点内测试：<line>（距线段的距离 <= stroke-width/2）。
[[nodiscard]] auto hit_line(const Shape &s, float px, float py) -> bool {
    const float vx = s.x2 - s.x1;
    const float vy = s.y2 - s.y1;
    const float len2 = (vx * vx) + (vy * vy);
    float t = len2 > 0.0f ? (((px - s.x1) * vx) + ((py - s.y1) * vy)) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = px - (s.x1 + (t * vx));
    const float dy = py - (s.y1 + (t * vy));
    const float half = std::max(0.5f, s.sw * 0.5f);
    return ((dx * dx) + (dy * dy)) <= (half * half);
}

/// @brief 点内测试：<polygon>（射线法）。
[[nodiscard]] auto hit_polygon(const Shape &s, float px, float py) -> bool {
    const std::size_t n = s.pts.size() / 2;
    if (n < 3) {
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = s.pts.at(i * 2);
        const float yi = s.pts.at((i * 2) + 1);
        const float xj = s.pts.at(j * 2);
        const float yj = s.pts.at((j * 2) + 1);
        if (((yi > py) != (yj > py)) && (px < ((((xj - xi) * (py - yi)) / (yj - yi)) + xi))) {
            inside = !inside;
        }
    }
    return inside;
}

/// @brief 点内测试（SVG 用户坐标空间）。
[[nodiscard]] auto hit(const Shape &s, float px, float py) -> bool {
    switch (s.kind) {
    case Shape::Kind::Rect: return hit_rect(s, px, py);
    case Shape::Kind::Circle: return hit_circle(s, px, py);
    case Shape::Kind::Ellipse: return hit_ellipse(s, px, py);
    case Shape::Kind::Line: return hit_line(s, px, py);
    case Shape::Kind::Polygon: return hit_polygon(s, px, py);
    }
    return false;
}

/// @brief 将文本解析为浮点；解析失败（或前导 '+' 等不可解析形式）返回 std::nullopt。
[[nodiscard]] auto try_parse_float(const std::string &num) -> std::optional<float> {
    if (num.empty()) {
        return std::nullopt;
    }
    // std::from_chars 不接受前导 '+'，这里与 std::stof 行为对齐（用 string_view 切片避免裸指针算术）。
    const std::string_view sv{ num };
    const std::string_view body = sv.front() == '+' ? sv.substr(1) : sv;
    const char *const data = body.data();
    const char *const last = std::next(data, static_cast<std::ptrdiff_t>(body.size()));
    float value = 0.0f;
    const auto res = std::from_chars(data, last, value);
    if (res.ec != std::errc() || res.ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析 points="x1,y1 x2,y2 ..."（逗号/空白分隔均可）。
[[nodiscard]] auto parse_points(const std::string &s) -> std::vector<float> {
    std::vector<float> out;
    std::string num;
    for (const char ch : s) {
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E') {
            num.push_back(ch);
        } else if (!num.empty()) {
            if (const auto v = try_parse_float(num)) {
                out.push_back(*v);
            }
            num.clear();
        }
    }
    if (!num.empty()) {
        if (const auto v = try_parse_float(num)) {
            out.push_back(*v);
        }
    }
    if (out.size() % 2 != 0) {
        out.pop_back();
    }
    return out;
}

/// @brief 解析单个标签为形状；不支持的标签返回 std::nullopt（跳过降级）。
[[nodiscard]] auto parse_one_tag(const std::string &tag) -> std::optional<Shape> {
    Shape s;
    if (tag.starts_with("<rect")) {
        s.kind = Shape::Kind::Rect;
        s.x = attr_f(tag, "x");
        s.y = attr_f(tag, "y");
        s.w = attr_f(tag, "width");
        s.h = attr_f(tag, "height");
        s.rx = attr_f(tag, "rx");
    } else if (tag.starts_with("<circle")) {
        s.kind = Shape::Kind::Circle;
        s.cx = attr_f(tag, "cx");
        s.cy = attr_f(tag, "cy");
        s.r = attr_f(tag, "r");
    } else if (tag.starts_with("<ellipse")) {
        s.kind = Shape::Kind::Ellipse;
        s.cx = attr_f(tag, "cx");
        s.cy = attr_f(tag, "cy");
        s.r = attr_f(tag, "rx");
        s.ry = attr_f(tag, "ry");
    } else if (tag.starts_with("<line")) {
        s.kind = Shape::Kind::Line;
        s.x1 = attr_f(tag, "x1");
        s.y1 = attr_f(tag, "y1");
        s.x2 = attr_f(tag, "x2");
        s.y2 = attr_f(tag, "y2");
        s.sw = attr_f(tag, "stroke-width", 1.0f);
    } else if (tag.starts_with("<polygon") || tag.starts_with("<polyline")) {
        s.kind = Shape::Kind::Polygon;
        s.pts = parse_points(attr_of(tag, "points"));
    } else {
        return std::nullopt; // 未知/不支持标签（含 <path>）：跳过降级
    }
    s.fill = parse_color(attr_of(tag, "fill"));
    // SVG 缺省 fill=black（除 line 外）
    if (attr_of(tag, "fill").empty() && s.kind != Shape::Kind::Line) {
        s.fill = parse_color("black");
    }
    s.stroke = parse_color(attr_of(tag, "stroke"));
    // line 无 fill 语义：用 stroke 上色（缺省黑）
    if (s.kind == Shape::Kind::Line && s.stroke.none) {
        s.stroke = parse_color("black");
    }
    return s;
}

/// @brief 光栅化：逐像素映射回 SVG 用户坐标，后画的形状覆盖先画的。
[[nodiscard]] auto rasterize(const std::vector<Shape> &shapes, int out_w, int out_h, float vb_x, float vb_y, float vb_w,
                             float vb_h) -> Image {
    Image img;
    img.width = out_w;
    img.height = out_h;
    img.pixels.assign(static_cast<std::size_t>(out_w) * out_h * 4, 0); // 透明底
    const float sx = vb_w / static_cast<float>(out_w);
    const float sy = vb_h / static_cast<float>(out_h);
    for (int py = 0; py < out_h; ++py) {
        for (int px = 0; px < out_w; ++px) {
            const float ux = vb_x + ((static_cast<float>(px) + 0.5f) * sx);
            const float uy = vb_y + ((static_cast<float>(py) + 0.5f) * sy);
            for (const Shape &s : shapes) {
                const SvgColor &c = s.kind == Shape::Kind::Line ? s.stroke : s.fill;
                if (c.none) {
                    continue;
                }
                if (hit(s, ux, uy)) {
                    const std::size_t off = ((static_cast<std::size_t>(py) * out_w) + px) * 4;
                    img.pixels.at(off) = c.r;
                    img.pixels.at(off + 1) = c.g;
                    img.pixels.at(off + 2) = c.b;
                    img.pixels.at(off + 3) = c.a;
                }
            }
        }
    }
    return img;
}

} // namespace

/// @brief 单文档形状数量上限：数万形状 × 8192² 逐像素命中测试构成 CPU/内存 DoS
/// （SVG 文件属不可信输入），超限直接拒绝解码。
constexpr std::size_t AURORA_MAX_SVG_SHAPES = 4096;

auto load_image_svg(const std::vector<std::uint8_t> &buf, int target_w, int target_h) -> Result<Image> {
    const std::string doc(buf.begin(), buf.end());
    const auto svg_pos = doc.find("<svg");
    if (svg_pos == std::string::npos) {
        return make_error(ErrorCode::IOParseFailed, "load_image_svg: not an SVG document (no <svg> root)");
    }
    // 根标签属性段
    const auto svg_end = doc.find('>', svg_pos);
    const std::string root =
        doc.substr(svg_pos, svg_end == std::string::npos ? std::string::npos : svg_end - svg_pos + 1);

    // 固有尺寸：viewBox 优先，否则 width/height，缺省 64
    float vb_x = 0.0f;
    float vb_y = 0.0f;
    float vb_w = 0.0f;
    float vb_h = 0.0f;
    const std::string vb = attr_of(root, "viewBox");
    if (!vb.empty()) {
        const auto nums = parse_points(vb); // 复用数字扫描
        if (nums.size() >= 4) {
            vb_x = nums.at(0);
            vb_y = nums.at(1);
            vb_w = nums.at(2);
            vb_h = nums.at(3);
        }
    }
    if (vb_w <= 0.0f || vb_h <= 0.0f) {
        vb_w = attr_f(root, "width", 64.0f);
        vb_h = attr_f(root, "height", 64.0f);
        vb_x = 0.0f;
        vb_y = 0.0f;
    }
    if (vb_w <= 0.0f || vb_h <= 0.0f) {
        return make_error(ErrorCode::LayoutInvalidConstraints, "load_image_svg: invalid intrinsic size");
    }

    const int out_w = target_w > 0 ? target_w : static_cast<int>(std::lround(vb_w));
    const int out_h = target_h > 0 ? target_h : static_cast<int>(std::lround(vb_h));
    if (out_w <= 0 || out_h <= 0 || out_w > 8192 || out_h > 8192) {
        return make_error(ErrorCode::LayoutSizeOutOfConstraints, "load_image_svg: output size out of range");
    }

    // 解析全部支持的形状标签（文档序 = 绘制序）
    std::vector<Shape> shapes;
    std::size_t cursor = svg_end == std::string::npos ? svg_pos : svg_end;
    while (true) {
        const auto lt = doc.find('<', cursor);
        if (lt == std::string::npos) {
            break;
        }
        const auto gt = doc.find('>', lt);
        if (gt == std::string::npos) {
            break;
        }
        const std::string tag = doc.substr(lt, gt - lt + 1);
        cursor = gt + 1;

        const auto shape = parse_one_tag(tag);
        if (!shape) {
            continue;
        }
        shapes.push_back(*shape);
        if (shapes.size() > AURORA_MAX_SVG_SHAPES) {
            return make_error(ErrorCode::IOParseFailed, "load_image_svg: too many shapes (limit 4096)");
        }
    }

    return rasterize(shapes, out_w, out_h, vb_x, vb_y, vb_w, vb_h);
}

} // namespace aurora::detail
