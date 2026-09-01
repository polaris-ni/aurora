#pragma once

#include <memory>

#include "aurora/image/image_codec.h"

namespace aurora::image {

// 各编解码器工厂（实现位于对应 .cpp，注册表据此惰性装配）。
// 头文件不含任何三方库依赖，避免污染注册表 TU。
auto create_stb_codec() -> std::shared_ptr<ImageCodec>;       // 通用兜底解码（png/jpg/gif/bmp/tga/hdr）
auto create_bmp_codec() -> std::shared_ptr<ImageCodec>;       // 内置未压缩 24 位 BMP
auto create_svg_codec() -> std::shared_ptr<ImageCodec>;       // 内置 SVG 子集光栅化
auto create_png_write_codec() -> std::shared_ptr<ImageCodec>; // 内置 PNG 编码（write_png）

#ifdef AURORA_BUILD_IMAGE_JPEG
auto create_jpeg_turbo_codec() -> std::shared_ptr<ImageCodec>; // libjpeg-turbo 解码+编码
#endif
#ifdef AURORA_BUILD_IMAGE_WEBP
auto create_webp_codec() -> std::shared_ptr<ImageCodec>; // libwebp 解码+编码+动图
#endif
#ifdef AURORA_BUILD_IMAGE_PNG
auto create_png_wuffs_codec() -> std::shared_ptr<ImageCodec>; // wuffs PNG 解码
auto create_gif_wuffs_codec() -> std::shared_ptr<ImageCodec>; // wuffs GIF 动图解码
#endif

} // namespace aurora::image
