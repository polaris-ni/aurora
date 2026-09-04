// 字体发现 / 回退链单元测试：验证内置默认字体、内存注册、缺字回退与 CJK 非 tofu。
// ── API 覆盖映射 ─────────────────────────────
// render/font_discovery.h(FontDiscovery 内置默认字体/内存注册/缺字回退链)。

#include <ft2build.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "test_harness.h"
#include FT_FREETYPE_H

#include "aurora/core/font.h"
#include "aurora/render/font_discovery.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/noto_font_data.h"
#include "aurora/render/painter.h"

using aurora::Color;
using aurora::Font;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::render::FontEngine;
using aurora::render::FontFace;
using aurora::render::noto_sans_ttf;
using aurora::render::resolve_faces;

namespace {
// 统计画布上「非背景色」像素数（证明确有字形被绘出，非空/非 tofu）。
auto count_non_bg(const Painter &p, Color bg) -> int {
    int cnt = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.m_r != bg.m_r || c.m_g != bg.m_g || c.m_b != bg.m_b) {
                ++cnt;
            }
        }
    }
    return cnt;
}

// 某码点是否至少在一个候选 face 中有真实字形（非 .notdef）。
auto has_glyph(unsigned cp) -> bool {
    const auto faces = resolve_faces("");
    return std::ranges::any_of(faces,
                               [cp](FontFace const *ff) -> bool { return FT_Get_Char_Index(ff->face, cp) != 0; });
}
}  // namespace

AURORA_TEST() {
    (void)FontEngine::instance();
    constexpr Color bg{10, 20, 30, 255};
    constexpr Color fg{240, 240, 240, 255};

    // 1) 内置默认字体：Latin 文本可度量且 > 0
    {
        const Font f{.size_pt = 16.0F};
        const float w = FontEngine::measure_width("Hello, Aurora", f);
        AURORA_TEST_CHECK(w > 0.0F);
        AURORA_LOG_INFO("test", "[1] default font measures Latin (w=", w, ") OK");
    }

    // 2) 内存注册字体（register_font_from_memory）后可用
    {
        const auto data = noto_sans_ttf();
        std::vector bytes(data.begin(), data.end());
        FontEngine::register_font_from_memory("MyEmbedded", bytes);
        const Font f{.family = "MyEmbedded", .size_pt = 16.0F};
        const float w = FontEngine::measure_width("Embedded", f);
        AURORA_TEST_CHECK(w > 0.0F);
        AURORA_LOG_INFO("test", "[2] register_font_from_memory OK (w=", w, ")");
    }

    // 3) Latin 文本实际绘制出像素
    {
        Painter p;
        p.begin(240, 48);
        p.fill_rect(
            Rect{.origin = Point{.x = 0, .y = 0},
                 .size = Size{.width = static_cast<float>(p.width()), .height = static_cast<float>(p.height())}},
            bg);
        p.draw_text(Rect{.origin = Point{.x = 4, .y = 8}, .size = Size{.width = 232, .height = 32}}, "Hello",
                    Font{.size_pt = 20.0F}, fg);
        const int cnt = count_non_bg(p, bg);
        AURORA_TEST_CHECK(cnt > 0);
        AURORA_LOG_INFO("test", "[3] Latin draws pixels (non-bg=", cnt, ") OK");
    }

    // 4) CJK 非 tofu：若系统/内置链含 CJK 字形，则绘制出像素
    {
        const std::string cjk = "你好世界";
        bool any = false;
        for (const char *q = cjk.c_str(); (*q) != 0;) {
            auto c0 = static_cast<unsigned char>(*q);
            int len = 1;  // NOLINT
            unsigned u = 0;
            if (c0 < 0x80) {
                len = 1;
                u = c0;
            } else if ((c0 & 0xE0U) == 0xC0U) {
                len = 2;
                u = (c0 & 0x1FU);
            } else if ((c0 & 0xF0U) == 0xE0U) {
                len = 3;
                u = (c0 & 0x0FU);
            } else {
                len = 4;
                u = (c0 & 0x07U);
            }
            for (int k = 1; k < len; ++k) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
                u = (u << 6U) | (static_cast<unsigned char>(q[k]) & 0x3FU);
            }
            if (has_glyph(u)) {
                any = true;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            q += len;
        }
        if (!any) {
            AURORA_LOG_INFO("test", "[4] skip: no CJK face available in this environment");
        } else {
            Painter p;
            p.begin(240, 48);
            p.fill_rect(
                Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(p.width()), .height = static_cast<float>(p.height())}},
                bg);
            p.draw_text(Rect{.origin = Point{.x = 4, .y = 8}, .size = Size{.width = 232, .height = 32}}, cjk,
                        Font{.size_pt = 20.0F}, fg);
            const int cnt = count_non_bg(p, bg);
            AURORA_TEST_CHECK(cnt > 0);
            AURORA_LOG_INFO("test", "[4] CJK draws pixels (non-bg=", cnt, ") OK");
        }
    }
}
