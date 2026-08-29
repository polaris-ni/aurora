#include "aurora/render/font_discovery.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/render/freetype_library.h"
#include "aurora/render/noto_font_data.h"

namespace aurora::render {

namespace {
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, bugprone-throwing-static-initialization)
// 字体发现模块需要跨调用保持状态的全局可变容器；收敛在匿名命名空间内，不对外暴露。
std::unordered_map<std::string, std::vector<std::shared_ptr<FontFace>>> g_registry;
bool g_initialized = false;
int g_next_id = 1;

// resolve_faces 结果缓存：持有 shared_ptr 保证 FontFace 生命周期，
// 返回的裸指针由缓存的 shared_ptr 引用计数保活，避免每帧重复构造 vector + 去重。
std::unordered_map<std::string, std::vector<std::shared_ptr<FontFace>>> g_resolve_cache;
// 裸指针缓存：命中时直接返回已构造好的 vector<FontFace*>，零分配。
std::unordered_map<std::string, std::vector<FontFace *>> g_resolve_ptr_cache;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, bugprone-throwing-static-initialization)

auto invalidate_resolve_cache() -> void {
    g_resolve_cache.clear();
    g_resolve_ptr_cache.clear();
}

auto make_face_from_memory(std::vector<std::uint8_t> bytes) -> std::shared_ptr<FontFace> {
    const FT_Library lib = ft_library();
    if (lib == nullptr) {
        return nullptr;
    }
    auto ff = std::make_shared<FontFace>();
    ff->mem = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    if (FT_New_Memory_Face(lib, ff->mem->data(), static_cast<FT_Long>(ff->mem->size()), 0, &ff->face) != 0) {
        return nullptr;
    }
    if (FT_Select_Charmap(ff->face, FT_ENCODING_UNICODE) != 0) {
        ff->face->charmap = (ff->face->charmaps != nullptr) ? *ff->face->charmaps : nullptr;
    }
    ff->id = g_next_id++;
    return ff;
}

auto make_face_from_file(const std::string &path) -> std::shared_ptr<FontFace> {
    const FT_Library lib = ft_library();
    if (lib == nullptr) {
        return nullptr;
    }
    auto ff = std::make_shared<FontFace>();
    const FT_Error err = FT_New_Face(lib, path.c_str(), 0, &ff->face);
    if (err != 0) {
        return nullptr;
    }
    // 偏好 Unicode 字符映射，确保 FT_Get_Char_Index 对 CJK/Unicode 码点正确命中。
    if (FT_Select_Charmap(ff->face, FT_ENCODING_UNICODE) != 0) {
        ff->face->charmap = (ff->face->charmaps != nullptr) ? *ff->face->charmaps : nullptr;
    }
    ff->id = g_next_id++;
    return ff;
}

auto push_face(const std::string &family, const std::shared_ptr<FontFace> &ff) -> void {
    if (ff) {
        g_registry[family].push_back(ff);
    }
}

auto register_system_fallbacks() -> void;
} // namespace

auto init_font_discovery() -> void {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    if (ft_library() == nullptr) {
        return;
    }
    // 内嵌 Noto Sans（OFL）作为全平台确定性默认字体（latin）。
    const auto noto_data = noto_sans_ttf();
    std::vector<std::uint8_t> noto(noto_data.begin(), noto_data.end());
    auto nf = make_face_from_memory(std::move(noto));
    if (nf) {
        // 同一 FT_Face 挂到多个逻辑名构成默认链。
        g_registry[""].push_back(nf);
        g_registry["sans-serif"].push_back(nf);
        g_registry["Noto Sans"].push_back(nf);
        g_registry["default"].push_back(nf);
    }
    // 平台系统字体回退（含 CJK），保证缺字非 tofu。
    register_system_fallbacks();
}

auto shutdown_font_discovery() -> void {
    for (auto &item : g_registry | std::views::values) {
        for (const auto &ff : item) {
            if (ff && (ff->face != nullptr)) {
                FT_Done_Face(ff->face);
                ff->face = nullptr;
            }
        }
    }
    g_registry.clear();
    g_resolve_cache.clear();
    g_resolve_ptr_cache.clear();
    g_initialized = false;
    ft_shutdown();
}

auto register_font_memory(const std::string &family, std::vector<std::uint8_t> bytes) -> void {
    init_font_discovery();
    push_face(family, make_face_from_memory(std::move(bytes)));
    invalidate_resolve_cache();
}

auto register_font_file(const std::string &family, const std::string &path) -> void {
    init_font_discovery();
    push_face(family, make_face_from_file(path));
    invalidate_resolve_cache();
}

auto set_default_font_file(const std::string &path) -> void {
    init_font_discovery();
    g_registry[""].clear();
    g_registry["sans-serif"].clear();
    const auto ff = make_face_from_file(path);
    if (ff) {
        g_registry[""].push_back(ff);
        g_registry["sans-serif"].push_back(ff);
    }
    invalidate_resolve_cache();
}

auto add_default_face(const std::shared_ptr<FontFace> &ff) -> void {
    init_font_discovery();
    if (ff) {
        g_registry[""].push_back(ff);
        g_registry["sans-serif"].push_back(ff);
    }
    invalidate_resolve_cache();
}

auto resolve_faces(const std::string &family) -> const std::vector<FontFace *> & {
    init_font_discovery();
    // 裸指针缓存命中：直接返回引用，零分配。
    const auto pit = g_resolve_ptr_cache.find(family);
    if (pit != g_resolve_ptr_cache.end()) {
        return pit->second;
    }
    // shared_ptr 缓存命中：从 owned 重建裸指针 vector 并缓存。
    const auto cit = g_resolve_cache.find(family);
    if (cit != g_resolve_cache.end()) {
        auto &ptrs = g_resolve_ptr_cache[family];
        ptrs.reserve(cit->second.size());
        for (auto &sp : cit->second) {
            ptrs.push_back(sp.get());
        }
        return ptrs;
    }
    std::vector<std::shared_ptr<FontFace>> owned;
    auto emit = [&](const std::string &key) -> void {
        const auto it = g_registry.find(key);
        if (it != g_registry.end()) {
            for (auto &ff : it->second) {
                owned.push_back(ff);
            }
        }
    };
    if (!family.empty()) {
        emit(family);
        if (family == "serif") {
            emit("Times New Roman");
        } else if (family == "monospace" || family == "mono") {
            emit("Consolas");
        }
    }
    // 默认链兜底（去重在末尾处理）。
    emit("");
    emit("sans-serif");
    emit("default");
    // 去重：按裸指针地址去重，保留首次出现。
    std::vector<std::shared_ptr<FontFace>> uniq;
    auto &seen = g_resolve_ptr_cache[family];
    for (auto &sp : owned) {
        if (std::ranges::find(seen, sp.get()) == seen.end()) {
            seen.push_back(sp.get());
            uniq.push_back(sp);
        }
    }
    g_resolve_cache[family] = std::move(uniq);
    return seen;
}

namespace {

auto register_system_fallbacks() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    // 系统字体目录；拉丁回退 + CJK 回退（确保 CJK 非 tofu）。
    struct SysFont {
        const char *file;
    };
    constexpr std::array<const char *, 2> roots = { "C:\\Windows\\Fonts", nullptr };
    constexpr std::array candidates = {
        SysFont{ "segoeui.ttf" },                            // 拉丁回退
        SysFont{ "arial.ttf" },   SysFont{ "msyh.ttc" },     // 中日韩（微软雅黑）
        SysFont{ "msyh.ttf" },    SysFont{ "simsun.ttc" },   // 中文（宋体）
        SysFont{ "simsun.ttf" },  SysFont{ "MSGOTHIC.TTC" }, // 日文
        SysFont{ "malgun.ttf" },                             // 韩文（微软雅黑韩文）
    };
    for (const char *root : roots) {
        if (root == nullptr) {
            continue;
        }
        const std::string base = root;
        for (const auto &c : candidates) {
            const std::string path = base + "\\" + c.file;
            auto ff = make_face_from_file(path);
            if (ff) {
                add_default_face(ff);
            }
        }
    }
#elif defined(AURORA_PLATFORM_LINUX)
    // 常见发行版字体路径（Fedora / Debian•Ubuntu / Arch）；拉丁回退 + CJK 回退（确保 CJK 非 tofu）。
    // 不依赖 fontconfig：直接探测候选文件，保持零三方依赖与确定性。
    const std::array<const char *, 11> candidates = {
        // 拉丁回退
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",                 // Fedora
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",                   // Debian/Ubuntu
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                               // Arch
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf", // Fedora
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",   // Debian/Ubuntu
        // 中日韩（Noto CJK / 文泉驿）
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc", // Fedora
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",              // Debian/Ubuntu
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",                   // Arch
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    for (const char *path : candidates) {
        auto ff = make_face_from_file(path);
        if (ff) {
            add_default_face(ff);
        }
    }
#else
    (void)0;
#endif
}

} // namespace

} // namespace aurora::render
