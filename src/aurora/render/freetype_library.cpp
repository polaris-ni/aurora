#include "aurora/render/freetype_library.h"

#include "aurora/core/log.h"
#include FT_LCD_FILTER_H

namespace aurora::render {

namespace {
FT_Library g_lib = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace

auto ft_library() -> FT_Library {
    if (g_lib == nullptr) {
        if (FT_Init_FreeType(&g_lib) != 0) {
            AURORA_LOG_ERROR("font", "FT_Init_FreeType failed");
            g_lib = nullptr;
        } else {
            // 使用 LIGHT LCD filter，ClearType 子像素边缘更锐利、不发虚。
            FT_Library_SetLcdFilter(g_lib, FT_LCD_FILTER_LIGHT);
        }
    }
    return g_lib;
}

auto ft_shutdown() -> void {
    if (g_lib != nullptr) {
        FT_Done_FreeType(g_lib);
        g_lib = nullptr;
    }
}

}  // namespace aurora::render
