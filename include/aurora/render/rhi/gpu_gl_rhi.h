#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/rhi_frame_sink.h"

namespace aurora::rhi {

// ---- GL 类型别名（仅本头内 GLFn 签名使用；本库公共头不含任何 GL 原生头） ----
// 供 `GpuGlRhi` 经函数表调用 GL 3.3 core 而无需 <GL/gl.h> / GLAD；值与原生 GL 类型逐一同宽。
// ⚠️ 若消费者同时包含真实 GL 头并 `using namespace aurora::rhi`，别名可能与原生 typedef 冲突
//（同名同宽，实际无害；但属已知边界，勿在本头之外扩散这些别名）。
using GLenum_ = std::uint32_t;
using GLboolean_ = std::uint8_t;
using GLbitfield_ = std::uint32_t;
using GLint_ = std::int32_t;
using GLsizei_ = std::int32_t;
using GLuint_ = std::uint32_t;
using GLsizeiptr_ = std::ptrdiff_t;
using GLfloat_ = float;
using GLchar_ = char;
using GLubyte_ = std::uint8_t;

/// @brief GL 3.3 core 函数表：经加载器逐个 `GetProcAddress` 装载，签名与 GL 官方原型同宽。
///
/// 不依赖系统 `<GL/gl.h>`（Windows 仅声明 1.1）、不引入 GLAD/gl3w 三方头。函数指针缺项
/// （驱动过老 / 截断实现）由 `load_gl` 置空，`GpuGlRhi` 构造时检测并整体判败（软件回退）。
struct GLFn {
    // ---- 着色器与程序 ----
    GLuint_ (*create_shader)(GLenum_ type) = nullptr;
    void (*shader_source)(GLuint_ shader, GLsizei_ count, const GLchar_ *const *str, const GLint_ *length) = nullptr;
    void (*compile_shader)(GLuint_ shader) = nullptr;
    void (*get_shader_iv)(GLuint_ shader, GLenum_ pname, GLint_ *params) = nullptr;
    void (*get_shader_info_log)(GLuint_ shader, GLsizei_ buf_size, GLsizei_ *length, GLchar_ *info_log) = nullptr;
    void (*delete_shader)(GLuint_ shader) = nullptr;
    GLuint_ (*create_program)() = nullptr;
    void (*attach_shader)(GLuint_ program, GLuint_ shader) = nullptr;
    void (*link_program)(GLuint_ program) = nullptr;
    void (*get_program_iv)(GLuint_ program, GLenum_ pname, GLint_ *params) = nullptr;
    void (*get_program_info_log)(GLuint_ program, GLsizei_ buf_size, GLsizei_ *length, GLchar_ *info_log) = nullptr;
    void (*delete_program)(GLuint_ program) = nullptr;
    void (*use_program)(GLuint_ program) = nullptr;
    GLint_ (*get_uniform_location)(GLuint_ program, const GLchar_ *name) = nullptr;
    void (*uniform1i)(GLint_ location, GLint_ v0) = nullptr;
    void (*uniform1f)(GLint_ location, GLfloat_ v0) = nullptr;
    void (*uniform2f)(GLint_ location, GLfloat_ v0, GLfloat_ v1) = nullptr;
    void (*uniform3f)(GLint_ location, GLfloat_ v0, GLfloat_ v1, GLfloat_ v2) = nullptr;
    void (*uniform4f)(GLint_ location, GLfloat_ v0, GLfloat_ v1, GLfloat_ v2, GLfloat_ v3) = nullptr;

    // ---- 顶点数组与缓冲 ----
    void (*gen_vertex_arrays)(GLsizei_ n, GLuint_ *arrays) = nullptr;
    void (*delete_vertex_arrays)(GLsizei_ n, const GLuint_ *arrays) = nullptr;
    void (*bind_vertex_array)(GLuint_ array) = nullptr;
    void (*gen_buffers)(GLsizei_ n, GLuint_ *buffers) = nullptr;
    void (*delete_buffers)(GLsizei_ n, const GLuint_ *buffers) = nullptr;
    void (*bind_buffer)(GLenum_ target, GLuint_ buffer) = nullptr;
    void (*buffer_data)(GLenum_ target, GLsizeiptr_ size, const void *data, GLenum_ usage) = nullptr;
    void (*enable_vertex_attrib_array)(GLuint_ index) = nullptr;
    void (*vertex_attrib_pointer)(GLuint_ index, GLint_ size, GLenum_ type, GLboolean_ normalized, GLsizei_ stride,
                                  const void *pointer) = nullptr;

    // ---- 纹理 ----
    void (*gen_textures)(GLsizei_ n, GLuint_ *textures) = nullptr;
    void (*delete_textures)(GLsizei_ n, const GLuint_ *textures) = nullptr;
    void (*bind_texture)(GLenum_ target, GLuint_ texture) = nullptr;
    void (*active_texture)(GLenum_ unit) = nullptr;
    void (*tex_image_2d)(GLenum_ target, GLint_ level, GLint_ internalformat, GLsizei_ width, GLsizei_ height,
                         GLint_ border, GLenum_ format, GLenum_ type, const void *pixels) = nullptr;
    void (*tex_sub_image_2d)(GLenum_ target, GLint_ level, GLint_ xoffset, GLint_ yoffset, GLsizei_ width,
                             GLsizei_ height, GLenum_ format, GLenum_ type, const void *pixels) = nullptr;
    void (*tex_parameter_i)(GLenum_ target, GLenum_ pname, GLint_ param) = nullptr;
    void (*pixel_store_i)(GLenum_ pname, GLint_ param) = nullptr;

    // ---- 状态与绘制 ----
    void (*viewport)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height) = nullptr;
    void (*clear_color)(GLfloat_ red, GLfloat_ green, GLfloat_ blue, GLfloat_ alpha) = nullptr;
    void (*clear)(GLbitfield_ mask) = nullptr;
    void (*enable)(GLenum_ cap) = nullptr;
    void (*disable)(GLenum_ cap) = nullptr;
    void (*scissor)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height) = nullptr;
    void (*blend_func_separate)(GLenum_ sfactor_rgb, GLenum_ dfactor_rgb, GLenum_ sfactor_alpha,
                                GLenum_ dfactor_alpha) = nullptr;
    void (*draw_elements)(GLenum_ mode, GLsizei_ count, GLenum_ type, const void *indices) = nullptr;
    void (*draw_arrays)(GLenum_ mode, GLint_ first, GLsizei_ count) = nullptr;
    void (*flush)() = nullptr;

    // ---- 帧缓冲（MSAA 渲染 + resolve + 呈现） ----
    void (*gen_framebuffers)(GLsizei_ n, GLuint_ *framebuffers) = nullptr;
    void (*delete_framebuffers)(GLsizei_ n, const GLuint_ *framebuffers) = nullptr;
    void (*bind_framebuffer)(GLenum_ target, GLuint_ framebuffer) = nullptr;
    void (*framebuffer_texture_2d)(GLenum_ target, GLenum_ attachment, GLenum_ textarget, GLuint_ texture,
                                   GLint_ level) = nullptr;
    void (*framebuffer_renderbuffer)(GLenum_ target, GLenum_ attachment, GLenum_ renderbuffertarget,
                                     GLuint_ renderbuffer) = nullptr;
    GLenum_ (*check_framebuffer_status)(GLenum_ target) = nullptr;
    void (*gen_renderbuffers)(GLsizei_ n, GLuint_ *renderbuffers) = nullptr;
    void (*delete_renderbuffers)(GLsizei_ n, const GLuint_ *renderbuffers) = nullptr;
    void (*bind_renderbuffer)(GLenum_ target, GLuint_ renderbuffer) = nullptr;
    void (*renderbuffer_storage_multisample)(GLenum_ target, GLsizei_ samples, GLenum_ internalformat, GLsizei_ width,
                                             GLsizei_ height) = nullptr;
    void (*blit_framebuffer)(GLint_ src_x0, GLint_ src_y0, GLint_ src_x1, GLint_ src_y1, GLint_ dst_x0, GLint_ dst_y0,
                             GLint_ dst_x1, GLint_ dst_y1, GLbitfield_ mask, GLenum_ filter) = nullptr;
    void (*read_pixels)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height, GLenum_ format, GLenum_ type,
                        void *pixels) = nullptr;

    // ---- 查询 ----
    const GLubyte_ *(*get_string)(GLenum_ name) = nullptr;
    void (*get_integer_v)(GLenum_ pname, GLint_ *params) = nullptr;
    GLenum_ (*get_error)() = nullptr;

    /// @brief 函数表是否完整（无空指针）。`load_gl` 后判定；测试桩全量填充即完整。
    [[nodiscard]] auto complete() const -> bool;
};

/// @brief 经加载回调逐名装载 GL 3.3 core 函数表（如 GLFW 的 `glfwGetProcAddress`）。
/// @param proc 名字 → 函数地址回调（原始 `void*` 返回，调用方负责 `reinterpret_cast` 适配
///             平台类型，与 GLAD/gladLoadGLLoader 同口径）。
auto load_gl(void *(*proc)(const char *name)) -> GLFn;

/// @brief GPU 栅格后端：`DisplayList` 的 OpenGL 3.3 core 消费者（`RhiBackend` + `RhiFrameSink`）。
///
/// 命令消费（`submit`）只做「命令 → 顶点/状态」翻译，**不触 GL**；GL 调用集中在
/// `flush`（批提交）与 `end_frame`（上屏 blit）。批切分保序不重排：管线/裁剪态/
/// 混合态变化即断批。全局 alpha（`SetAlpha`）烘焙进顶点色，不断批。
///
/// 命令覆盖（全部为 GPU 实路径，无跳过降级）：几何组（FillRect/ClearRect/DrawRect/
/// DrawLine/RoundedBorder）、状态组（PushClip/PushClipRounded/PopClip/SetAlpha）、渐变组
///（LinearGradient/RadialGradient，经 256×1 LUT 纹理采样，语义与软件 `sample_gradient`
/// 对齐）、图像组（DrawImage，PMA 纹理 + `Image::content_hash()` 摘要缓存；Composite，
/// 仿射矩阵直烘四角顶点 + NEAREST 逐像素取样同软件）、文本（DrawText，多页 R8 字形图集
///——满页开新页、页数封顶 LRU 淘汰，光栅化复用软件 `GlyphAtlas` 经字形发射桥同源发射）
/// 与效果组（Shadow 单批 SDF 距离衰减；BlurRegion 两遍分离 box blur 经 resolve/temp FBO
/// ping-pong；BlendRegion/MaskRegion 单 pass 采样回写——三者区域物理像素换算与软件三原语同形）。
/// 裁剪语义与软件路径对齐：矩形/圆角裁剪统一走 shader 内 SDF alpha（不 discard，
/// 不用 scissor），栈顶即各层矩形交集（与 `Painter::push_clip` 交叠语义一致）。
///
/// 初始化失败（版本不足 / 着色器链接失败 / GL 错误）→ `valid()` 为 false，调用方
/// 整体回退软件路径，不做逐命令混合。
///
/// 本类与 `load_gl` / `GLFn` **恒编译进库**（不裁切于 `AURORA_ENABLE_GLFW_GPU_GL`）：
/// feature 宏只控制 `GlfwSurface` 是否接线 GPU 模式；未开启时本类同样可用（构造即
/// invalid），供测试桩与消费者显式装配。
class GpuGlRhi final : public RhiBackend, public RhiFrameSink {
  public:
    /// @brief 帧级诊断计数（性能观测 / 测试断言）。
    struct FrameStats {
        std::uint32_t draw_calls = 0;  ///< flush 次数（= 批数）
        std::uint32_t vertices = 0;    ///< 本帧提交顶点数
        std::uint32_t skipped_cmds = 0;  ///< 遇到未实现命令而跳过的条数
    };

    /// @brief 默认构造：不装载 GL（`valid()` 为 false），供占位与测试桩场景。
    GpuGlRhi();
    /// @brief 装载构造：以上下文就绪的函数表初始化着色器/缓冲/MSAA 帧缓冲。
    /// 失败（含函数表缺项）时 `valid()` 为 false，不抛异常。
    explicit GpuGlRhi(GLFn fn);
    ~GpuGlRhi() override;
    GpuGlRhi(const GpuGlRhi &) = delete;
    auto operator=(const GpuGlRhi &) -> GpuGlRhi & = delete;
    GpuGlRhi(GpuGlRhi &&) = delete;
    auto operator=(GpuGlRhi &&) -> GpuGlRhi & = delete;

    /// @brief GL 资源是否就绪（构造成功且未发生上下文级失败）。
    [[nodiscard]] auto valid() const -> bool;

    [[nodiscard]] auto name() const -> std::string_view override { return "gpu-gl"; }

    /// @brief 命令消费面视图（`DisplayList::replay` 入口）；帧调度与命令消费同对象。
    [[nodiscard]] auto backend() -> RhiBackend & override { return *this; }

    /// @brief 消费一条命令（翻译进当前批；GL 调用延迟到 flush）。
    auto submit(const DrawCmd &cmd, const CmdData &data) -> void override;

    // ---- RhiFrameSink ----
    [[nodiscard]] auto begin_frame(int device_width, int device_height, float scale) -> bool override;
    auto end_frame() -> void override;

    /// @brief 本帧（自 `begin_frame` 起）累计诊断计数。
    [[nodiscard]] auto stats() const -> FrameStats;

    /// @brief 字形图集页边长（默认 1024；须在 `begin_frame` 前设置，非正值忽略）。
    /// 常规消费者无须调用；测试用小页覆盖「满页开新页 / 页数封顶 LRU 淘汰」路径。
    auto set_glyph_page_size(int side) -> void;

    /// @brief 当前帧内容读回（RGBA8，行序自底向上为 GL 帧缓冲原序）。仅供诊断/快照。
    /// resolve 延迟到本次调用按需补做（`end_frame` 无效果消费时直接 MSAA 上屏）；
    /// 调用窗口：`end_frame` 之后、下一次 `begin_frame` 之前（此后 MSAA 已清屏）。
    /// 返回 false 表示后端不可用或读回失败。
    [[nodiscard]] auto read_pixels(std::vector<std::uint8_t> &out) -> bool;

    // ---- RhiBackend 能力位与流式纹理契约（specification/03 §8.7）----

    /// @brief GL 3.3 core 能力位：gpu=true；原生表面导入不实现（恒 false）；无 compute。
    [[nodiscard]] auto capabilities() const -> RhiCapabilities override;

    /// @brief 取常驻流式纹理槽（键寻址，槽复用、尺寸变化就地重定义；与 `DrawImage`
    /// 流式分支共享同一存储）。@return 句柄（非零）；`0` = 后端不可用。
    [[nodiscard]] auto acquire_stream_image(std::uint64_t key, int width, int height) -> StreamImageId override;

    /// @brief 流式图像增量更新：`pixels` 为整图像素基址（RGBA8 直色非预乘），
    /// `stride_bytes` 行跨距字节数（`0` = 紧凑行，槽宽 × 4），脏矩形 (x,y,w,h) sub-upload。
    auto update_stream_image(StreamImageId id, const std::uint8_t *pixels, std::size_t stride_bytes, int x, int y,
                             int w, int h) -> void override;

    /// @brief 释放流式纹理槽（先落地待提交批再删除；句柄此后无效，重复释放无害）。
    auto release_stream_image(StreamImageId id) -> void override;

    /// @brief 原生表面导入：GL 3.3 core 不实现（能力位恒 false），调用即单次告警并返回
    /// `0`，调用方回退 CPU 上传路径。
    [[nodiscard]] auto import_native_surface(const NativeSurfaceFrame &frame) -> StreamImageId override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora::rhi
