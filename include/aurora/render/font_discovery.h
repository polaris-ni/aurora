#pragma once

#include <ft2build.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include FT_FREETYPE_H

namespace aurora::render {

/// @brief 一个已加载的字体面（来自内存字节或系统字体文件）。
struct FontFace {
    FT_Face face = nullptr;  ///< 底层 FreeType face 句柄；生命周期由 FontFace::mem（内存字体）或平台字体管理器持有
    int id = 0;  ///< 图集缓存键所用的稳定序号
    int weight = 400;  ///< CSS 字重 100..900；FT style_flags 判定（bold→700，否则 400）；供 resolve_faces 按字重选面
    std::shared_ptr<std::vector<std::uint8_t>> mem;  ///< 内存字体字节（须保持存活至 face 释放）
};

/// @brief 初始化字体发现：懒注册内嵌 Noto Sans 与平台系统回退；重复调用为幂等。
/// @note 引擎首次用到字体时自动完成，无需显式调用点。
auto init_font_discovery() -> void;

/// @brief 关闭字体发现：释放所有已注册 FT_Face 并清空注册表；仅在进程退出路径调用。
auto shutdown_font_discovery() -> void;

/// @brief 注册内存字体（family 为空表示默认 sans-serif）。
/// @param family 逻辑字体族名；空串代表默认链首（同时挂到 `""`/`sans-serif` 键）。
/// @param bytes TTF/OTF 字体字节；由本接口接管所有权（内部转 shared_ptr 常驻）。
auto register_font_memory(const std::string &family, std::vector<std::uint8_t> bytes) -> void;

/// @brief 注册字体文件（family 为空表示默认）。
/// @param family 逻辑字体族名；空串代表默认链。
/// @param path 字体文件绝对/相对路径；由 FreeType 打开。
auto register_font_file(const std::string &family, const std::string &path) -> void;

/// @brief 覆盖默认链：清除 "" / "sans-serif" 并以指定字体文件作为默认（family 为空）。
/// @param path 用作新默认链首的字体文件路径。
auto set_default_font_file(const std::string &path) -> void;

/// @brief 解析逻辑 family 为有序候选 FT_Face 列表（含默认链兜底，供缺字回退）。
/// @param family 逻辑字体族名；空串走默认链。
/// @param weight 请求字重 100..900；精确匹配的面排最前，其余按字重距离升序稳定排列。
/// @return 该 (family, weight) 的候选 FontFace 指针列表引用（内部缓存常驻，勿持久化裸指针跨 shutdown）。
/// @note `find_glyph` 取首个含该字形的面：有粗体面时粗体优先命中；缺字回退到其他字重/回退面
///       （脚本回退语义不变）。
/// @note 本重载**不含**按族回退链，等价于回退链为空；见下个重载。
[[nodiscard]] auto resolve_faces(const std::string &family, int weight = 400) -> const std::vector<FontFace *> &;

/// @brief 解析逻辑 family 为有序候选 FT_Face 列表，缺字回退链**按族可配**（顺序由调用方给定）。
///
/// 供「等宽 + CJK 缺字回退」这类需要**本族缺字先看哪一族**的消费方使用：全局默认链是进程级的
/// 固定序列（`add_default_face` 不产生族名，调用方既看不到它的内容也无法调整顺序或插入族名），
/// 因此「本族 → 指定族 → … → 全局链」这条中间段在本重载之前没有任何表达入口。
///
/// 语义（三条不变量）：
/// 1. **顺序即语义，框架不重排**。链上族按 `fallback_families` 给定顺序依次追加到「本族自身的面」
///    之后，全局默认链（`""` / `sans-serif` / `default`）仍只作**尾部回落**。
///    链内部的字重排序只作用于**单个族自身**（与无链时同口径：精确匹配字重的面排该族首位、
///    其余按字重距离升序），**不跨族重排**——否则「A 族优先于 B 族」的声明会被打乱。
/// 2. **链上族名与 `list_font_families()` 同源**。链上任一族名都经与族名枚举**同一条**懒加载路径
///    解析（目录 catalog → `materialize_catalog_family`），故只要族名取自 `list_font_families()`
///    的结果，`resolve_faces` 一定解析得到属于该族的面；解析不到（未安装 / 拼错）的族**跳过**，
///    不阻断后续链段，最终仍由全局链兜底。
/// 3. **不指定即零影响**。`fallback_families` 为空时，本函数与 `resolve_faces(family, weight)`
///    走同一条代码路径、返回同一份缓存，输出逐字节一致（既有消费者不受影响）。
///
/// @param family 主族名（链首）；语义与 `resolve_faces(family, weight)` 完全一致。
/// @param weight 请求字重 100..900；作用于主族与链上**每个**族。
/// @param fallback_families 按族的回退链（可为空；空串与重复项按「未解析到面」跳过并去重）。
/// @return 该 (family, weight, 链) 的候选 FontFace 指针列表引用（内部缓存常驻，
///         勿持久化裸指针跨 shutdown）。
[[nodiscard]] auto resolve_faces(const std::string &family, int weight, std::span<const std::string> fallback_families)
    -> const std::vector<FontFace *> &;

/// @brief 内部：向默认链追加候选 FT_Face（平台字体发现使用）。
/// @param ff 待加入 `""`/`sans-serif` 默认链的字体面共享指针；空指针被忽略。
auto add_default_face(const std::shared_ptr<FontFace> &ff) -> void;

/// @brief 一个可枚举的字体族（供「字体族下拉」这类 UI 直接消费）。
struct FontFamilyInfo {
    std::string family;  ///< 族名；可原样喂给 `resolve_faces()` 且一定解析得到面（见 `list_font_families` 的同源保证）
    bool monospace = false;  ///< 是否等宽：**以度量判定为准**，族名只作度量不可得时的补充命中
    int face_count = 0;  ///< 该族已注册的面数（>= 1）
};

/// @brief 列出当前可用的字体族（按 `family` **升序、去重**，下拉数据源要求的稳定序）。
///
/// **同源保证（核心不变量）**：返回的每个 `family` 都保证 `resolve_faces(family, weight)` 能解析出
/// **属于该族**的面，而不是悄悄落到默认链——枚举出来的名字点下去解析不到 face，比不枚举更糟。
/// 因此本函数对每个族**真实加载**其面再判定等宽性（系统字体按目录扫描结果懒加载）。
///
/// **等宽判定以度量为准**：取该族一个面，在同一像素尺寸下比较代表性码点（`'i'` / `'W'` / `'0'`）
/// 的 advance 是否全等；全等即等宽。既有的族名特判（`monospace` / `mono`）**降级为补充命中**——
/// 只在度量不可得（面加载失败 / 缺码点）时生效。故「名字像等宽但度量非等宽」的族判 `false`，
/// 名字表命中不了度量测得出的真等宽族。
///
/// 三平台口径一致（目录扫描 + `FT_Face` 的 `family_name`）：Windows 扫 `C:\\Windows\\Fonts`
/// （递归），Linux 扫 `/usr/share/fonts` / `/usr/local/share/fonts` / `~/.fonts` /
/// `~/.local/share/fonts`，macOS 扫 `/System/Library/Fonts` / `/Library/Fonts` / `~/Library/Fonts`。
/// **不引入 fontconfig**（保持零三方依赖）：它给出的族名与 `FT_Face::family_name` 不同源，
/// 反而会让三平台的口径对不齐。返回的**集合**允许因平台装的字体不同而不同，但排序、去重、
/// 等宽判定、与 `resolve_faces` 的同源性这四处必须逐平台一致。
///
/// 伪族名 `""` / `"sans-serif"` / `"default"` 是默认链的键而非族名，**不**出现在结果里
/// （它们指向同一个 Noto Sans 面，列出来只会让下拉里出现三个等价项）。
///
/// 无头（headless）与其余后端走**同一实现**（字体发现不按后端分支）：至少内置族与经
/// `register_font_*` 注册的族一定出现在结果里，故无头 / CI 通道也能断言同源性。
///
/// 缓存：目录扫描结果首次调用时建立，随 `shutdown_font_discovery()` 与任一 `register_font_*`
/// 自然失效；调用方**无需**手动刷新。
/// 未初始化时（未调 `init_font_discovery()`）返回仅含内置族 / 已注册族的集合，不崩溃。
///
/// @param monospace_only `true` 时只返回 `monospace == true` 的族（是全集的子集，判据自明）。
/// @return 升序、去重、同源可用的族列表；无任何可用族时为空。
[[nodiscard]] auto list_font_families(bool monospace_only = false) -> std::vector<FontFamilyInfo>;

}  // namespace aurora::render
