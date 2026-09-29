#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace aurora::render {

/// @brief CPU 字形图集（GPU-ready 缓存）。
///
/// 按 (face_id, glyph_index, px, mode) 缓存 FreeType 解码后的覆盖度位图，避免逐帧重复
/// 光栅化。覆盖度以 A8（灰度 `Supersample`）或 RGB（ClearType `FT_RENDER_MODE_LCD` 子像素）
/// 存储；LRU 淘汰防止 CJK 等大量字形导致内存无限增长。
///
/// 当前为逐字形缓存（内存连续、矩形稳定），后续可直接重排为连续纹理上传 GPU。
class GlyphAtlas {
  public:
    /// @brief 光栅化模式：与 `TextAAMode` 对应，决定条目缓冲的像素排布（A8 或 RGB 三通道）。
    enum class Mode : std::uint8_t { Gray, Lcd };

    /// @brief LRU 容量上限（超出后淘汰最久未用条目），防止 CJK 等大量字形内存膨胀。
    static constexpr std::size_t AURORA_MAX_ENTRIES = 4096;

    /// @brief 单个字形的光栅化缓存条目：包含 FreeType 解码后的覆盖度缓冲与布局度量。
    struct Entry {
        Mode mode = Mode::Gray;  ///< 覆盖度缓冲的像素排布模式（Gray=A8，Lcd=RGB 三通道）
        int left = 0;  ///< bitmap_left：字形相对起笔点的左偏移（px）
        int top = 0;  ///< bitmap_top：基线到字形顶部的偏移（px，向上为正）
        int width = 0;  ///< 逻辑列数；LCD 下 RGB 缓冲宽为 3 * width
        int rows = 0;  ///< 缓冲行数（覆盖度缓冲纵向尺寸）
        int pitch = 0;  ///< 源缓冲每行字节（诊断用，缓存副本按 width 紧密排列）
        float advance = 0.0F;  ///< 字形前进量（px）
        std::vector<std::uint8_t> buf;  ///< Gray: width*rows 的 A8；Lcd: 3*width*rows 的 RGB
    };

    /// @brief 查缓存；命中则更新 LRU 并返回条目指针，未命中返回 nullptr。
    /// @param key 字形缓存键（由 face_id/glyph_index/px/mode/italic/synthetic_bold 复合而成）。
    /// @return 命中时返回条目常驻指针（生命周期受本图集管理）；未命中返回 `nullptr`。
    [[nodiscard]] auto find(std::uint64_t key) -> const Entry *;

    /// @brief 插入并标记最近使用；超出容量时淘汰最久未用条目。
    /// @param key 字形缓存键（同 `find`）。
    /// @param entry 待缓存的字形光栅化条目（按值传入，由图集接管所有权）。
    auto insert(std::uint64_t key, Entry entry) -> void;

    /// @brief 清空全部缓存。
    auto clear() -> void;

  private:
    auto touch(std::uint64_t key) const -> void;

    std::unordered_map<std::uint64_t, Entry> map_;
    mutable std::list<std::uint64_t> lru_;
    /// @brief key → lru_ 迭代器映射，用于 O(1) LRU 更新。
    mutable std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> lru_map_;
};

}  // namespace aurora::render
