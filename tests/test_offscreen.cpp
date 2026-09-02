// 目标源单元：render/offscreen.h
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_golden.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/image.h"
#include "aurora/render/offscreen.h"

#include "test_harness.h"

namespace aurora::tests::sec_golden {
namespace {

// ---- 文件工具 ----
[[maybe_unused]] auto read_file(const std::string &path, std::vector<std::uint8_t> &out) -> bool {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz < 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    if (sz > 0) {
        f.read(reinterpret_cast<char *>(out.data()), sz);
    }
    return true;
}

auto copy_file(const std::string &src, const std::string &dst) -> bool {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in || !out) {
        return false;
    }
    out << in.rdbuf();
    return true;
}

// ---- 无头渲染：把控件树布局+绘制为内存 RGBA8 缓冲（确定性软件栅格）----
auto render_to_rgba(Widget &root, const int w, const int h) -> std::vector<std::uint8_t> {
    constexpr BuildContext ctx;
    root.mount(ctx);

    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) };
    root.layout(c, ctx);

    Painter painter;
    painter.begin(w, h);
    root.paint(painter,
               Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                     .size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) } },
               ctx);

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    const std::uint8_t *d = painter.data();
    return std::vector(d, d + n);
}

// ---- 像素级 diff 统计 ----
struct DiffStat {
    long mismatched = 0;      ///< 超过容差的像素数
    int max_channel_diff = 0; ///< 单通道最大绝对差
    int first_x = -1;
    int first_y = -1;          ///< 首个差异像素坐标
    double mean_abs_err = 0.0; ///< 平均绝对差（逐像素最大通道差均值）
};

auto pixel_diff(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b, const int w, const int h,
                int tol) -> DiffStat {
    DiffStat s;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    long err_sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t *pa = &a[i * 4u];
        const std::uint8_t *pb = &b[i * 4u];
        int m = 0;
        for (int k = 0; k < 4; ++k) {
            const int d = std::abs(static_cast<int>(pa[k]) - static_cast<int>(pb[k]));
            m = std::max(d, m);
        }
        err_sum += m;
        s.max_channel_diff = std::max(m, s.max_channel_diff);
        if (m > tol) {
            ++s.mismatched;
            if (s.first_x < 0) {
                s.first_x = static_cast<int>(i % static_cast<std::size_t>(w));
                s.first_y = static_cast<int>(i / static_cast<std::size_t>(w));
            }
        }
    }
    s.mean_abs_err = static_cast<double>(err_sum) / static_cast<double>(n);
    return s;
}

} // namespace

/**
 * @brief 像素级 golden 回归测试（specification/03-layout-render.md §10.1）：确定性地把固定控件树渲染为 RGBA8，
 * 与已提交的真值 `tests/golden/` 下的 PNG 做**像素级 diff**（逐像素、逐通道），而非
 * 逐字节比对，从而稳健对抗抗锯齿/子像素字体差异带来的字节抖动。
 *
 * 行为：
 * - `AURORA_UPDATE_GOLDEN=1`：把当前渲染覆盖为新的 golden（首次生成/主动更新真值）。
 * - `AURORA_GOLDEN_MAX_DIFF=<n>`（默认 0）：单像素允许的最大通道差容差。
 * - `AURORA_GOLDEN_MAX_PIXELS=<n>`（默认 0）：允许多少个差异像素（>容差）仍判通过。
 * - 默认模式：差异像素数超过阈值即失败，并打印差异报告（差异数/最大通道差/首差异坐标）。
 *
 * 工作目录应为仓库根（CMake 已设 `WORKING_DIRECTORY` 为源码根），
 * 以便 `tests/golden/` 相对路径正确解析。
 */
static auto run() -> int {
    constexpr int w = 240;
    constexpr int h = 120;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv 在 MSVC/clang-cl 下被标为"不安全"，但它是标准可移植接口
#endif
    const char *golden_dir = std::getenv("AURORA_GOLDEN_DIR");
    const std::string dir = (golden_dir != nullptr) ? std::string(golden_dir) : std::string("tests/golden");
    const std::string out_path = dir + "/_render_out.png";
    const std::string golden_path = dir + "/golden_basic_column.png";

    Node root = Column{
        Text{ LocalizedString{ "Hello, Aurora" } },
        Text{ LocalizedString{ "Pixel golden test" } },
    };

    // ---- 更新模式：用与校验完全一致的 RGBA8 缓冲写出 PNG 真值（避免 PNG 编解码往返引入的微小 alpha 抖动）----
    if (const char *update = std::getenv("AURORA_UPDATE_GOLDEN"); (update != nullptr) && update[0] != '\0') {
        const std::vector<std::uint8_t> buf = render_to_rgba(root.widget(), w, h);
        if (!write_png(out_path.c_str(), w, h, buf.data())) {
            AURORA_LOG_ERROR("test", "[golden] write_png failed: ", out_path);
            return 2;
        }
        if (!copy_file(out_path, golden_path)) {
            AURORA_LOG_ERROR("test", "[golden] failed to update golden: ", golden_path);
            return 3;
        }
        AURORA_LOG_INFO("test", "[golden] golden updated: ", golden_path);
        return 0;
    }

    // ---- 渲染当前帧为 RGBA8 ----
    const std::vector<std::uint8_t> buf = render_to_rgba(root.widget(), w, h);

    // ---- 解码真值 PNG ----
    auto gres = Image::load(golden_path);
    if (!gres) {
        AURORA_LOG_ERROR("test", "[golden] golden missing or undecodable: ", golden_path,
                         "  reason: ", gres.error().message, "  (run with AURORA_UPDATE_GOLDEN=1 to generate it)");
        return 3;
    }
    const Image &golden = gres.value();

    if (golden.width != w || golden.height != h) {
        AURORA_LOG_ERROR("test", "[golden] dimension mismatch: golden ", golden.width, "x", golden.height,
                         " vs render ", w, "x", h);
        return 1;
    }

    // ---- 容差（默认严格：逐字节一致）----
    int tol = 0;
    if (const char *t = std::getenv("AURORA_GOLDEN_MAX_DIFF")) {
        tol = static_cast<int>(std::strtol(t, nullptr, 10));
    }
    long long max_pixels = 0;
    if (const char *mp = std::getenv("AURORA_GOLDEN_MAX_PIXELS")) {
        max_pixels = std::strtoll(mp, nullptr, 10);
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    const DiffStat s = pixel_diff(buf, golden.pixels, w, h, tol);

    if (s.mismatched > max_pixels) {
        AURORA_LOG_ERROR("test", "[golden] MISMATCH: ", s.mismatched, "/", (static_cast<long>(w) * h),
                         " pixels differ (max channel delta=", s.max_channel_diff, ", mean abs err=", s.mean_abs_err,
                         ")");
        if (s.first_x >= 0) {
            AURORA_LOG_ERROR("test", ", first diff at (", s.first_x, ",", s.first_y, ")");
        }
        AURORA_LOG_ERROR("test", "\n  tolerance: max_channel_diff=", tol, " max_pixels=", max_pixels,
                         "\n  current render written to: ", out_path, " (for visual diff)");
        // 落盘当前渲染，便于人工目检 / 与 golden 做图像 diff。
        (void)write_png(out_path.c_str(), w, h, buf.data());
        return 1;
    }

    AURORA_LOG_INFO("test", "[golden] OK: ", s.mismatched, " mismatched pixels (of ", (static_cast<long>(w) * h),
                    "), max channel delta=", s.max_channel_diff, ", mean abs err=", s.mean_abs_err);
    return 0;
}
} // namespace aurora::tests::sec_golden

AURORA_TEST() {
    // sec_golden::run() 以返回码表达 golden 比对结论（0=一致，非0=缺真值/维度不符/像素超差），
    // 必须显式判定，否则迁移后失败码会被丢弃、用例恒过。
    AURORA_TEST_CHECK_EQ(aurora::tests::sec_golden::run(), 0);
}
