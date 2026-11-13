#pragma once

// 最小 PDF 写出器：**内部头**（src/aurora/render/detail/，与 aria_protocol.h 等内部实现同列）。
//
// 只做一件事：把一组已合成好的 RGB8 页面按 PDF 1.4 的「图像 XObject + FlateDecode」形态落盘。
// **不**写文本、不写矢量图元——`Painter` 产出的是栅格位图，落到 PDF 就是逐页嵌入图像；由此
// 文本不可选择 / 不可搜索、放大会失真、体积随页数线性膨胀，三条局限在
// `codespec/specification/06-app-platform.md` 的文档输出一节里如实申报。
//
// 压缩口径：PDF 的 `FlateDecode` 流按 ISO 32000 是 **zlib（RFC 1950）**包装，而非裸 DEFLATE
// （RFC 1951）。本写出器因此产出 zlib 流；容器格式是「stored block」（BTYPE=00），
// 即不追求压缩率、只保证可被任一 PDF 阅读器解码（真源是帧缓冲，压缩率不是本写出器的目标）。
// 单测可用 `aurora::decompress(AURORA_COMPRESS_FORMAT_ZLIB, ...)` 逐字节回读校验。
//
// 门控：无。本头在所有平台编译；无 OS 依赖。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::render::detail {

/// @brief 一页待嵌入的 RGB 位图及其在 PDF 页上的落位。
///
/// 像素行序为**自上而下**（与 framebuffer 一致）；矩阵在内容流里做一次垂直翻转，使 PDF 的
/// 左下原点坐标系里看到的是正向页面。
struct PdfImagePage {
    int pixel_width = 0;  ///< 位图宽（像素）
    int pixel_height = 0;  ///< 位图高（像素）
    std::vector<std::uint8_t> rgb;  ///< RGB8 直色像素（长度 = pixel_width * pixel_height * 3）
    double place_x_pt = 0.0;  ///< 落位左下角 x（PDF 点）
    double place_y_pt = 0.0;  ///< 落位左下角 y（PDF 点）
    double place_width_pt = 0.0;  ///< 落位宽（PDF 点）
    double place_height_pt = 0.0;  ///< 落位高（PDF 点）
};

/// @brief 把字节流编码为 zlib 包装的 stored DEFLATE 块（RFC 1950 + RFC 1951）。
/// @param input 原始字节（任意长度，含空）。
/// @return zlib 流（2 字节头 + stored blocks + 4 字节 Adler-32）。
[[nodiscard]] auto deflate_zlib_store(std::span<const std::uint8_t> input) -> std::vector<std::uint8_t>;

/// @brief 组装完整 PDF 字节流（确定性输出：同输入逐字节一致）。
/// @param page_width_pt 页宽（PDF 点，72 dpi）。
/// @param page_height_pt 页高（PDF 点）。
/// @param pages 逐页位图与落位（至少一页；空页表为非法参数）。
/// @return 成功携 PDF 字节；尺寸非法 / 像素长度不匹配 / 页表为空时为结构化错误。
[[nodiscard]] auto build_pdf(double page_width_pt, double page_height_pt, const std::vector<PdfImagePage> &pages)
    -> Result<std::vector<std::uint8_t>>;

/// @brief 组装并写出一份 PDF 文件。
/// @param path 输出路径（覆盖写入）。
/// @param page_width_pt 页宽（PDF 点）。
/// @param page_height_pt 页高（PDF 点）。
/// @param pages 逐页位图与落位。
/// @return 成功或结构化错误（组装失败 / 文件打不开 / 写入失败）。
[[nodiscard]] auto write_pdf(const std::string &path, double page_width_pt, double page_height_pt,
                             const std::vector<PdfImagePage> &pages) -> Result<void>;

}  // namespace aurora::render::detail
