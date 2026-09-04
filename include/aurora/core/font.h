#pragma once

#include <string>

namespace aurora {

/// @brief 字体描述（资源类型；实际字形解码在 render 后端完成）。
struct Font {
    std::string family = "sans-serif";
    float size_pt = 14.0F;
    int weight = 400;  ///< 100..900，遵循 CSS 字重约定

    [[nodiscard]] auto operator==(const Font &o) const noexcept -> bool {
        return family == o.family && size_pt == o.size_pt && weight == o.weight;
    }
    // NOLINTNEXTLINE(*-redundant-parentheses)
    [[nodiscard]] auto operator!=(const Font &o) const noexcept -> bool { return !(*this == o); }
};

}  // namespace aurora
