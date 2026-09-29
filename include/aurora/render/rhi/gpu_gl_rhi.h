#pragma once

/// @brief GPU 栅格后端声明：GL 3.3 core 函数表 `GLFn`、加载器 `load_gl` 与 `DisplayList` 消费者 `GpuGlRhi`。
/// @file gpu_gl_rhi.h

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
// （同名同宽，实际无害；但属已知边界，勿在本头之外扩散这些别名）。
//
// 本区别名（`GLenum_` / `GLuint_`…）按 `TypeAliasCase: CamelCase` 必然告警：名字逐字取自 GL
// 官方 typedef（`GLenum`），尾下划线正是为规避与 `<GL/gl.h>` 同名 typedef 的碰撞而加——改名或
// 去下划线都会丢掉「与原生类型一一对应」这层可读性，且属 CODING_STANDARDS.md §2 的官方名镜像
// 豁免面。命名豁免无法按命名空间限定，故按区间逐点豁免。
// NOLINTBEGIN(readability-identifier-naming)
/// @brief 官方 `GLenum` 的镜像别名：GL 枚举/位域参数类型（与 `std::uint32_t` 同宽）。本组别名
/// （`GLenum_` / `GLuint_`…）仅供本头 `GLFn` 签名使用，与原生 GL typedef 逐一同宽，勿扩散到本头之外。
using GLenum_ = std::uint32_t;
using GLboolean_ = std::uint8_t;  ///< 镜像官方 `GLboolean`：布尔实参，与 `std::uint8_t` 同宽。
using GLbitfield_ = std::uint32_t;  ///< 镜像官方 `GLbitfield`：缓冲位掩码实参（如 `glClear` 的 mask）。
using GLint_ = std::int32_t;  ///< 镜像官方 `GLint`：有符号整型实参。
using GLsizei_ = std::int32_t;  ///< 镜像官方 `GLsizei`：尺寸/计数整型实参。
using GLuint_ = std::uint32_t;  ///< 镜像官方 `GLuint`：GL 对象名称（着色器/程序/缓冲/纹理/FBO 等句柄）。
using GLsizeiptr_ = std::ptrdiff_t;  ///< 镜像官方 `GLsizeiptr`：缓冲区字节大小（有符号）。
using GLfloat_ = float;  ///< 镜像官方 `GLfloat`：单精度浮点实参。
using GLchar_ = char;  ///< 镜像官方 `GLchar`：着色器源码字符。
using GLubyte_ = std::uint8_t;  ///< 镜像官方 `GLubyte`：字节数据（`glGetString` 返回元素等）。
// NOLINTEND(readability-identifier-naming)

/// @brief GL 3.3 core 函数表：经加载器逐个 `GetProcAddress` 装载，签名与 GL 官方原型同宽。
///
/// 不依赖系统 `<GL/gl.h>`（Windows 仅声明 1.1）、不引入 GLAD/gl3w 三方头。函数指针缺项
/// （驱动过老 / 截断实现）由 `load_gl` 置空，`GpuGlRhi` 构造时检测并整体判败（软件回退）。
struct GLFn {
    /// @brief [着色器与程序] glCreateShader：创建指定类型着色器对象并返回名称。
    GLuint_ (*create_shader)(GLenum_ type) = nullptr;
    /// @brief glShaderSource：以 count 段源码替换着色器内容（length 为 null 按 strlen）。
    void (*shader_source)(GLuint_ shader, GLsizei_ count, const GLchar_ *const *str, const GLint_ *length) = nullptr;
    void (*compile_shader)(GLuint_ shader) = nullptr;  ///< glCompileShader：编译着色器（成败经 get_shader_iv 查询）。
    /// @brief glGetShaderiv：读取着色器整型状态（COMPILE_STATUS / INFO_LOG_LENGTH 等）。
    void (*get_shader_iv)(GLuint_ shader, GLenum_ pname, GLint_ *params) = nullptr;
    /// @brief glGetShaderInfoLog：读取着色器编译日志（length 为 null 则不回报长度）。
    void (*get_shader_info_log)(GLuint_ shader, GLsizei_ buf_size, GLsizei_ *length, GLchar_ *info_log) = nullptr;
    void (*delete_shader)(GLuint_ shader) = nullptr;  ///< glDeleteShader：删除着色器对象（仍附着于程序时延迟删除）。
    GLuint_ (*create_program)() = nullptr;  ///< glCreateProgram：创建空程序对象并返回名称。
    void (*attach_shader)(GLuint_ program, GLuint_ shader) = nullptr;  ///< glAttachShader：把着色器附着到程序。
    /// @brief glLinkProgram：链接已附着着色器为可执行程序（状态经 get_program_iv 查询）。
    void (*link_program)(GLuint_ program) = nullptr;
    /// @brief glGetProgramiv：读取程序对象整型状态（LINK_STATUS / INFO_LOG_LENGTH 等）。
    void (*get_program_iv)(GLuint_ program, GLenum_ pname, GLint_ *params) = nullptr;
    /// @brief glGetProgramInfoLog：读取程序链接日志。
    void (*get_program_info_log)(GLuint_ program, GLsizei_ buf_size, GLsizei_ *length, GLchar_ *info_log) = nullptr;
    void (*delete_program)(GLuint_ program) = nullptr;  ///< glDeleteProgram：删除程序对象（仍在使用时延迟删除）。
    void (*use_program)(GLuint_ program) = nullptr;  ///< glUseProgram：设程序为当前渲染状态（0 = 解绑）。
    /// @brief glGetUniformLocation：按名查询 uniform 变量位置（-1 = 未找到）。
    GLint_ (*get_uniform_location)(GLuint_ program, const GLchar_ *name) = nullptr;
    /// @brief glUniform1i：写入整型 uniform（采样器 → 纹理单元绑定等）。
    void (*uniform1i)(GLint_ location, GLint_ v0) = nullptr;
    void (*uniform1f)(GLint_ location, GLfloat_ v0) = nullptr;  ///< glUniform1f：写入单分量浮点 uniform。
    void (*uniform2f)(GLint_ location, GLfloat_ v0, GLfloat_ v1) = nullptr;  ///< glUniform2f：写入二分量浮点 uniform。
    /// @brief glUniform3f：写入三分量浮点 uniform。
    void (*uniform3f)(GLint_ location, GLfloat_ v0, GLfloat_ v1, GLfloat_ v2) = nullptr;
    /// @brief glUniform4f：写入四分量浮点 uniform（颜色/vec4 参数）。
    void (*uniform4f)(GLint_ location, GLfloat_ v0, GLfloat_ v1, GLfloat_ v2, GLfloat_ v3) = nullptr;

    /// @brief [顶点数组与缓冲] glGenVertexArrays：生成 n 个 VAO 名称写入 arrays。
    void (*gen_vertex_arrays)(GLsizei_ n, GLuint_ *arrays) = nullptr;
    /// @brief glDeleteVertexArrays：删除 n 个 VAO（无效名称忽略）。
    void (*delete_vertex_arrays)(GLsizei_ n, const GLuint_ *arrays) = nullptr;
    void (*bind_vertex_array)(GLuint_ array) = nullptr;  ///< glBindVertexArray：绑定当前 VAO（0 = 解绑）。
    void (*gen_buffers)(GLsizei_ n, GLuint_ *buffers) = nullptr;  ///< glGenBuffers：生成 n 个缓冲对象名称写入 buffers。
    void (*delete_buffers)(GLsizei_ n, const GLuint_ *buffers) = nullptr;  ///< glDeleteBuffers：删除 n 个缓冲对象。
    /// @brief glBindBuffer：把缓冲对象绑到目标槽位（顶点/索引缓冲）。
    void (*bind_buffer)(GLenum_ target, GLuint_ buffer) = nullptr;
    /// @brief glBufferData：按 size 字节重定义目标缓冲存储（data 为 null 仅分配，usage = 用途提示）。
    void (*buffer_data)(GLenum_ target, GLsizeiptr_ size, const void *data, GLenum_ usage) = nullptr;
    /// @brief glEnableVertexAttribArray：启用给定索引的顶点属性数组。
    void (*enable_vertex_attrib_array)(GLuint_ index) = nullptr;
    /// @brief glVertexAttribPointer：描述属性格式（size 分量数/type 类型/归一化/行跨距，pointer =
    /// 绑定缓冲内字节偏移）。
    void (*vertex_attrib_pointer)(GLuint_ index, GLint_ size, GLenum_ type, GLboolean_ normalized, GLsizei_ stride,
                                  const void *pointer) = nullptr;

    /// @brief [纹理] glGenTextures：生成 n 个纹理名称写入 textures。
    void (*gen_textures)(GLsizei_ n, GLuint_ *textures) = nullptr;
    void (*delete_textures)(GLsizei_ n, const GLuint_ *textures) = nullptr;  ///< glDeleteTextures：删除 n 个纹理对象。
    /// @brief glBindTexture：把纹理绑到当前纹理单元的目标槽（0 = 解绑）。
    void (*bind_texture)(GLenum_ target, GLuint_ texture) = nullptr;
    void (*active_texture)(GLenum_ unit) = nullptr;  ///< glActiveTexture：选择当前纹理单元。
    /// @brief glTexImage2D：定义 level 层为 width×height 纹理存储（internalformat = 存储格式，format/type =
    /// 源布局，border 恒 0）。
    void (*tex_image_2d)(GLenum_ target, GLint_ level, GLint_ internalformat, GLsizei_ width, GLsizei_ height,
                         GLint_ border, GLenum_ format, GLenum_ type, const void *pixels) = nullptr;
    /// @brief glTexSubImage2D：局部更新 (xoffset,yoffset) 起 width×height 块（行跨距/对齐经 pixel_store_i）。
    void (*tex_sub_image_2d)(GLenum_ target, GLint_ level, GLint_ xoffset, GLint_ yoffset, GLsizei_ width,
                             GLsizei_ height, GLenum_ format, GLenum_ type, const void *pixels) = nullptr;
    /// @brief glTexParameteri：设置纹理参数（过滤/环绕模式）。
    void (*tex_parameter_i)(GLenum_ target, GLenum_ pname, GLint_ param) = nullptr;
    /// @brief glPixelStorei：设置像素传输参数（本后端用 UNPACK_ALIGNMENT / UNPACK_ROW_LENGTH）。
    void (*pixel_store_i)(GLenum_ pname, GLint_ param) = nullptr;

    /// @brief [状态与绘制] glViewport：设视口映射矩形（原点左下）。
    void (*viewport)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height) = nullptr;
    /// @brief glClearColor：设 glClear 使用的 RGBA 值。
    void (*clear_color)(GLfloat_ red, GLfloat_ green, GLfloat_ blue, GLfloat_ alpha) = nullptr;
    void (*clear)(GLbitfield_ mask) = nullptr;  ///< glClear：按位掩码清除指定缓冲（如 COLOR_BUFFER_BIT）。
    void (*enable)(GLenum_ cap) = nullptr;  ///< glEnable：启用状态开关（BLEND 等）。
    void (*disable)(GLenum_ cap) = nullptr;  ///< glDisable：关闭状态开关。
    /// @brief glScissor：设裁剪矩形；本后端裁剪走 SDF alpha 不经此路径，仅按核心函数表完整装载。
    void (*scissor)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height) = nullptr;
    /// @brief glBlendFuncSeparate：分别设 RGB/Alpha 混合因子（构造期固定直色 src-over）。
    void (*blend_func_separate)(GLenum_ sfactor_rgb, GLenum_ dfactor_rgb, GLenum_ sfactor_alpha,
                                GLenum_ dfactor_alpha) = nullptr;
    /// @brief glDrawElements：索引绘制 count 个 type 元素（indices = 绑定索引缓冲内字节偏移）。
    void (*draw_elements)(GLenum_ mode, GLsizei_ count, GLenum_ type, const void *indices) = nullptr;
    /// @brief glDrawArrays：自 first 起绘制 count 个连续顶点。
    void (*draw_arrays)(GLenum_ mode, GLint_ first, GLsizei_ count) = nullptr;
    void (*flush)() = nullptr;  ///< glFlush：促使已写入的 GL 命令尽快执行（不等待完成）。

    /// @brief [帧缓冲：MSAA 渲染 + resolve + 呈现] glGenFramebuffers：生成 n 个 FBO 名称写入 framebuffers。
    void (*gen_framebuffers)(GLsizei_ n, GLuint_ *framebuffers) = nullptr;
    /// @brief glDeleteFramebuffers：删除 n 个 FBO。
    void (*delete_framebuffers)(GLsizei_ n, const GLuint_ *framebuffers) = nullptr;
    /// @brief glBindFramebuffer：绑定 FBO 到读/写目标（0 = 默认帧缓冲即屏幕）。
    void (*bind_framebuffer)(GLenum_ target, GLuint_ framebuffer) = nullptr;
    /// @brief glFramebufferTexture2D：把纹理指定层挂到附着点（texture 0 = 卸载）。
    void (*framebuffer_texture_2d)(GLenum_ target, GLenum_ attachment, GLenum_ textarget, GLuint_ texture,
                                   GLint_ level) = nullptr;
    /// @brief glFramebufferRenderbuffer：把渲染缓冲挂到附着点。
    void (*framebuffer_renderbuffer)(GLenum_ target, GLenum_ attachment, GLenum_ renderbuffertarget,
                                     GLuint_ renderbuffer) = nullptr;
    /// @brief glCheckFramebufferStatus：查询 FBO 完整性（FRAMEBUFFER_COMPLETE = 可渲染）。
    GLenum_ (*check_framebuffer_status)(GLenum_ target) = nullptr;
    /// @brief glGenRenderbuffers：生成 n 个渲染缓冲名称写入 renderbuffers。
    void (*gen_renderbuffers)(GLsizei_ n, GLuint_ *renderbuffers) = nullptr;
    /// @brief glDeleteRenderbuffers：删除 n 个渲染缓冲。
    void (*delete_renderbuffers)(GLsizei_ n, const GLuint_ *renderbuffers) = nullptr;
    /// @brief glBindRenderbuffer：绑定渲染缓冲到目标槽。
    void (*bind_renderbuffer)(GLenum_ target, GLuint_ renderbuffer) = nullptr;
    /// @brief glRenderbufferStorageMultisample：定义当前渲染缓冲为 samples 级多重采样 width×height 存储（MSAA
    /// 颜色附着）。
    void (*renderbuffer_storage_multisample)(GLenum_ target, GLsizei_ samples, GLenum_ internalformat, GLsizei_ width,
                                             GLsizei_ height) = nullptr;
    /// @brief glBlitFramebuffer：FBO 间位块传输（mask = 拷贝缓冲位，filter = 采样过滤；多重采样 → 单采样即 resolve）。
    void (*blit_framebuffer)(GLint_ src_x0, GLint_ src_y0, GLint_ src_x1, GLint_ src_y1, GLint_ dst_x0, GLint_ dst_y0,
                             GLint_ dst_x1, GLint_ dst_y1, GLbitfield_ mask, GLenum_ filter) = nullptr;
    /// @brief glReadPixels：从当前读 FBO 回读 width×height 到 pixels（格式/类型 = 回读布局）。
    void (*read_pixels)(GLint_ x, GLint_ y, GLsizei_ width, GLsizei_ height, GLenum_ format, GLenum_ type,
                        void *pixels) = nullptr;

    // [查询] glGetString：返回枚举名对应的 NUL 结尾字节串（VERSION 等，生命周期归驱动）。
    const GLubyte_ *(*get_string)(GLenum_ name) = nullptr;
    void (*get_integer_v)(GLenum_ pname, GLint_ *params) = nullptr;  ///< glGetIntegerv：读取当前上下文整型状态。
    /// @brief glGetError：弹出一个错误码并清空（NO_ERROR = 无错）；检出错误即置失败并回退软件路径。
    GLenum_ (*get_error)() = nullptr;

    /// @brief 函数表是否完整（无空指针）。`load_gl` 后判定；测试桩全量填充即完整。
    /// @return true = 全部函数指针槽位均已装载；false = 存在缺项（驱动过老/加载失败），应整体回退软件路径。
    [[nodiscard]] auto complete() const -> bool;
};

/// @brief 经加载回调逐名装载 GL 3.3 core 函数表（如 GLFW 的 `glfwGetProcAddress`）。
/// @param proc 名字 → 函数地址回调（原始 `void*` 返回，调用方负责 `reinterpret_cast` 适配
///             平台类型，与 GLAD/gladLoadGLLoader 同口径）。
/// @return 装载后的函数表：逐槽以 proc 按 `glXxx` 名查询填充，未命中保持 `nullptr`（以
///         `GLFn::complete` 判定整体可用性）；`proc` 为 null 时返回全空表。
auto load_gl(void *(*proc)(const char *name)) -> GLFn;

/// @brief GPU 栅格后端：`DisplayList` 的 OpenGL 3.3 core 消费者（`RhiBackend` + `RhiFrameSink`）。
///
/// 命令消费（`submit`）只做「命令 → 顶点/状态」翻译，**不触 GL**；GL 调用集中在
/// `flush`（批提交）与 `end_frame`（上屏 blit）。批切分保序不重排：管线/裁剪态/
/// 混合态变化即断批。全局 alpha（`SetAlpha`）烘焙进顶点色，不断批。
///
/// 命令覆盖（全部为 GPU 实路径，无跳过降级）：几何组（FillRect/ClearRect/DrawRect/
/// DrawLine/RoundedBorder）、状态组（PushClip/PushClipRounded/PopClip/SetAlpha）、渐变组
/// （LinearGradient/RadialGradient，经 256×1 LUT 纹理采样，语义与软件 `sample_gradient`
/// 对齐）、图像组（DrawImage，PMA 纹理 + `Image::content_hash()` 摘要缓存；Composite，
/// 仿射矩阵直烘四角顶点 + NEAREST 逐像素取样同软件）、文本（DrawText，多页 R8 字形图集
/// ——满页开新页、页数封顶 LRU 淘汰，光栅化复用软件 `GlyphAtlas` 经字形发射桥同源发射）
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
        std::uint32_t vertices = 0;  ///< 本帧提交顶点数
        std::uint32_t skipped_cmds = 0;  ///< 保留字段：当前实现全部为 GPU 实路径、无跳过分支，恒为 0
    };

    /// @brief 默认构造：不装载 GL（`valid()` 为 false），供占位与测试桩场景。
    GpuGlRhi();
    /// @brief 装载构造：以上下文就绪的函数表初始化着色器/缓冲/MSAA 帧缓冲。
    /// 失败（含函数表缺项）时 `valid()` 为 false，不抛异常。
    /// @param fn 已装载的 GL 函数表（`load_gl` 或测试桩填充）；要求调用时当前线程已有就绪的 GL 上下文。
    explicit GpuGlRhi(GLFn fn);
    /// @brief 析构：删除 VAO/VBO/IBO、MSAA/resolve/temp 帧缓冲与渲染缓冲，及全部缓存纹理
    /// （渐变 LUT、图像、字形图集页、流式槽）。
    /// 直接调用 GL 删除函数，须在构造所用上下文仍有效时销毁对象。
    ~GpuGlRhi() override;
    GpuGlRhi(const GpuGlRhi &) = delete;
    auto operator=(const GpuGlRhi &) -> GpuGlRhi & = delete;
    GpuGlRhi(GpuGlRhi &&) = delete;
    auto operator=(GpuGlRhi &&) -> GpuGlRhi & = delete;

    /// @brief GL 资源是否就绪（构造成功且未发生上下文级失败）。
    /// @return true = 函数表完整、初始化成功且未检出 GL 错误；false = 默认构造或已失败。
    [[nodiscard]] auto valid() const -> bool;

    /// @brief 后端名称（`RhiBackend::name` 契约实现）。
    /// @return 恒 `"gpu-gl"`。
    [[nodiscard]] auto name() const -> std::string_view override { return "gpu-gl"; }

    /// @brief 命令消费面视图（`DisplayList::replay` 入口）；帧调度与命令消费同对象。
    /// @return 指向本对象的 `RhiBackend` 引用。
    [[nodiscard]] auto backend() -> RhiBackend & override { return *this; }

    /// @brief 消费一条命令（翻译进当前批；GL 调用延迟到 flush）。
    /// @param cmd 命令描述符（kind 选择翻译分支，bounds 提供几何矩形）。
    /// @param data 命令载荷（颜色/渐变参数/图像/字形串等，按 cmd.kind 解释）。
    auto submit(const DrawCmd &cmd, const CmdData &data) -> void override;

    /// @brief 帧开始（RhiFrameSink）：按需重设 MSAA 帧缓冲尺寸，整帧清全透明零基底，并重置
    /// 批状态、裁剪栈、alpha 与帧计数。
    /// @param device_width 设备像素宽（≤0 直接失败）。
    /// @param device_height 设备像素高（≤0 直接失败）。
    /// @param scale 内容缩放比；非正值按 1.0 处理。
    /// @return true = 帧已就绪可提交命令；false = 后端不可用或帧缓冲建立失败。
    [[nodiscard]] auto begin_frame(int device_width, int device_height, float scale) -> bool override;
    /// @brief 帧结束（RhiFrameSink）：落地尾批后把帧内容 blit 上屏——resolve 纹理仍新鲜时直接从
    /// 它呈现，否则 MSAA 直 blit 默认帧缓冲（resolve 延迟，由后续效果 pass 或 read_pixels 按需补做）。
    auto end_frame() -> void override;

    /// @brief 本帧（自 `begin_frame` 起）累计诊断计数。
    /// @return 本帧统计快照；后端不可用时为全零。
    [[nodiscard]] auto stats() const -> FrameStats;

    /// @brief 字形图集页边长（默认 1024；须在 `begin_frame` 前设置，非正值忽略）。
    /// 常规消费者无须调用；测试用小页覆盖「满页开新页 / 页数封顶 LRU 淘汰」路径。
    /// @param side 新页边长（像素）；仅正值生效。
    auto set_glyph_page_size(int side) -> void;

    /// @brief 当前帧内容读回（RGBA8，行序自底向上为 GL 帧缓冲原序）。仅供诊断/快照。
    /// resolve 延迟到本次调用按需补做（`end_frame` 无效果消费时直接 MSAA 上屏）；
    /// 调用窗口：`end_frame` 之后、下一次 `begin_frame` 之前（此后 MSAA 已清屏）。
    /// @param out 输出缓冲：覆写为设备宽高 × 4 字节的 RGBA8 像素。
    /// @return true = 读回成功；false = 后端不可用、尺寸未就绪或 GL 读回失败。
    [[nodiscard]] auto read_pixels(std::vector<std::uint8_t> &out) -> bool;

    // ---- RhiBackend 能力位与流式纹理契约（specification/03 §8.7）----

    /// @brief GL 3.3 core 能力位：gpu=true；原生表面导入不实现（恒 false）；无 compute。
    /// @return 就绪时仅 gpu 位为 true；不可用（默认构造/已失败）时全零。
    [[nodiscard]] auto capabilities() const -> RhiCapabilities override;

    /// @brief 取常驻流式纹理槽（键寻址，槽复用、尺寸变化就地重定义；与 `DrawImage`
    /// 流式分支共享同一存储）。
    /// @param key 槽的稳定身份键：同键复用同一槽，句柄即该槽的 GL 纹理名。
    /// @param width 槽纹理宽（像素）；与现存槽不一致时就地重定义。
    /// @param height 槽纹理高（像素）。
    /// @return 流式句柄（非零 = 槽 GL 纹理名）；`0` = 后端不可用。
    [[nodiscard]] auto acquire_stream_image(std::uint64_t key, int width, int height) -> StreamImageId override;

    /// @brief 流式图像增量更新：脏矩形 (x,y,w,h) sub-upload 进句柄所指槽。
    /// @param id `acquire_stream_image` 签发的句柄；`0` 或不属于任何现存槽时忽略。
    /// @param pixels 整幅图像素基址（RGBA8 直色非预乘）；null 忽略。
    /// @param stride_bytes 行跨距字节数（`0` = 紧凑行，槽宽 × 4）。
    /// @param x 脏矩形左上角 x（像素，以 pixels 行序计）。
    /// @param y 脏矩形左上角 y（像素）。
    /// @param w 脏矩形宽（非正值忽略）。
    /// @param h 脏矩形高（非正值忽略）；越出槽界的矩形整体忽略（界内性由调用方保证）。
    auto update_stream_image(StreamImageId id, const std::uint8_t *pixels, std::size_t stride_bytes, int x, int y,
                             int w, int h) -> void override;

    /// @brief 释放流式纹理槽（先落地待提交批再删除；句柄此后无效，重复释放无害）。
    /// @param id 待释放句柄；`0` 或不属于任何现存槽时忽略。
    auto release_stream_image(StreamImageId id) -> void override;

    /// @brief 原生表面导入：GL 3.3 core 不实现（能力位恒 false），调用即单次告警并返回
    /// `0`，调用方回退 CPU 上传路径。
    /// @param frame 待导入的原生表面帧（本路径不消费）。
    /// @return 恒 `0`（不支持导入）。
    [[nodiscard]] auto import_native_surface(const NativeSurfaceFrame &frame) -> StreamImageId override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora::rhi
