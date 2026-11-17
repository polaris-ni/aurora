#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace aurora::render {

namespace detail {
/// @brief 仅库内使用的图集通道：供命中直通缓存在不复查哈希表的前提下读取/提升内部记录。
struct GlyphAtlasAccess;
}  // namespace detail

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
    /// @return 命中时返回条目指针；该指针**仅在下次 `insert` / `clear` 之前有效**——插入超出容量
    ///         会触发淘汰，指向被淘汰条目的指针随即悬垂。调用方不得跨插入保存本指针。
    [[nodiscard]] auto find(std::uint64_t key) -> const Entry *;

    /// @brief 插入并标记最近使用；超出容量时淘汰最久未用条目。
    /// @param key 字形缓存键（同 `find`）。
    /// @param entry 待缓存的字形光栅化条目（按值传入，由图集接管所有权）。
    /// @return 条目在本图集内的常驻指针（未命中插入后无需再次 `find` 即可直接使用）。
    /// @note 返回值与原指针同样只在下次 `insert` / `clear` 前有效（本次插入自身不淘汰新条目，
    ///       但下一次插入可能）。
    auto insert(std::uint64_t key, Entry entry) -> const Entry *;

    /// @brief 清空全部缓存。
    auto clear() -> void;

  private:
    friend struct detail::GlyphAtlasAccess;

    /// @brief 缓存记录：条目本体 + 最近使用序号。
    ///
    /// 命中只回写序号（一次哈希内的标量写），不再做链表 erase/push_front；
    /// 超容量时按序号扫描淘汰最久未用者，保证与逐次 LRU 提升完全相同的淘汰顺序。
    ///
    /// 为什么淘汰保留全表扫描而不改最小堆：溢出后的插入是**冷路径**（每新增一个字形才发生一次，
    /// 且同一次插入本就要付光栅化与缓冲拷贝），而命中是热路径。按使用序号维护堆会把 O(log n)
    /// 的比较与下沉搬到每次命中上，热路径变贵、冷路径只省一次线性比较——次数上得不偿失。
    struct Record {
        Entry entry;  ///< 字形条目本体
        std::uint64_t last_used = 0;  ///< 最近使用序号（越大越新）
    };

    /// @brief 标记记录为最近使用（使用序号的唯一写入点，`find` 与直通缓存命中共用）。
    /// @param record 目标记录。
    /// @return 该记录的条目指针。
    auto touch(Record &record) -> const Entry *;

    /// @brief 按 key 查记录；命中则标记最近使用。`find` 与内部直通通道共用的唯一查找入口。
    /// @param key 字形缓存键。
    /// @return 命中记录指针；未命中返回 nullptr。
    [[nodiscard]] auto find_record(std::uint64_t key) -> Record *;

    /// @brief 淘汰当前最久未用的一条（仅在插入后超出容量时调用）。
    auto evict_oldest() -> void;

    std::unordered_map<std::uint64_t, Record> map_;
    std::uint64_t use_clock_ = 0;  ///< 单调递增的使用序号计数器
    /// @brief 最近一次命中/插入的条目指针缓存：未命中→插入→再查找的常见序列免二次哈希。
    Record *last_hit_ = nullptr;
    std::uint64_t last_hit_key_ = 0;
    /// @brief 插入/清空世代号：每次 `insert` / `clear` 自增。
    ///
    /// 缓存方持有的记录句柄只在「同一世代内」可信：任意插入都可能淘汰旧记录使句柄悬垂，
    /// 而淘汰在容量未溢出时不发生、无法由条目数区分。故以世代整体作废，判定成本仅一次比较。
    std::uint64_t generation_ = 0;
};

namespace detail {

/// @brief 仅库内使用的图集通道（字形命中直通缓存专用，不属于公共 API 面）。
struct GlyphAtlasAccess {
    /// @brief 与 `GlyphAtlas::find` 语义完全一致（含 LRU 使用序号提升），额外给出内部记录句柄。
    /// @param atlas 目标图集。
    /// @param key 字形缓存键。
    /// @param record 命中时写入内部记录句柄（不透明，仅可回传给 `touch`）；未命中不改写。
    /// @return 命中条目指针；未命中返回 nullptr。
    [[nodiscard]] static auto lookup(GlyphAtlas &atlas, std::uint64_t key, void *&record) -> const GlyphAtlas::Entry *;

    /// @brief 复现 `find` 对内部记录的使用序号更新（缓存命中必须调用，否则淘汰顺序偏离基线）。
    /// @param atlas 目标图集（提供使用序号计数器）。
    /// @param record `lookup` 给出且世代未变的内部记录句柄。
    /// @return 该记录的条目指针。
    [[nodiscard]] static auto touch(GlyphAtlas &atlas, void *record) -> const GlyphAtlas::Entry * {
        return atlas.touch(*static_cast<GlyphAtlas::Record *>(record));
    }

    /// @brief 读取插入/清空世代号：与缓存方记录的世代不一致时，此前保存的句柄一律作废。
    /// @param atlas 目标图集。
    /// @return 当前世代号。
    [[nodiscard]] static auto generation(const GlyphAtlas &atlas) -> std::uint64_t { return atlas.generation_; }
};

}  // namespace detail

}  // namespace aurora::render
