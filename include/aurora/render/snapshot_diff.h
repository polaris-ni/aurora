#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/**
 * @brief 快照对比结果（specification/03-layout-render.md §8.4）。
 */
struct SnapshotDiff {
    bool size_mismatch = false;  ///< 尺寸不一致（其余字段无意义）
    std::size_t pixel_diff_count = 0;  ///< 超过容差的像素数
    int max_color_delta = 0;  ///< 最大单通道差值（0..255）
    double diff_ratio = 0.0;  ///< 差异像素占比（0..1）
    Image diff_image;  ///< 差异可视化图（差异处红色，相同处原图淡化）

    /// @brief 按阈值判定是否通过（差异占比 <= max_ratio）。
    [[nodiscard]] auto passed(double max_ratio = 0.0) const -> bool {
        return !size_mismatch && diff_ratio <= max_ratio;
    }
};

namespace detail {

/// @brief 单个像素的最大单通道差值（0..255）。供 `compare_snapshots` 与区域聚合共用同一判定口径，
///        避免「入库」与「聚合」两处对容差的解释不一致。
[[nodiscard]] inline auto snapshot_pixel_delta(const Image &baseline, const Image &current, std::size_t off) -> int {
    int delta = 0;
    for (int ch = 0; ch < 4; ++ch) {
        delta = std::max(delta, std::abs(static_cast<int>(baseline.pixels[off + static_cast<std::size_t>(ch)]) -
                                         static_cast<int>(current.pixels[off + static_cast<std::size_t>(ch)])));
    }
    return delta;
}

/// @brief 两个矩形的交叠面积（0 表示不相交）。用于把差异区域归因到控件。
[[nodiscard]] inline auto rect_overlap_area(const Rect &a, const Rect &b) -> double {
    const double ix =
        static_cast<double>(std::min(a.right(), b.right())) - static_cast<double>(std::max(a.origin.x, b.origin.x));
    const double iy =
        static_cast<double>(std::min(a.bottom(), b.bottom())) - static_cast<double>(std::max(a.origin.y, b.origin.y));
    if (ix <= 0.0 || iy <= 0.0) {
        return 0.0;
    }
    return ix * iy;
}

/// @brief 并查集：把「脏网格」按四连通归并为区域。
///
/// 合并时**恒以较小的根作为根**（而非按秩），使结果与 union 的调用顺序无关 —— 遍历序改变不会改变输出，
/// 保证 golden 与快照区域内的编号稳定可比对。
class TileUnion {
  public:
    explicit TileUnion(std::size_t n) : parent_(n) {
        for (std::size_t i = 0; i < n; ++i) {
            parent_[i] = i;
        }
    }

    [[nodiscard]] auto find(std::size_t x) -> std::size_t {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];  // 路径减半压缩
            x = parent_[x];
        }
        return x;
    }

    auto unite(std::size_t a, std::size_t b) -> void {
        const std::size_t ra = find(a);
        const std::size_t rb = find(b);
        if (ra == rb) {
            return;
        }
        parent_[std::max(ra, rb)] = std::min(ra, rb);
    }

  private:
    std::vector<std::size_t> parent_;
};

}  // namespace detail

/**
 * @brief 逐像素对比两张 RGBA8 快照（specification/03-layout-render.md §8.4）。
 *
 * @param baseline  基线图
 * @param current   当前图
 * @param tolerance 单通道容差（0..255；任一 RGBA 通道差 > tolerance 记为差异像素）
 * @return 对比结果（含差异可视化图：差异像素红色、相同像素基线淡化 25%）
 *
 * 用途：CI 集成 —— golden 基线与当前渲染比对，超阈值报错；
 * CLI：`aurora-cli snapshot --compare baseline.png`。
 */
[[nodiscard]] inline auto compare_snapshots(const Image &baseline, const Image &current, int tolerance = 0)
    -> SnapshotDiff {
    SnapshotDiff out;
    if (baseline.width != current.width || baseline.height != current.height) {
        out.size_mismatch = true;
        return out;
    }
    const std::size_t total = static_cast<std::size_t>(baseline.width) * baseline.height;
    out.diff_image.width = baseline.width;
    out.diff_image.height = baseline.height;
    out.diff_image.pixels.assign(total * 4, 0);

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 所有下标访问均由 i < total 与 ch < 4 保证在已分配的 [0, total*4) 范围内；
    // 此处为逐像素快照对比热循环，使用 operator[] 避免 at() 的重复边界检查开销。
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t off = i * 4;
        const int delta = detail::snapshot_pixel_delta(baseline, current, off);
        out.max_color_delta = std::max(out.max_color_delta, delta);
        if (delta > tolerance) {
            ++out.pixel_diff_count;
            // 差异处标红
            out.diff_image.pixels[off] = 255;
            out.diff_image.pixels[off + 1] = 0;
            out.diff_image.pixels[off + 2] = 0;
            out.diff_image.pixels[off + 3] = 255;
        } else {
            // 相同处基线淡化 25%（保留轮廓便于人工核对）
            out.diff_image.pixels[off] = static_cast<std::uint8_t>(baseline.pixels[off] / 4);
            out.diff_image.pixels[off + 1] = static_cast<std::uint8_t>(baseline.pixels[off + 1] / 4);
            out.diff_image.pixels[off + 2] = static_cast<std::uint8_t>(baseline.pixels[off + 2] / 4);
            out.diff_image.pixels[off + 3] = 255;
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    out.diff_ratio = total > 0 ? static_cast<double>(out.pixel_diff_count) / static_cast<double>(total) : 0.0;
    return out;
}

/**
 * @brief 差异区域聚合选项（specification/03-layout-render.md §8.4）。
 */
struct DiffRegionOptions {
    int tile_size = 8;  ///< 网格边长（像素，左上角对齐）。<= 0 时退化为逐像素（等价于边长 1）。
    double tile_dirty_ratio = 0.0;  ///< 网格内差异像素占比**大于**该值才算脏网格。默认 0
                                    ///< 即「有一处差异即脏」——诊断工具应当宁可多报不可漏报；
                                    ///< 想过滤孤立噪点可调高（如 0.5 要求网格内过半像素有差异）。
    std::size_t min_region_pixels = 1;  ///< 差异像素数少于该值的区域被丢弃（滤碎块）。默认不丢弃。
};

/**
 * @brief 差异区域：把逐像素的 diff mask 按网格归并后的规整矩形（specification/03-layout-render.md §8.4）。
 *
 * 存在的意义：`SnapshotDiff` 只告诉「差异占 1.97%」，回答不了「差异**在哪儿**」。
 * 有了区域，AI 才能进一步把区域归因到具体控件（见 `attribute_diff_regions`）。
 *
 * 坐标以**图像像素**为单位、左上角为原点，复用既有 `Rect`（`core/types.h`）—— 本库没有整数矩形类型，
 * 为避免仅为诊断目的新增一个 `RectI`（占用公共 API 预算），像素值以 float 承载，取用者按整点理解即可。
 */
struct DiffRegion {
    Rect bounds;  ///< 区域矩形（像素坐标）。等于其全部脏网格的包络。
    std::size_t diff_pixels = 0;  ///< 区域内超容差的像素数（精确值，不含因网格对齐引入的填充）
    double coverage = 0.0;  ///< diff_pixels 占本区域面积之比；低表示差异在区域内很稀疏
    int max_color_delta = 0;  ///< 区域内最大单通道色差（0..255）
};

/**
 * @brief 把两张快照的差异按网格聚合成若干矩形区域（specification/03-layout-render.md §8.4）。
 *
 * 算法（确定性、单次线性扫描）：
 *   1. 按 `tile_size` 切网格，统计每格差异像素数，超过 `tile_dirty_ratio` 的记为脏格；
 *   2. 对脏格做**四连通**归并（并查集，`TileUnion`），每个连通块取全格包络成一个矩形；
 *   3. 按 `min_region_pixels` 过滤碎块；结果按 `diff_pixels` 降序，相等时按（y, x）升序。
 *
 * 为什么是网格归并而不是 flood-fill 连通域：前者 O(n) 且**无递归栈风险**，输出的矩形天然规整，
 * 且网格本身就是「空间容差」——相邻但相隔一两个像素的差异会被并成一块，这正符合人的直觉
 * （真正的像素级连通域会把抗锯齿边缘切成大量细碎区域，反而不利于人类或 LLM 阅读）。
 *
 * @param baseline  基线图
 * @param current   当前图
 * @param tolerance 单通道容差，与 `compare_snapshots` 同口径
 * @param opt       聚合选项
 * @return 区域列表（按差异像素降序）。尺寸不一致或空图时返回空列表。
 *
 * @note Thread: safe, no shared state
 * @note Side-effects: none
 */
[[nodiscard]] inline auto cluster_diff_regions(const Image &baseline, const Image &current, int tolerance = 0,
                                               const DiffRegionOptions &opt = {}) -> std::vector<DiffRegion> {
    if (baseline.width != current.width || baseline.height != current.height) {
        return {};
    }
    const int width = baseline.width;
    const int height = baseline.height;
    if (width <= 0 || height <= 0) {
        return {};
    }

    const auto uw = static_cast<std::size_t>(width);
    const auto uh = static_cast<std::size_t>(height);
    const int tile = opt.tile_size > 0 ? opt.tile_size : 1;
    const int tiles_x = (width + tile - 1) / tile;
    const int tiles_y = (height + tile - 1) / tile;
    const auto utx = static_cast<std::size_t>(tiles_x);
    const std::size_t tile_count = utx * static_cast<std::size_t>(tiles_y);

    // ── 第一遍：统计每个网格的像素数与差异像素数 ──
    std::vector<std::size_t> tile_total(tile_count, 0);
    std::vector<std::size_t> tile_diff(tile_count, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 下标访问均由 y < height / x < width / off + ch < uh*uw*4 保证在已分配范围内；
    // 此处为逐像素热循环，使用 operator[] 避免 at() 的重复边界检查开销。
    for (std::size_t y = 0; y < uh; ++y) {
        const std::size_t row = (y / static_cast<std::size_t>(tile)) * utx;  // 注意：这里是**网格行**，不是像素行
        for (std::size_t x = 0; x < uw; ++x) {
            const std::size_t t = row + x / static_cast<std::size_t>(tile);
            ++tile_total[t];
            const std::size_t off = (y * uw + x) * 4U;
            if (detail::snapshot_pixel_delta(baseline, current, off) > tolerance) {
                ++tile_diff[t];
            }
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    // ── 标记脏格并行优先四连通归并 ──
    std::vector<bool> dirty(tile_count, false);
    std::vector<std::size_t> comp_of(tile_count, tile_count);  // tile_count 作为「未归属」哨兵
    std::vector<std::size_t> min_x;
    std::vector<std::size_t> min_y;
    std::vector<std::size_t> max_x;
    std::vector<std::size_t> max_y;
    detail::TileUnion uf(tile_count);

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            const auto t = static_cast<std::size_t>(ty) * utx + static_cast<std::size_t>(tx);
            const auto effective =
                tile_total[t] > 0 ? static_cast<double>(tile_diff[t]) / static_cast<double>(tile_total[t]) : 0.0;
            if (tile_diff[t] == 0 || effective <= opt.tile_dirty_ratio) {
                continue;
            }
            dirty[t] = true;
            if (tx > 0 && dirty[t - 1]) {
                uf.unite(t, t - 1);
            }
            if (ty > 0 && dirty[t - utx]) {
                uf.unite(t, t - utx);
            }
        }
    }

    // ── 给每个连通块编号并累计像素包络（行优先遍历，编号确定） ──
    for (std::size_t t = 0; t < tile_count; ++t) {
        if (!dirty[t]) {
            continue;
        }
        const std::size_t root = uf.find(t);
        if (comp_of[root] == tile_count) {
            comp_of[root] = min_x.size();
            const auto ty = t / utx;
            const auto tx = t % utx;
            min_x.push_back(tx);
            max_x.push_back(tx);
            min_y.push_back(ty);
            max_y.push_back(ty);
        }
        const std::size_t c = comp_of[root];
        const auto ty = t / utx;
        const auto tx = t % utx;
        min_x[c] = std::min(min_x[c], tx);
        max_x[c] = std::max(max_x[c], tx);
        min_y[c] = std::min(min_y[c], ty);
        max_y[c] = std::max(max_y[c], ty);
    }

    if (min_x.empty()) {
        return {};
    }

    // ── 第二遍：按像素精确累计每个区域的差异数与最大色差 ──
    std::vector<std::size_t> region_pixels(min_x.size(), 0);
    std::vector<int> region_delta(min_x.size(), 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 同第一遍：下标由循环边界与已初始化的容器长度保证。
    for (std::size_t y = 0; y < uh; ++y) {
        const std::size_t row = (y / static_cast<std::size_t>(tile)) * utx;  // 网格行，同第一遍
        for (std::size_t x = 0; x < uw; ++x) {
            const std::size_t off = (y * uw + x) * 4U;
            const int delta = detail::snapshot_pixel_delta(baseline, current, off);
            if (delta <= tolerance) {
                continue;
            }
            const std::size_t t = row + x / static_cast<std::size_t>(tile);
            if (!dirty[t]) {
                continue;  // 干净格里的孤立差异像素不归属任何区域（可由 min_region_pixels 另行拾取）
            }
            const std::size_t c = comp_of[uf.find(t)];
            ++region_pixels[c];
            region_delta[c] = std::max(region_delta[c], delta);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    // ── 组装结果 ──
    std::vector<DiffRegion> out;
    out.reserve(min_x.size());
    for (std::size_t c = 0; c < min_x.size(); ++c) {
        if (region_pixels[c] < opt.min_region_pixels) {
            continue;
        }
        const auto x0 = static_cast<float>(min_x[c] * static_cast<std::size_t>(tile));
        const auto y0 = static_cast<float>(min_y[c] * static_cast<std::size_t>(tile));
        const auto x1 =
            std::min(static_cast<float>(uw), static_cast<float>((max_x[c] + 1) * static_cast<std::size_t>(tile)));
        const auto y1 =
            std::min(static_cast<float>(uh), static_cast<float>((max_y[c] + 1) * static_cast<std::size_t>(tile)));
        DiffRegion r;
        r.bounds = Rect{.origin = Point{.x = x0, .y = y0}, .size = Size{.width = x1 - x0, .height = y1 - y0}};
        r.diff_pixels = region_pixels[c];
        r.coverage = r.bounds.size.width > 0.0F && r.bounds.size.height > 0.0F
                         ? static_cast<double>(region_pixels[c]) /
                               (static_cast<double>(r.bounds.size.width) * static_cast<double>(r.bounds.size.height))
                         : 0.0;
        r.max_color_delta = region_delta[c];
        out.push_back(r);
    }

    std::sort(out.begin(), out.end(), [](const DiffRegion &a, const DiffRegion &b) {
        if (a.diff_pixels != b.diff_pixels) {
            return a.diff_pixels > b.diff_pixels;
        }
        if (a.bounds.origin.y != b.bounds.origin.y) {
            return a.bounds.origin.y < b.bounds.origin.y;
        }
        return a.bounds.origin.x < b.bounds.origin.x;
    });
    return out;
}

/**
 * @brief 控件的布局盒快照（纯值数据，specification/03-layout-render.md §8.4）。
 *
 * 刻意做成**纯值数据而非 `Node&`**：归因只需「控件在哪儿、叫什么、怎么称呼」，不需要活的控件树。
 * 这样做的好处是双重的——归因函数可被**没有布局过的假数据**直接测到，也能接受来自序列化、
 * 逻辑快照或跨进程 REST 响应的几何，而不必在 render 域里持有 TreeNode。
 * 由 `collect_widget_boxes`（`widget/inspect.h`）负责把控件树拍平成这张表。
 */
struct WidgetBox {
    std::string path;  ///< 索引路径，与 `find_node_by_path` / `PUT /api/widget/{path}` 同格式（根为空串）
    std::string type;  ///< 控件类型名（`Widget::type_name()`）
    Rect bounds;  ///< 布局盒（逻辑单位 dp，与 `Node::bounds()` 同源）
};

/**
 * @brief 已归因的差异区域（specification/03-layout-render.md §8.4）。
 *
 * 回答「这块差异是**谁画的**」。归因结果里的 `path` 可直接回喂给 `Inspector::find_node`
 * 或 REST 的 `/api/widget/{path}`，AI 因此能从「有一块像素不对」走到「去改这个控件的属性」。
 */
struct RegionAttribution {
    DiffRegion region;  ///< 原始差异区域
    std::string widget_path;  ///< 命中的控件路径；无法归因时为空串
    std::string widget_type;  ///< 命中的控件类型；无法归因时为空串
    Rect widget_bounds;  ///< 命中控件的布局盒
    double widget_area_ratio = 0.0;  ///< 该控件被本区域覆盖的比例（0..1）
    bool partial_overlap = false;  ///< 区域有部分溢出到该控件之外（说明还牵连同层兄弟或背景）

    /// @brief 是否成功归因到控件。
    ///
    /// ⚠️ 判据是类型名而非路径：**根控件的路径本身就是空串**，用路径判空会把「归因到根」的
    /// 正常结果误判成「未归因」。
    [[nodiscard]] auto attributed() const -> bool { return !widget_type.empty(); }
};

/**
 * @brief 把差异区域归因到控件（specification/03-layout-render.md §8.4）。
 *
 * 规则：对每条区域，取**交叠面积最大**的控件；交叠相等时取 **DFS 序更靠后**的那个 ——
 * `boxes` 由 `collect_widget_boxes` 以先序遍历产出，父先于子，故「更靠后」等价于「更深」。
 * 这条 tie-break 是有意的：整块区域往往同时落在某个 Column 和它内部某个 Text 上，
 * 而真正画出问题像素的是更深的那一个。
 *
 * @param regions          待归因的区域（来自 `cluster_diff_regions`）
 * @param boxes            控件布局盒表；应来自 `collect_widget_boxes`
 * @param pixels_per_unit  每逻辑单位的像素数（渲染缩放）。`Node::bounds()` 是 dp、区域是像素，
 *                         二者换算靠它。默认 1.0（多数离屏渲染是 1:1）；<= 0 时按 1.0 处理。
 * @return 与 `regions` 等长、同序的结果列表；未命中任何控件的条目两个字符串字段为空。
 *
 * @note Thread: safe, no shared state
 * @note Side-effects: none
 */
[[nodiscard]] inline auto attribute_diff_regions(const std::vector<DiffRegion> &regions,
                                                 std::span<const WidgetBox> boxes, float pixels_per_unit = 1.0F)
    -> std::vector<RegionAttribution> {
    constexpr std::size_t AURORA_NONE = static_cast<std::size_t>(-1);
    const double scale = pixels_per_unit > 0.0F ? static_cast<double>(pixels_per_unit) : 1.0;

    std::vector<RegionAttribution> out;
    out.reserve(regions.size());
    for (const DiffRegion &r : regions) {
        RegionAttribution a;
        a.region = r;

        double best_overlap = 0.0;
        std::size_t best = AURORA_NONE;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const Rect device{.origin = Point{.x = static_cast<float>(boxes[i].bounds.origin.x * scale),
                                              .y = static_cast<float>(boxes[i].bounds.origin.y * scale)},
                              .size = Size{.width = static_cast<float>(boxes[i].bounds.size.width * scale),
                                           .height = static_cast<float>(boxes[i].bounds.size.height * scale)}};
            const double overlap = detail::rect_overlap_area(r.bounds, device);
            // >= 而非 >：交叠相等时让 DFS 序更靠后（更深）的控件胜出。
            if (overlap > 0.0 && overlap >= best_overlap) {
                best_overlap = overlap;
                best = i;
            }
        }

        if (best == AURORA_NONE) {
            out.push_back(a);
            continue;
        }

        a.widget_path = boxes[best].path;
        a.widget_type = boxes[best].type;
        a.widget_bounds = boxes[best].bounds;

        const double widget_area = static_cast<double>(boxes[best].bounds.size.width * scale) *
                                   static_cast<double>(boxes[best].bounds.size.height * scale);
        a.widget_area_ratio = widget_area > 0.0 ? best_overlap / widget_area : 0.0;

        const double region_area = static_cast<double>(r.bounds.size.width) * static_cast<double>(r.bounds.size.height);
        a.partial_overlap = (region_area - best_overlap) > 0.5;

        out.push_back(a);
    }
    return out;
}

/**
 * @brief 快照差异的完整报告（specification/03-layout-render.md §8.4）。
 *
 * 三层信息叠在一起，分别供不同消费方使用：
 *   - `raw`              逐像素统计（`compare_snapshots` 原样结果）—— 给机器判定通过与否；
 *   - `regions`          空间位置 —— 回答「差异在哪儿」；
 *   - `attributed`       控件归因 —— 回答「是谁画的」，其 `path` 可直接回喂给 `find_node` / REST。
 *
 * `to_text()` 产出人（或 LLM）可直接读的多行摘要，`to_json()` 产出同等信息的结构化信封。
 */
struct SnapshotDiffReport {
    SnapshotDiff raw;  ///< 逐像素统计（向后兼容，语义不变）
    std::vector<DiffRegion> regions;  ///< 未归因的差异区域
    std::vector<RegionAttribution> attributed;  ///< 已归因的差异区域，与 `regions` 等长同序
    double attributed_ratio = 0.0;  ///< 成功归因的差异像素占全部差异像素之比

    /// @brief 是否通过（沿用 `SnapshotDiff::passed` 的逐像素判据；语义不因本报告改变）。
    [[nodiscard]] auto passed() const -> bool { return raw.passed(); }

    /// @brief 结构化信封，供 MCP / Inspector 返回给调用方。
    [[nodiscard]] auto to_json() const -> Json;

    /// @brief 多行文本摘要。`max_regions` 限制逐条列出的条数（其余折叠为一行计数）。
    [[nodiscard]] auto to_text(std::size_t max_regions = 8) const -> std::string;
};

namespace detail {

/// @brief 矩形 → JSON 对象 {x,y,w,h}。矩形统一用这一种编码，避免各消费方自行约定。
[[nodiscard]] inline auto rect_to_json(const Rect &r) -> Json {
    Json j = Json::object();
    j["x"] = r.origin.x;
    j["y"] = r.origin.y;
    j["w"] = r.size.width;
    j["h"] = r.size.height;
    return j;
}

}  // namespace detail

inline auto SnapshotDiffReport::to_json() const -> Json {
    Json j = Json::object();
    j["size_mismatch"] = raw.size_mismatch;
    j["passed"] = passed();
    j["pixel_diff_count"] = raw.pixel_diff_count;
    j["max_color_delta"] = raw.max_color_delta;
    j["diff_ratio"] = raw.diff_ratio;
    j["attributed_ratio"] = attributed_ratio;

    Json region_arr = Json::array();
    for (const DiffRegion &r : regions) {
        Json o = detail::rect_to_json(r.bounds);
        o["diff_pixels"] = r.diff_pixels;
        o["coverage"] = r.coverage;
        o["max_color_delta"] = r.max_color_delta;
        region_arr.push_back(o);
    }
    j["regions"] = region_arr;

    Json attr_arr = Json::array();
    for (const RegionAttribution &a : attributed) {
        Json o = Json::object();
        o["region"] = detail::rect_to_json(a.region.bounds);
        o["diff_pixels"] = a.region.diff_pixels;
        // 未归因时用 null 而非空串；但注意**根控件的合法路径就是空串**，故这里不能反过来把
        // 空路径当作「未归因」——判据统一交给 `attributed()`（看类型名）。
        o["widget_path"] = a.attributed() ? Json(a.widget_path) : Json(nullptr);
        o["widget_type"] = a.attributed() ? Json(a.widget_type) : Json(nullptr);
        o["widget_box"] = detail::rect_to_json(a.widget_bounds);
        o["widget_area_ratio"] = a.widget_area_ratio;
        o["partial_overlap"] = a.partial_overlap;
        attr_arr.push_back(o);
    }
    j["attributed"] = attr_arr;
    return j;
}

inline auto SnapshotDiffReport::to_text(std::size_t max_regions) const -> std::string {
    std::ostringstream os;
    os << "snapshot diff: " << raw.pixel_diff_count << " px (" << std::fixed << std::setprecision(2)
       << raw.diff_ratio * 100.0 << "%), max delta " << raw.max_color_delta << ", " << regions.size() << " region(s)"
       << (raw.size_mismatch ? " [SIZE MISMATCH]" : "") << '\n';

    const std::size_t shown = max_regions < attributed.size() ? max_regions : attributed.size();
    for (std::size_t i = 0; i < shown; ++i) {
        const RegionAttribution &a = attributed[i];
        const double share = raw.pixel_diff_count > 0 ? static_cast<double>(a.region.diff_pixels) * 100.0 /
                                                            static_cast<double>(raw.pixel_diff_count)
                                                      : 0.0;
        os << "  [" << (i + 1) << "] " << std::fixed << std::setprecision(1) << share << "%  ";
        os << (a.attributed() ? (a.widget_path + " (" + a.widget_type + ")") : std::string("<unattributed>"));
        os << "  bounds=(" << std::setprecision(0) << a.region.bounds.origin.x << ',' << a.region.bounds.origin.y << ' '
           << a.region.bounds.size.width << 'x' << a.region.bounds.size.height << ')';
        if (a.attributed()) {
            os << "  widget_area=" << std::setprecision(2) << a.widget_area_ratio;
        }
        os << "  max_delta=" << a.region.max_color_delta;
        if (a.partial_overlap) {
            os << "  [partial]";
        }
        os << '\n';
    }
    if (shown < attributed.size()) {
        os << "  ... " << (attributed.size() - shown) << " more region(s) omitted\n";
    }
    return os.str();
}

/**
 * @brief 一步产出差异报告（specification/03-layout-render.md §8.4）。
 *
 * 等价于依次调用 `compare_snapshots` → `cluster_diff_regions` → `attribute_diff_regions`，
 * 是给调用方（golden 测试、MCP、Inspector）的唯一推荐入口 —— 少一步手写组合，
 * 就少一处「忘了做归因」或「两处容差口径不一致」的机会。
 *
 * @param boxes  可选：控件布局盒表（`collect_widget_boxes` 产出）。不传则仍产出 `regions`，
 *               但 `attributed` 全部为未归因、且 path/type 在 JSON 里为 null。
 *
 * @note Thread: safe, no shared state
 * @note Side-effects: none
 */
[[nodiscard]] inline auto build_snapshot_diff_report(const Image &baseline, const Image &current,
                                                     std::span<const WidgetBox> boxes = {}, int tolerance = 0,
                                                     const DiffRegionOptions &opt = {}, float pixels_per_unit = 1.0F)
    -> SnapshotDiffReport {
    SnapshotDiffReport report;
    report.raw = compare_snapshots(baseline, current, tolerance);
    if (report.raw.size_mismatch) {
        return report;  // 尺寸不合，其余字段无意义（与 compare_snapshots 的短路语义一致）
    }
    report.regions = cluster_diff_regions(baseline, current, tolerance, opt);
    report.attributed = attribute_diff_regions(report.regions, boxes, pixels_per_unit);

    std::size_t attributed_pixels = 0;
    for (const RegionAttribution &a : report.attributed) {
        if (a.attributed()) {
            attributed_pixels += a.region.diff_pixels;
        }
    }
    report.attributed_ratio = report.raw.pixel_diff_count > 0 ? static_cast<double>(attributed_pixels) /
                                                                    static_cast<double>(report.raw.pixel_diff_count)
                                                              : 0.0;
    return report;
}

}  // namespace aurora
