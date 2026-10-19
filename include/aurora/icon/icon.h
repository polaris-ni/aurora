#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"

namespace aurora {

class Painter;  ///< 前向声明：避免值类型头引入重型渲染头（完整定义见 render/painter.h）。

namespace icon {

/// @brief 图标字体字形变体：由字体族名与 Unicode 码位定位一个字形。
///
/// 字体由应用经 `FontEngine::register_font` 或 `register_font_from_memory` 注册；
/// 库不内置任何图标字体，未注册族名时绘制侧返回结构化错误而非静默回退。
/// @note Thread: thread-safe
/// @note Side-effects: none
struct GlyphIcon {
    std::string font_family;  ///< 字体族名；须已注册到 `FontEngine`。
    char32_t codepoint = 0;  ///< Unicode 码位；须在该字体面内存在。

    /// @brief 相等比较：族名与码位全等。
    /// @param o 右操作数。
    /// @return 二者均相等时为 true。
    [[nodiscard]] auto operator==(const GlyphIcon &o) const -> bool {
        return font_family == o.font_family && codepoint == o.codepoint;
    }

    /// @brief 不等比较：对 `operator==` 取反。
    /// @param o 右操作数。
    /// @return 任一字段不相等时为 true。
    [[nodiscard]] auto operator!=(const GlyphIcon &o) const -> bool { return !(*this == o); }
};

/// @brief SVG 矢量资产变体：由源字节定位，按目标尺寸重光栅化。
///
/// 只走 `load_image_svg` 的内存字节入口（wasm 目标无文件系统、编解码注册表只支持固有尺寸）。
/// @note Thread: thread-safe
/// @note Side-effects: none
struct SvgIcon {
    std::vector<std::uint8_t> source;  ///< SVG 源字节；经内置 SVG 子集光栅器按目标尺寸光栅化。
    std::string source_id;  ///< 源标识（诊断与缓存键的可读部分）；可空。

    /// @brief 相等比较：源标识与源字节全等。
    /// @param o 右操作数。
    /// @return 二者均相等时为 true。
    [[nodiscard]] auto operator==(const SvgIcon &o) const -> bool {
        return source_id == o.source_id && source == o.source;
    }

    /// @brief 不等比较：对 `operator==` 取反。
    /// @param o 右操作数。
    /// @return 任一字段不相等时为 true。
    [[nodiscard]] auto operator!=(const SvgIcon &o) const -> bool { return !(*this == o); }
};

/// @brief 图标数据：`GlyphIcon` / `SvgIcon` 两变体的统一值语义描述符。
///
/// 变体互斥由 `std::variant` 结构化保证——取错变体得到空指针，而不是读到另一个变体的残留字段。
/// 可拷贝、可比较、无虚函数。位图不作变体：位图图标归既有 `Image` 控件，不在此重复建模。
/// @note Thread: thread-safe
/// @note Side-effects: none
class IconData {
  public:
    /// @brief 图标变体判别。
    enum class Kind : std::uint8_t {
        Glyph,  ///< 图标字体字形变体。
        Svg,  ///< SVG 矢量资产变体。
    };

    /// @brief 默认构造：`Glyph` 变体、空字体族、码位 0。
    IconData() = default;

    /// @brief 构造字形图标。
    /// @param font_family 字体族名（须已注册到 `FontEngine`）。
    /// @param codepoint Unicode 码位。
    /// @return 持 `GlyphIcon` 变体的图标数据。
    [[nodiscard]] static auto glyph(std::string font_family, char32_t codepoint) -> IconData;

    /// @brief 构造 SVG 图标。
    /// @param source SVG 源字节（移入，不留副本）。
    /// @param source_id 源标识，供诊断与缓存键使用；可空。
    /// @return 持 `SvgIcon` 变体的图标数据。
    [[nodiscard]] static auto svg(std::vector<std::uint8_t> source, std::string source_id = {}) -> IconData;

    /// @brief 查询当前变体。
    /// @return 当前持有的变体判别值。
    [[nodiscard]] auto kind() const -> Kind;

    /// @brief 取字形变体视图（只读）。
    /// @return 当前为 `Glyph` 变体时指向其内容，否则为空指针。
    [[nodiscard]] auto as_glyph() const -> const GlyphIcon *;

    /// @brief 取 SVG 变体视图（只读）。
    /// @return 当前为 `Svg` 变体时指向其内容，否则为空指针。
    [[nodiscard]] auto as_svg() const -> const SvgIcon *;

    /// @brief 取字形变体视图（可写）。
    /// @return 当前为 `Glyph` 变体时指向其内容，否则为空指针。
    [[nodiscard]] auto as_glyph() -> GlyphIcon *;

    /// @brief 取 SVG 变体视图（可写）。
    /// @return 当前为 `Svg` 变体时指向其内容，否则为空指针。
    [[nodiscard]] auto as_svg() -> SvgIcon *;

    /// @brief 相等比较：变体判别与内容全等。
    /// @param o 右操作数。
    /// @return 变体与内容均相同时为 true。
    [[nodiscard]] auto operator==(const IconData &o) const -> bool { return data_ == o.data_; }

    /// @brief 不等比较：对 `operator==` 取反。
    /// @param o 右操作数。
    /// @return 变体或内容任一不同时为 true。
    [[nodiscard]] auto operator!=(const IconData &o) const -> bool { return data_ != o.data_; }

  private:
    std::variant<GlyphIcon, SvgIcon> data_{GlyphIcon{}};
};

/// @brief 应用侧语义名注册表：把 ASCII snake_case 语义名映射到 `IconData`。
///
/// 库**不预置任何条目**——语义名与资产都由应用注册。查询未注册的名字返回结构化错误，
/// 并回带已注册名作为候选，供 AI 与 `validate_ui` 纠错。
/// @note Thread: main-thread only（注册与查询均为进程级表，未加锁）
/// @note Side-effects: 写进程级映射表
class IconRegistry {
  public:
    /// @brief 注册（或覆盖）一个语义名。
    /// @param name 语义名：ASCII snake_case，表达含义而非位置或尺寸。
    /// @param data 该名字对应的图标数据（按值存入）。
    static auto register_icon(std::string name, IconData data) -> void;

    /// @brief 注销一个语义名。
    /// @param name 待注销的语义名。
    /// @return 该名字原先存在时为 true，否则 false。
    [[nodiscard]] static auto unregister(std::string_view name) -> bool;

    /// @brief 按语义名查图标。
    /// @param name 语义名。
    /// @return 命中返回图标数据；未注册返回 `icon-name-not-registered` 结构化错误，
    ///         错误的 message 内含已注册名候选。
    [[nodiscard]] static auto lookup(std::string_view name) -> Result<IconData>;

    /// @brief 列出全部已注册语义名（升序）。
    /// @return 已注册名字的升序快照。
    [[nodiscard]] static auto names() -> std::vector<std::string>;

    /// @brief 清空全部注册项。
    static auto clear() -> void;
};

/// @brief 在框内直绘一个图标（字形或 SVG），按着色与光学补偿定位。
///
/// `Icon` / `IconButton` 与需要图标前导内容的控件（`Chip`）共用此绘制入口。
/// @param p 绘制上下文。
/// @param box 绘制框（已按盒尺寸约束）。
/// @param data 已解析图标数据。
/// @param color 生效着色。
/// @param optical_bias 光学中心垂直补偿（dp，参数化）。
auto paint_icon_data(Painter &p, const Rect &box, const IconData &data, const Color &color, float optical_bias) -> void;

}  // namespace icon
}  // namespace aurora
