#pragma once

// ============================================================
// fake GL 驱动桩（tests/support/fake_gl.h）—— 全量填充 GLFn 函数表
// ------------------------------------------------------------
// 供 utest_gpu_gl_rhi（GpuGlRhi 契约断言）与 tools/bench/bench_gpu.cpp
//（GPU 特性基准的确定性计数器）共用：不依赖测试框架，仅依赖 rhi 公共头。
// 行为模拟「3.3 core 完整实现」并记录关键调用（draw/clear/blit/上传）供
// 断言与计数。GLFn 成员是裸函数指针，桩为静态函数 + `current` 实例转发
//（进程内串行执行，无并发）。可注入失败点：链接失败 / 版本不足。
// ============================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aurora/render/rhi/gpu_gl_rhi.h"

namespace aurora::testing {

namespace rhi = aurora::rhi;  // GLFn / GL 类型别名（GLenum_ 等）均居 rhi 命名空间

// GL 常量值（与 GL 官方规范一致；fake 桩只回填固定值，不引入 src/ 私有头）。
// 命名按仓库常量规范（CODING_STANDARDS.md §2：AURORA_ 前缀 UPPER_CASE）——k 前缀 CamelCase
// 禁用；外部 API 镜像豁免（gl_core.h）不适用于测试 TU。
constexpr std::uint32_t AURORA_GL_VERSION_QUERY = 0x1F02;
constexpr std::uint32_t AURORA_GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr std::uint32_t AURORA_GL_ARRAY_BUFFER_TARGET = 0x8892;
constexpr std::uint32_t AURORA_GL_FRAMEBUFFER_TARGET = 0x8D40;
constexpr std::uint32_t AURORA_GL_DRAW_FRAMEBUFFER_TARGET = 0x8CA9;
constexpr std::uint32_t AURORA_GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr std::uint32_t AURORA_GL_FILTER_NEAREST = 0x2600;
constexpr std::uint32_t AURORA_GL_FILTER_LINEAR = 0x2601;
constexpr std::uint32_t AURORA_GL_FORMAT_RGBA = 0x1908;

/// @brief fake GL 驱动桩：全量填充 GLFn 函数表，行为模拟「3.3 core 完整实现」并记录关键
/// 调用（draw/clear/blit）供断言。可注入失败点：链接失败 / 版本不足。
class FakeGl {
  public:
    explicit FakeGl(bool fail_link = false, std::string version = "3.3 (Core Profile) fake")
        : fail_link_(fail_link), version_(std::move(version)) {
        current = this;
        fill();
    }
    ~FakeGl() {
        current = nullptr;
    }
    FakeGl(const FakeGl &) = delete;
    auto operator=(const FakeGl &) -> FakeGl & = delete;

    rhi::GLFn fn{};

    // 观测计数
    int draw_calls = 0;  ///< draw_elements 次数（= 批提交次数）
    int clears = 0;
    int blits = 0;
    int texture_gens = 0;    ///< gen_textures 发放纹理对象总数（纹理分配次数口径）
    int texture_deletes = 0; ///< delete_textures 删除纹理对象总数（淘汰抖动口径）

    /// @brief gen_framebuffers 发放顺序（ensure_framebuffer 固定发放 msaa → resolve → temp）。
    std::vector<rhi::GLuint_> gen_fbo_ids;
    /// @brief 每次 draw_elements 时的 DRAW 帧缓冲绑定（效果 pass 的 ping-pong 序列验证）。
    std::vector<rhi::GLuint_> draw_fbo_targets;
    /// @brief 最近一次 ARRAY_BUFFER buffer_data 内容（批顶点 / 效果 quad 的字节级验证）。
    std::vector<std::uint8_t> last_vbo_data;
    /// @brief tex_parameter_i(MIN_FILTER) 设置序列（DrawImage LINEAR / Composite NEAREST）。
    std::vector<rhi::GLint_> tex_min_filters;
    rhi::GLuint_ cur_draw_fbo = 0;  ///< 当前 DRAW 帧缓冲（FRAMEBUFFER / DRAW_FRAMEBUFFER 均更新）

    /// @brief tex_image_2d 带像素数据的上传记录（渐变 LUT 内容验证用；空数据上传不入列）。
    struct Upload {
        rhi::GLsizei_ width = 0;
        rhi::GLsizei_ height = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<Upload> uploads;

    /// @brief tex_sub_image_2d 子区域上传记录（字形图集槽位验证用；R8 单通道，w*h 字节）。
    struct SubUpload {
        rhi::GLint_ x = 0;
        rhi::GLint_ y = 0;
        rhi::GLsizei_ width = 0;
        rhi::GLsizei_ height = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<SubUpload> sub_uploads;
    /// @brief RGBA 直色子上传记录（常驻流式纹理通道验证用；4 字节/像素）。
    std::vector<SubUpload> rgba_sub_uploads;

  private:
    bool fail_link_;
    std::string version_;
    std::uint32_t next_id_ = 1;
    static inline FakeGl *current = nullptr;

    static void gen_ids(rhi::GLsizei_ n, rhi::GLuint_ *out) {
        for (rhi::GLsizei_ i = 0; i < n; ++i) {
            out[i] = current->next_id_++;
        }
    }
    static void gen_tex_ids(rhi::GLsizei_ n, rhi::GLuint_ *out) {
        gen_ids(n, out);
        current->texture_gens += n;
    }
    static void delete_objects(rhi::GLsizei_, const rhi::GLuint_ *) {}
    static void delete_tex_objects(rhi::GLsizei_ n, const rhi::GLuint_ *) {
        current->texture_deletes += n;
    }

    // ---- 着色器与程序 ----
    static auto create_shader(rhi::GLenum_) -> rhi::GLuint_ {
        return current->next_id_++;
    }
    static void shader_source(rhi::GLuint_, rhi::GLsizei_, const rhi::GLchar_ *const *, const rhi::GLint_ *) {}
    static void compile_shader(rhi::GLuint_) {}
    static void get_shader_iv(rhi::GLuint_, rhi::GLenum_, rhi::GLint_ *params) {
        *params = 1;
    }
    static void get_shader_info_log(rhi::GLuint_, rhi::GLsizei_, rhi::GLsizei_ *length, rhi::GLchar_ *) {
        *length = 0;
    }
    static void delete_shader(rhi::GLuint_) {}
    static auto create_program() -> rhi::GLuint_ {
        return current->next_id_++;
    }
    static void attach_shader(rhi::GLuint_, rhi::GLuint_) {}
    static void link_program(rhi::GLuint_) {}
    static void get_program_iv(rhi::GLuint_, rhi::GLenum_, rhi::GLint_ *params) {
        *params = current->fail_link_ ? 0 : 1;
    }
    static void get_program_info_log(rhi::GLuint_, rhi::GLsizei_, rhi::GLsizei_ *length, rhi::GLchar_ *) {
        *length = 0;
    }
    static void delete_program(rhi::GLuint_) {}
    static void use_program(rhi::GLuint_) {}
    static auto get_uniform_location(rhi::GLuint_, const rhi::GLchar_ *) -> rhi::GLint_ {
        return static_cast<rhi::GLint_>(current->next_id_++);
    }
    static void uniform1i(rhi::GLint_, rhi::GLint_) {}
    static void uniform1f(rhi::GLint_, rhi::GLfloat_) {}
    static void uniform2f(rhi::GLint_, rhi::GLfloat_, rhi::GLfloat_) {}
    static void uniform3f(rhi::GLint_, rhi::GLfloat_, rhi::GLfloat_, rhi::GLfloat_) {}
    static void uniform4f(rhi::GLint_, rhi::GLfloat_, rhi::GLfloat_, rhi::GLfloat_, rhi::GLfloat_) {}

    // ---- 顶点数组与缓冲 ----
    static void bind_vertex_array(rhi::GLuint_) {}
    static void bind_buffer(rhi::GLenum_, rhi::GLuint_) {}
    static void buffer_data(rhi::GLenum_ target, rhi::GLsizeiptr_ size, const void *data, rhi::GLenum_) {
        if (target == AURORA_GL_ARRAY_BUFFER_TARGET && data != nullptr) {
            const auto *p = static_cast<const std::uint8_t *>(data);
            current->last_vbo_data.assign(p, p + static_cast<std::size_t>(size));
        }
    }
    static void enable_vertex_attrib_array(rhi::GLuint_) {}
    static void vertex_attrib_pointer(rhi::GLuint_, rhi::GLint_, rhi::GLenum_, rhi::GLboolean_, rhi::GLsizei_,
                                      const void *) {}

    // ---- 纹理 ----
    static void bind_texture(rhi::GLenum_, rhi::GLuint_) {}
    static void active_texture(rhi::GLenum_) {}
    static void tex_image_2d(rhi::GLenum_, rhi::GLint_, rhi::GLint_, rhi::GLsizei_ width, rhi::GLsizei_ height,
                             rhi::GLint_, rhi::GLenum_, rhi::GLenum_, const void *pixels) {
        if (pixels != nullptr) {
            Upload up;
            up.width = width;
            up.height = height;
            const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
            const auto *p = static_cast<const std::uint8_t *>(pixels);
            up.data.assign(p, p + bytes);
            current->uploads.push_back(std::move(up));
        }
    }
    static void tex_parameter_i(rhi::GLenum_, rhi::GLenum_ pname, rhi::GLint_ param) {
        if (pname == AURORA_GL_TEXTURE_MIN_FILTER) {
            current->tex_min_filters.push_back(param);
        }
    }
    static void tex_sub_image_2d(rhi::GLenum_, rhi::GLint_, rhi::GLint_ xoffset, rhi::GLint_ yoffset,
                                 rhi::GLsizei_ width, rhi::GLsizei_ height, rhi::GLenum_ format, rhi::GLenum_,
                                 const void *pixels) {
        SubUpload up;
        up.x = xoffset;
        up.y = yoffset;
        up.width = width;
        up.height = height;
        if (pixels != nullptr) {
            const auto *p = static_cast<const std::uint8_t *>(pixels);
            const std::size_t bpp = format == AURORA_GL_FORMAT_RGBA ? 4U : 1U;
            up.data.assign(p, p + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bpp);
        }
        if (format == AURORA_GL_FORMAT_RGBA) {
            current->rgba_sub_uploads.push_back(std::move(up));
        } else {
            current->sub_uploads.push_back(std::move(up));
        }
    }
    static void pixel_store_i(rhi::GLenum_, rhi::GLint_) {}

    // ---- 状态与绘制 ----
    static void viewport(rhi::GLint_, rhi::GLint_, rhi::GLsizei_, rhi::GLsizei_) {}
    static void clear_color(rhi::GLfloat_, rhi::GLfloat_, rhi::GLfloat_, rhi::GLfloat_) {}
    static void clear(rhi::GLbitfield_) {
        current->clears++;
    }
    static void enable(rhi::GLenum_) {}
    static void disable(rhi::GLenum_) {}
    static void scissor(rhi::GLint_, rhi::GLint_, rhi::GLsizei_, rhi::GLsizei_) {}
    static void blend_func_separate(rhi::GLenum_, rhi::GLenum_, rhi::GLenum_, rhi::GLenum_) {}
    static void draw_elements(rhi::GLenum_, rhi::GLsizei_, rhi::GLenum_, const void *) {
        current->draw_calls++;
        current->draw_fbo_targets.push_back(current->cur_draw_fbo);
    }
    static void draw_arrays(rhi::GLenum_, rhi::GLint_, rhi::GLsizei_) {}
    static void flush() {}

    // ---- 帧缓冲 ----
    static void gen_fbo(rhi::GLsizei_ n, rhi::GLuint_ *out) {
        gen_ids(n, out);
        for (rhi::GLsizei_ i = 0; i < n; ++i) {
            current->gen_fbo_ids.push_back(out[i]);
        }
    }
    static void bind_framebuffer(rhi::GLenum_ target, rhi::GLuint_ fbo) {
        if (target == AURORA_GL_FRAMEBUFFER_TARGET || target == AURORA_GL_DRAW_FRAMEBUFFER_TARGET) {
            current->cur_draw_fbo = fbo;
        }
    }
    static void framebuffer_texture_2d(rhi::GLenum_, rhi::GLenum_, rhi::GLenum_, rhi::GLuint_, rhi::GLint_) {}
    static void framebuffer_renderbuffer(rhi::GLenum_, rhi::GLenum_, rhi::GLenum_, rhi::GLuint_) {}
    static auto check_framebuffer_status(rhi::GLenum_) -> rhi::GLenum_ {
        return AURORA_GL_FRAMEBUFFER_COMPLETE;
    }
    static void bind_renderbuffer(rhi::GLenum_, rhi::GLuint_) {}
    static void renderbuffer_storage_multisample(rhi::GLenum_, rhi::GLsizei_, rhi::GLenum_, rhi::GLsizei_,
                                                 rhi::GLsizei_) {}
    static void blit_framebuffer(rhi::GLint_, rhi::GLint_, rhi::GLint_, rhi::GLint_, rhi::GLint_, rhi::GLint_,
                                 rhi::GLint_, rhi::GLint_, rhi::GLbitfield_, rhi::GLenum_) {
        current->blits++;
    }
    static void read_pixels(rhi::GLint_, rhi::GLint_, rhi::GLsizei_ width, rhi::GLsizei_ height, rhi::GLenum_,
                            rhi::GLenum_, void *pixels) {
        std::memset(pixels, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    }

    // ---- 查询 ----
    static auto get_string(rhi::GLenum_ name) -> const rhi::GLubyte_ * {
        if (name == AURORA_GL_VERSION_QUERY) {
            return reinterpret_cast<const rhi::GLubyte_ *>(current->version_.c_str());  // NOLINT(*-pro-type-reinterpret-cast)
        }
        return reinterpret_cast<const rhi::GLubyte_ *>("");  // NOLINT(*-pro-type-reinterpret-cast)
    }
    static void get_integer_v(rhi::GLenum_, rhi::GLint_ *) {}
    static auto get_error() -> rhi::GLenum_ {
        return 0;  // NO_ERROR
    }

    auto fill() -> void {
        fn.create_shader = &create_shader;
        fn.shader_source = &shader_source;
        fn.compile_shader = &compile_shader;
        fn.get_shader_iv = &get_shader_iv;
        fn.get_shader_info_log = &get_shader_info_log;
        fn.delete_shader = &delete_shader;
        fn.create_program = &create_program;
        fn.attach_shader = &attach_shader;
        fn.link_program = &link_program;
        fn.get_program_iv = &get_program_iv;
        fn.get_program_info_log = &get_program_info_log;
        fn.delete_program = &delete_program;
        fn.use_program = &use_program;
        fn.get_uniform_location = &get_uniform_location;
        fn.uniform1i = &uniform1i;
        fn.uniform1f = &uniform1f;
        fn.uniform2f = &uniform2f;
        fn.uniform3f = &uniform3f;
        fn.uniform4f = &uniform4f;
        fn.gen_vertex_arrays = &gen_ids;
        fn.delete_vertex_arrays = &delete_objects;
        fn.bind_vertex_array = &bind_vertex_array;
        fn.gen_buffers = &gen_ids;
        fn.delete_buffers = &delete_objects;
        fn.bind_buffer = &bind_buffer;
        fn.buffer_data = &buffer_data;
        fn.enable_vertex_attrib_array = &enable_vertex_attrib_array;
        fn.vertex_attrib_pointer = &vertex_attrib_pointer;
        fn.gen_textures = &gen_tex_ids;
        fn.delete_textures = &delete_tex_objects;
        fn.bind_texture = &bind_texture;
        fn.active_texture = &active_texture;
        fn.tex_image_2d = &tex_image_2d;
        fn.tex_sub_image_2d = &tex_sub_image_2d;
        fn.tex_parameter_i = &tex_parameter_i;
        fn.pixel_store_i = &pixel_store_i;
        fn.viewport = &viewport;
        fn.clear_color = &clear_color;
        fn.clear = &clear;
        fn.enable = &enable;
        fn.disable = &disable;
        fn.scissor = &scissor;
        fn.blend_func_separate = &blend_func_separate;
        fn.draw_elements = &draw_elements;
        fn.draw_arrays = &draw_arrays;
        fn.flush = &flush;
        fn.gen_framebuffers = &gen_fbo;
        fn.delete_framebuffers = &delete_objects;
        fn.bind_framebuffer = &bind_framebuffer;
        fn.framebuffer_texture_2d = &framebuffer_texture_2d;
        fn.framebuffer_renderbuffer = &framebuffer_renderbuffer;
        fn.check_framebuffer_status = &check_framebuffer_status;
        fn.gen_renderbuffers = &gen_ids;
        fn.delete_renderbuffers = &delete_objects;
        fn.bind_renderbuffer = &bind_renderbuffer;
        fn.renderbuffer_storage_multisample = &renderbuffer_storage_multisample;
        fn.blit_framebuffer = &blit_framebuffer;
        fn.read_pixels = &read_pixels;
        fn.get_string = &get_string;
        fn.get_integer_v = &get_integer_v;
        fn.get_error = &get_error;
    }
};

}  // namespace aurora::testing
