#pragma once

#include <array>
#include <cmath>
#include <string>

namespace aurora::render {

/**
 * @brief 内置位图字体（零依赖、跨平台）：用于软件栅格后端的真实文字绘制。
 *
 * 设计网格 8x8，'#' 表示前景像素。覆盖 ASCII 大写、数字与常用标点；
 * 小写字母在查询时自动映射为大写（保证可读性，且无需额外字形数据）。
 * 未覆盖的字符（如 CJK）降级为空格字形，由调用方已有的降级机制处理。
 *
 * 这是无字体文件依赖的最小可行方案，让 headless 渲染也能输出可读文本。
 * 真实后端（如接入系统字体 / FreeType）可替换本实现而无需改动 widget 层。
 */
struct Glyph {
    std::array<const char *, 8> rows;
};

class BitmapFont {
  public:
    static constexpr int AURORA_CELL = 8; ///< 设计网格边长（像素，scale=1 时）

    /// @brief 返回字符对应的字形（小写自动转大写，未知字符降级为空格）。
    static auto glyph(char c) -> const Glyph & {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        switch (c) {
        case 'A': return AURORA_A;
        case 'B': return AURORA_B;
        case 'C': return AURORA_C;
        case 'D': return AURORA_D;
        case 'E': return AURORA_E;
        case 'F': return AURORA_F;
        case 'G': return AURORA_G;
        case 'H': return AURORA_H;
        case 'I': return AURORA_I;
        case 'J': return AURORA_J;
        case 'K': return AURORA_K;
        case 'L': return AURORA_L;
        case 'M': return AURORA_M;
        case 'N': return AURORA_N;
        case 'O': return AURORA_O;
        case 'P': return AURORA_P;
        case 'Q': return AURORA_Q;
        case 'R': return AURORA_R;
        case 'S': return AURORA_S;
        case 'T': return AURORA_T;
        case 'U': return AURORA_U;
        case 'V': return AURORA_V;
        case 'W': return AURORA_W;
        case 'X': return AURORA_X;
        case 'Y': return AURORA_Y;
        case 'Z': return AURORA_Z;
        case '0': return AURORA_K0;
        case '1': return AURORA_K1;
        case '2': return AURORA_K2;
        case '3': return AURORA_K3;
        case '4': return AURORA_K4;
        case '5': return AURORA_K5;
        case '6': return AURORA_K6;
        case '7': return AURORA_K7;
        case '8': return AURORA_K8;
        case '9': return AURORA_K9;
        case ' ': return AURORA_SPACE;
        case '!': return AURORA_BANG;
        case '"': return AURORA_D_QUOTE;
        case '#': return AURORA_HASH;
        case '%': return AURORA_PERCENT;
        case '&': return AURORA_AMP;
        case '\'': return AURORA_S_QUOTE;
        case '(': return AURORA_L_PAREN;
        case ')': return AURORA_R_PAREN;
        case '*': return AURORA_STAR;
        case '+': return AURORA_PLUS;
        case ',': return AURORA_COMMA;
        case '-': return AURORA_DASH;
        case '.': return AURORA_DOT;
        case '/': return AURORA_SLASH;
        case ':': return AURORA_COLON;
        case ';': return AURORA_SEMICOLON;
        case '<': return AURORA_L_ANGLE;
        case '=': return AURORA_EQUAL;
        case '>': return AURORA_R_ANGLE;
        case '?': return AURORA_QUESTION;
        case '@': return AURORA_AT;
        case '[': return AURORA_L_BRACKET;
        case ']': return AURORA_R_BRACKET;
        case '^': return AURORA_CARET;
        case '_': return AURORA_UNDER;
        case '`': return AURORA_BACKTICK;
        case '{': return AURORA_L_BRACE;
        case '|': return AURORA_PIPE;
        case '}': return AURORA_R_BRACE;
        case '~': return AURORA_TILDE;
        default: return AURORA_SPACE;
        }
    }

    /// @brief 单个设计像素在给定字号下的设备像素边长。
    static auto pixel_size(float size_pt) -> int {
        return static_cast<int>(std::max(1.0f, std::round(size_pt / 12.0f)));
    }

    /// @brief 测量字符串宽度（设备像素）。
    static auto measure_width(const std::string &s, float size_pt) -> float {
        return static_cast<float>(s.size()) * static_cast<float>(AURORA_CELL) * static_cast<float>(pixel_size(size_pt));
    }

    /// @brief 测量单行高度（设备像素）。
    static auto measure_height(float size_pt) -> float {
        return static_cast<float>(AURORA_CELL) * static_cast<float>(pixel_size(size_pt));
    }

  private:
    static constexpr Glyph AURORA_SPACE = { "        ", "        ", "        ", "        ",
                                            "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_A = { "  ####  ", " ##  ## ", " ##  ## ", " ###### ",
                                        " ##  ## ", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_B = { " ###### ", " ##  ## ", " ##  ## ", " ###### ",
                                        " ##  ## ", " ##  ## ", " ###### ", "        " };
    static constexpr Glyph AURORA_C = { "  ####  ", " ##  ## ", " ##     ", " ##     ",
                                        " ##     ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_D = { " ###### ", " ##  ## ", " ##  ## ", " ##  ## ",
                                        " ##  ## ", " ##  ## ", " ###### ", "        " };
    static constexpr Glyph AURORA_E = { " ###### ", " ##  ## ", " ##     ", " ###### ",
                                        " ##     ", " ##  ## ", " ###### ", "        " };
    static constexpr Glyph AURORA_F = { " ###### ", " ##  ## ", " ##     ", " ###### ",
                                        " ##     ", " ##     ", " ##     ", "        " };
    static constexpr Glyph AURORA_G = { "  ####  ", " ##  ## ", " ##     ", " ##  ## ",
                                        " ##  ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_H = { " ##  ## ", " ##  ## ", " ##  ## ", " ###### ",
                                        " ##  ## ", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_I = { "  ####  ", "   ##   ", "   ##   ", "   ##   ",
                                        "   ##   ", "   ##   ", "  ####  ", "        " };
    static constexpr Glyph AURORA_J = { "   #### ", "    ##  ", "    ##  ", "    ##  ",
                                        " ## ##  ", " ## ##  ", "  ###   ", "        " };
    static constexpr Glyph AURORA_K = { " ##  ## ", " ##  #  ", " ## #   ", " ###    ",
                                        " ## #   ", " ##  #  ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_L = { " ##     ", " ##     ", " ##     ", " ##     ",
                                        " ##     ", " ##     ", " ###### ", "        " };
    static constexpr Glyph AURORA_M = { " ##  ## ", " ### ## ", " ## ## #", " ## ## #",
                                        " ##  ## ", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_N = { " ##  ## ", " ###  ##", " ##   ##", " ##  ###",
                                        " ##  ###", " ##   ##", " ##  ## ", "        " };
    static constexpr Glyph AURORA_O = { "  ####  ", " ##  ## ", " ##  ## ", " ##  ## ",
                                        " ##  ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_P = { " ###### ", " ##  ## ", " ##  ## ", " ###### ",
                                        " ##     ", " ##     ", " ##     ", "        " };
    static constexpr Glyph AURORA_Q = { "  ####  ", " ##  ## ", " ##  ## ", " ##  ## ",
                                        " ## ##  ", " ##  #  ", "  ### # ", "        " };
    static constexpr Glyph AURORA_R = { " ###### ", " ##  ## ", " ##  ## ", " ###### ",
                                        " ## #   ", " ##  #  ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_S = { "  ####  ", " ##  ## ", " ##     ", "  ####  ",
                                        "     ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_T = { " ###### ", "   ##   ", "   ##   ", "   ##   ",
                                        "   ##   ", "   ##   ", "   ##   ", "        " };
    static constexpr Glyph AURORA_U = { " ##  ## ", " ##  ## ", " ##  ## ", " ##  ## ",
                                        " ##  ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_V = { " ##  ## ", " ##  ## ", " ##  ## ", " ##  ## ",
                                        " ##  ## ", "  ## ## ", "   ##   ", "        " };
    static constexpr Glyph AURORA_W = { " ##  ## ", " ##  ## ", " ## ## #", " ## ## #",
                                        " ### ###", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_X = { " ##  ## ", " ##  ## ", "  ####  ", "   ##   ",
                                        "  ####  ", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_Y = { " ##  ## ", " ##  ## ", "  ####  ", "   ##   ",
                                        "   ##   ", "   ##   ", "   ##   ", "        " };
    static constexpr Glyph AURORA_Z = { " ###### ", "     ## ", "    ##  ", "   ##   ",
                                        "  ##    ", " ##     ", " ###### ", "        " };
    static constexpr Glyph AURORA_K0 = { "  ####  ", " ##  ## ", " ## ### ", " ## ### ",
                                         " ### ## ", " ### ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_K1 = { "   ##   ", "  ###   ", "   ##   ", "   ##   ",
                                         "   ##   ", "   ##   ", " ###### ", "        " };
    static constexpr Glyph AURORA_K2 = { "  ####  ", " ##  ## ", "     ## ", "    ##  ",
                                         "   ##   ", "  ##    ", " ###### ", "        " };
    static constexpr Glyph AURORA_K3 = { " ###### ", "     ## ", "    ##  ", "  ####  ",
                                         "     ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_K4 = { " ##  ## ", " ##  ## ", " ##  ## ", " ###### ",
                                         "     ## ", "     ## ", "     ## ", "        " };
    static constexpr Glyph AURORA_K5 = { " ###### ", " ##     ", " ###### ", "     ## ",
                                         "     ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_K6 = { "  ####  ", " ##     ", " ##     ", " ###### ",
                                         " ##  ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_K7 = { " ###### ", "     ## ", "    ##  ", "   ##   ",
                                         "  ##    ", "  ##    ", "  ##    ", "        " };
    static constexpr Glyph AURORA_K8 = { "  ####  ", " ##  ## ", " ##  ## ", "  ####  ",
                                         " ##  ## ", " ##  ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_K9 = { "  ####  ", " ##  ## ", " ##  ## ", "  ##### ",
                                         "     ## ", "     ## ", "  ####  ", "        " };
    static constexpr Glyph AURORA_BANG = { "   ##   ", "   ##   ", "   ##   ", "   ##   ",
                                           "   ##   ", "        ", "   ##   ", "        " };
    static constexpr Glyph AURORA_D_QUOTE = { " ##  ## ", " ##  ## ", " ##  ## ", "        ",
                                              "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_HASH = { " ##  ## ", " ###### ", " ##  ## ", " ###### ",
                                           " ##  ## ", " ###### ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_PERCENT = { " ##  ## ", " ##  ## ", "   ##   ", "  ##    ",
                                              " ##     ", " ##  ## ", " ##  ## ", "        " };
    static constexpr Glyph AURORA_AMP = { "  ##    ", " ##  ## ", " ##  #  ", "  ###   ",
                                          " #  ##  ", " ##  ## ", "  ### # ", "        " };
    static constexpr Glyph AURORA_S_QUOTE = { "   ##   ", "   ##   ", "   ##   ", "        ",
                                              "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_L_PAREN = { "   ###  ", "  ##    ", " ##     ", " ##     ",
                                              " ##     ", "  ##    ", "   ###  ", "        " };
    static constexpr Glyph AURORA_R_PAREN = { "  ###   ", "    ##  ", "     ## ", "     ## ",
                                              "     ## ", "    ##  ", "  ###   ", "        " };
    static constexpr Glyph AURORA_STAR = { "        ", " ##  ## ", "  ####  ", " ###### ",
                                           "  ####  ", " ##  ## ", "        ", "        " };
    static constexpr Glyph AURORA_PLUS = { "        ", "   ##   ", "   ##   ", " ###### ",
                                           "   ##   ", "   ##   ", "        ", "        " };
    static constexpr Glyph AURORA_COMMA = { "        ", "        ", "        ", "        ",
                                            "        ", "   ##   ", "   ##   ", "  ##    " };
    static constexpr Glyph AURORA_DASH = { "        ", "        ", "        ", " ###### ",
                                           "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_DOT = { "        ", "        ", "        ", "        ",
                                          "        ", "   ##   ", "   ##   ", "        " };
    static constexpr Glyph AURORA_SLASH = { "      ##", "     ## ", "    ##  ", "   ##   ",
                                            "  ##    ", " ##     ", " ##     ", "        " };
    static constexpr Glyph AURORA_COLON = { "        ", "   ##   ", "   ##   ", "        ",
                                            "        ", "   ##   ", "   ##   ", "        " };
    static constexpr Glyph AURORA_SEMICOLON = { "        ", "   ##   ", "   ##   ", "        ",
                                                "        ", "   ##   ", "   ##   ", "  ##    " };
    static constexpr Glyph AURORA_L_ANGLE = { "    ##  ", "   ##   ", "  ##    ", " ##     ",
                                              "  ##    ", "   ##   ", "    ##  ", "        " };
    static constexpr Glyph AURORA_EQUAL = { "        ", "        ", " ###### ", "        ",
                                            " ###### ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_R_ANGLE = { "  ##    ", "   ##   ", "    ##  ", "     ## ",
                                              "    ##  ", "   ##   ", "  ##    ", "        " };
    static constexpr Glyph AURORA_QUESTION = { "  ####  ", " ##  ## ", "     ## ", "   ##   ",
                                               "   ##   ", "        ", "   ##   ", "        " };
    static constexpr Glyph AURORA_AT = { "  ####  ", " ##  ## ", " ## ### ", " ## ####",
                                         " ## # ##", " ##    #", "  #### #", "        " };
    static constexpr Glyph AURORA_L_BRACKET = { "  ####  ", "  ##    ", "  ##    ", "  ##    ",
                                                "  ##    ", "  ##    ", "  ####  ", "        " };
    static constexpr Glyph AURORA_R_BRACKET = { "  ####  ", "    ##  ", "    ##  ", "    ##  ",
                                                "    ##  ", "    ##  ", "  ####  ", "        " };
    static constexpr Glyph AURORA_CARET = { "        ", "   ##   ", "  #  #  ", " #    # ",
                                            "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_UNDER = { "        ", "        ", "        ", "        ",
                                            "        ", "        ", " ###### ", "        " };
    static constexpr Glyph AURORA_BACKTICK = { "   ##   ", "  ##    ", " ##     ", "        ",
                                               "        ", "        ", "        ", "        " };
    static constexpr Glyph AURORA_L_BRACE = { "   ###  ", "  ##    ", "  #     ", "  ##    ",
                                              "  #     ", "  ##    ", "   ###  ", "        " };
    static constexpr Glyph AURORA_PIPE = { "   ##   ", "   ##   ", "   ##   ", "   ##   ",
                                           "   ##   ", "   ##   ", "   ##   ", "        " };
    static constexpr Glyph AURORA_R_BRACE = { "  ###   ", "    ##  ", "     #  ", "    ##  ",
                                              "     #  ", "    ##  ", "  ###   ", "        " };
    static constexpr Glyph AURORA_TILDE = { "        ", "        ", " #    # ", "  #  #  ",
                                            "   ##   ", "        ", "        ", "        " };
};

} // namespace aurora::render