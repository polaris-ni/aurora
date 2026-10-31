#pragma once

/// @brief 文档输出与打印会话：把既有 `Painter` 绘制路径落到「页」上。
/// @file
///
/// 定位：现有落盘通道（`Application::render_to_png` / `Surface::capture_window`）都是
/// **屏幕几何的快照**——承袭画布宽高与 device pixel ratio，没有页尺寸、页边距、目标 DPI、
/// 多页与分页。本头补的正是「输出目标」那一层，不写新渲染器：每页仍由 `Painter` 在逻辑 dp
/// 上绘制，本层只负责页几何、缩放与落盘 / 送打印。
///
/// 栅格入 PDF 的三条局限（如实申报，见 `specification/06-app-platform.md`）：
/// - 文本不可选择、不可搜索（PDF 里只有图像 XObject，没有文本操作符）；
/// - 放大会失真（嵌入的是按目标 DPI 栅格化的位图）；
/// - 文件体积随页数线性膨胀（每页一张 DeviceRGB 图像）。
///
/// 五条硬约束（实现与验收同源）：
/// 1. **页边长用物理单位**（毫米），不用 dp：A4 的 210×297 mm 折到 96 dpi 不是整数 dp，
///    写成整数 dp 会先在纸张几何上损失再传到每页像素；
/// 2. **每页物理像素数以 `Painter` 的读回值为唯一真源**（`set_scale(dpi / 96)` → `begin()`
///    → `width()` / `height()`），本层不另算一套向上取整；
/// 3. **先合成到不透明底色再嵌入**：`Painter` 帧缓冲是 RGBA8，未绘制区是透明黑，而
///    `DeviceRGB` 图像 XObject 不带 alpha；
/// 4. **多页落在签名上**：页数与逐页绘制回调，而不是「一棵树自动分页」；
/// 5. **不复用 `render_to_image`**：它的逻辑像素恒等于设备像素（无 device pixel ratio 形参），
///    出不了 600 dpi 的页。
///
/// @note Thread: main-thread only（绘制回调与 `Painter` 同线程；送打印可阻塞，由调用方决定是否下沉）
/// @note Side-effects: `export_pdf` 写文件；`PrintSession` 占用打印后台处理程序会话与文件名
/// @note Rebuildable: yes（页面内容完全由回调决定）

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/render/painter.h"

namespace aurora {

/// @brief 页边距（毫米）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PageMargins {
    double top_mm = 12.7;  ///< 上边距（默认 0.5 英寸）
    double right_mm = 12.7;  ///< 右边距
    double bottom_mm = 12.7;  ///< 下边距
    double left_mm = 12.7;  ///< 左边距
};

/// @brief 纸张与栅格密度描述。
///
/// 尺寸与边距一律毫米：物理单位是纸张几何的唯一真源；`dpi` 只决定栅格密度，不改变纸张大小。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PageSpec {
    double width_mm = 210.0;  ///< 纸张宽（A4 默认）
    double height_mm = 297.0;  ///< 纸张高
    PageMargins margins{};  ///< 四边页边距
    int dpi = 300;  ///< 栅格密度（须 > 0；常见 96 / 300 / 600）
    Color background = Color::white();  ///< 页面底色（合成到不透明后再嵌入 PDF / 送打印）
};

/// @brief 内容区栅格尺寸（由 `Painter` 读回，非层内另算）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PageRaster {
    int width_px = 0;  ///< 内容区物理宽（`Painter::width()` 读回值）
    int height_px = 0;  ///< 内容区物理高（`Painter::height()` 读回值）
    int width_dp = 0;  ///< 交给 `Painter::begin` 的逻辑宽
    int height_dp = 0;  ///< 交给 `Painter::begin` 的逻辑高
    float scale = 1.0F;  ///< device pixel ratio（`dpi / 96`）
};

/// @brief 计算内容区栅格尺寸：`set_scale(dpi / 96)` → `begin(内容区 dp)` → 读回物理像素。
///
/// 物理像素数以 `Painter` 读回值为唯一真源——本层不另行 `ceil` / `round` 一套。
/// @param spec 纸张描述。
/// @return 成功携尺寸；页尺寸 / 边距 / DPI 非法时返回 `general-invalid-argument`。
/// @note Thread: callable on any thread（不触碰 OS / 全局状态）
/// @note Side-effects: 分配一块临时帧缓冲（只用于读回尺寸）
[[nodiscard]] auto page_content_raster(const PageSpec &spec) -> Result<PageRaster>;

/// @brief 逐页绘制回调：收到一枚已 `begin()` 的内容区 `Painter` 就画一页。
/// @param page_index 页序号（自 0 起，升序调用）
/// @param painter 已按页 DPI 缩放的 `Painter`（逻辑 dp；内容区左上角为原点，不含页边距）
using PageDrawer = std::function<void(std::size_t page_index, Painter &painter)>;

/// @brief 把逐页绘制结果导出为 PDF 1.4（栅格入 PDF）。
///
/// 每页流程：建 `Painter` → `set_scale(dpi / 96)` → `begin(内容区 dp)` → 填底色 → 回调绘制 →
/// RGBA 合成到 `spec.background` → 交最小 PDF 写出器（DeviceRGB 图像 XObject + FlateDecode）。
/// @param path 输出路径（覆盖写入）。
/// @param spec 纸张 / 边距 / DPI / 底色。
/// @param page_count 页数（须 ≥ 1）。
/// @param draw 逐页绘制回调（按 0..page_count-1 升序调用）。
/// @return 成功或结构化错误（参数非法 / 文件写入失败 / PDF 组装失败）。
/// @note Thread: 回调与 `Painter` 同线程；本函数可能写磁盘
/// @note Side-effects: 覆盖写 `path`
[[nodiscard]] auto export_pdf(const std::string &path, const PageSpec &spec, std::size_t page_count,
                              const PageDrawer &draw) -> Result<void>;

/// @brief 一个可用打印目标。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PrintTarget {
    std::string id;  ///< 平台标识（Windows 打印机名；macOS 打印机名）
    std::string name;  ///< 显示名
    bool is_default = false;  ///< 是否为系统默认打印机
};

/// @brief 枚举系统打印目标。
///
/// 契约：机器上没有任何打印机时返回**空列表配成功**（空集合不是错误）；平台无打印会话机制
/// 时返回 `print-session-unsupported`；机制在但枚举失败时返回 `print-session-failed`。
/// @return 打印目标列表或结构化错误。
/// @note Thread: 可能阻塞（打印后台处理程序枚举），避免在渲染热路径调用
/// @note Side-effects: 读系统打印服务状态
[[nodiscard]] auto print_targets() -> Result<std::vector<PrintTarget>>;

/// @brief 一次打印会话（打开目标 → 逐页提交 → 完成 / 中止）。
///
/// 不提供系统打印对话框：本库是 UI 库，打印目标选择与页面预览由应用自绘（避免为 Linux
/// 引入 GTK 依赖，也保证三平台观感一致）。`submit_page` 的位图若是 RGBA，本层先合成到
/// 页面底色再交后端——透明区域不得印成黑块。
///
/// @note Thread: 会话对象与全部方法 main-thread only；`submit_page` / `finish` 可能阻塞
/// @note Side-effects: 占用打印会话与打印机；`abort` / 析构未完成会话时丢弃已提交页
/// @note Rebuildable: no（会话不可复制、不可重放）
class PrintSession {
  public:
    /// @brief 打开打印会话（尚未提交任何页）。
    /// @param target_id `print_targets()` 给出的标识。
    /// @param spec 纸张 / 边距 / DPI / 底色（页位图按内容区栅格尺寸提交）。
    /// @return 成功携会话；平台不支持 / 目标不存在 / 后台处理程序拒绝时为结构化错误。
    [[nodiscard]] static auto open(std::string_view target_id, const PageSpec &spec) -> Result<PrintSession>;

    /// @brief 空会话（不可提交页；仅满足可移动容器的初始化需求）。
    PrintSession() noexcept;
    /// @brief 移动构造：接管会话所有权，源对象变为无效会话。
    /// @param other 源会话。
    PrintSession(PrintSession &&other) noexcept;
    /// @brief 移动赋值：先中止当前会话，再接管源会话（自赋值无操作）。
    /// @param other 源会话。
    /// @return 本对象引用。
    auto operator=(PrintSession &&other) noexcept -> PrintSession &;
    PrintSession(const PrintSession &) = delete;
    auto operator=(const PrintSession &) -> PrintSession & = delete;
    /// @brief 析构：未 `finish()` 的会话自动 `abort()`。
    ~PrintSession();

    /// @brief 提交一页（位图内容区；RGBA 先合成到页面底色）。
    /// @param page 页位图（尺寸须与 `page_content_raster(spec)` 一致）。
    /// @return 成功或结构化错误（会话已结束 / 尺寸不符 / 后台处理程序拒收）。
    [[nodiscard]] auto submit_page(const Image &page) -> Result<void>;

    /// @brief 结束文档并提交打印（幂等：已结束时为空操作）。
    /// @return 成功或结构化错误。
    [[nodiscard]] auto finish() -> Result<void>;

    /// @brief 中止会话（丢弃已提交页；幂等）。
    auto abort() -> void;

    /// @brief 会话是否仍可提交页。
    /// @return 已打开且未 finish / abort 时为 true。
    [[nodiscard]] auto is_open() const -> bool;

  private:
    struct Impl;  ///< 平台后端会话载荷（pimpl：公共头不引平台头）
    explicit PrintSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;  ///< 平台会话（空 = 无效会话）
    PageSpec spec_;  ///< 打开时的纸张描述
};

}  // namespace aurora
