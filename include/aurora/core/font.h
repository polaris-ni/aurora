#pragma once

#include <string>

namespace aurora {

/// @brief 字体描述（资源类型；实际字形解码在 render 后端完成）。
struct Font {
    std::string family = "sans-serif";  ///< 字体族名；后端找不到时回退平台默认族。
    float size_pt = 14.0F;  ///< 字号（单位：磅 pt）。
    int weight = 400;  ///< 100..900，遵循 CSS 字重约定

    /// @brief 相等比较：族名、字号、字重三者全等。
    /// @param o 右操作数。
    /// @return 三者均相等时为 true。
    [[nodiscard]] auto operator==(const Font &o) const noexcept -> bool {
        return family == o.family && size_pt == o.size_pt && weight == o.weight;
    }

    /// @brief 不等比较：对 `operator==` 取反。
    /// @param o 右操作数。
    /// @return 任一字段不相等时为 true。
    [[nodiscard]] auto operator!=(const Font &o) const noexcept -> bool {
        return !(*this == o);
    }  // NOLINT(*-redundant-parentheses)
};

}  // namespace aurora
