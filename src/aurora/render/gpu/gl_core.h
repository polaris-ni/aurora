#pragma once

// ============================================================
// gl_core.h — GL 3.3 core 常量子集（仓库私有，不入公共头）
// ------------------------------------------------------------
// 与 gl_loader/gpu_gl_rhi 配套：全库经 GLFn 函数表调用 GL，本头仅提供
// 枚举常量值（值取自 GL 官方规范，与 <GL/gl.h> 无关、不包含任何系统头）。
// ============================================================

#include <cstdint>

#include "aurora/render/rhi/gpu_gl_rhi.h"

namespace aurora::rhi::gl {

// 基础类型常量
constexpr GLenum_ FALSE_ = 0;
constexpr GLenum_ TRUE_ = 1;
constexpr GLenum_ POINTS = 0x0000;
constexpr GLenum_ LINES = 0x0001;
constexpr GLenum_ TRIANGLES = 0x0004;
constexpr GLenum_ FLOAT = 0x1406;
constexpr GLenum_ UNSIGNED_BYTE = 0x1401;
constexpr GLenum_ UNSIGNED_INT = 0x1405;
constexpr GLenum_ INT = 0x1404;

// 缓冲与绘制
constexpr GLenum_ ARRAY_BUFFER = 0x8892;
constexpr GLenum_ ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum_ STREAM_DRAW = 0x88E0;
constexpr GLenum_ STATIC_DRAW = 0x88E4;

// 着色器
constexpr GLenum_ VERTEX_SHADER = 0x8B31;
constexpr GLenum_ FRAGMENT_SHADER = 0x8B30;
constexpr GLenum_ COMPILE_STATUS = 0x8B81;
constexpr GLenum_ LINK_STATUS = 0x8B82;
constexpr GLenum_ INFO_LOG_LENGTH = 0x8B84;

// 纹理
constexpr GLenum_ TEXTURE_2D = 0x0DE1;
constexpr GLenum_ TEXTURE0 = 0x84C0;
constexpr GLenum_ RED = 0x1903;
constexpr GLenum_ RGBA = 0x1908;
constexpr GLenum_ RGBA8 = 0x8058;
constexpr GLenum_ R8 = 0x8229;
constexpr GLenum_ NEAREST = 0x2600;
constexpr GLenum_ LINEAR = 0x2601;
constexpr GLenum_ TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum_ TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum_ TEXTURE_WRAP_S = 0x2802;
constexpr GLenum_ TEXTURE_WRAP_T = 0x2803;
constexpr GLenum_ CLAMP_TO_EDGE = 0x812F;
constexpr GLenum_ UNPACK_ALIGNMENT = 0x0CF5;

// 状态
constexpr GLenum_ BLEND = 0x0BE2;
constexpr GLenum_ SCISSOR_TEST = 0x0C11;
constexpr GLenum_ SRC_ALPHA = 0x0302;
constexpr GLenum_ ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum_ ONE = 1;
constexpr GLenum_ ZERO = 0;
constexpr GLenum_ COLOR_BUFFER_BIT = 0x00004000;

// 帧缓冲
constexpr GLenum_ FRAMEBUFFER = 0x8D40;
constexpr GLenum_ READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum_ DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum_ RENDERBUFFER = 0x8D41;
constexpr GLenum_ COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum_ FRAMEBUFFER_COMPLETE = 0x8CD5;

// 查询
constexpr GLenum_ VERSION = 0x1F02;
constexpr GLenum_ NO_ERROR = 0;

}  // namespace aurora::rhi::gl
