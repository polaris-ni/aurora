#include "aurora/render/font_discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/render/cascadia_font_data.h"
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
    const FT_Library lib = ft_library();  // NOLINT
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
    // 按 OS/2 style_flags 判定字重：bold 文件记 700，其余 400。供 resolve_faces 按字重选面。
    ff->weight = (std::cmp_not_equal(ff->face->style_flags & FT_STYLE_FLAG_BOLD, 0U)) ? 700 : 400;
    ff->id = g_next_id++;
    return ff;
}

auto make_face_from_file(const std::string &path) -> std::shared_ptr<FontFace> {
    const FT_Library lib = ft_library();  // NOLINT
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
    ff->weight = (std::cmp_not_equal(ff->face->style_flags & FT_STYLE_FLAG_BOLD, 0U)) ? 700 : 400;
    ff->id = g_next_id++;
    return ff;
}

auto push_face(const std::string &family, const std::shared_ptr<FontFace> &ff) -> void {
    if (ff) {
        g_registry[family].push_back(ff);
    }
}

// ---- 系统字体目录 catalog（族名 → 字体文件）----
//
// 为什么要有它：`list_font_families()` 的同源保证要求「枚举出来的族一定解析得到面」，而既有
// 的 `register_system_fallbacks()` 只是**固定候选文件清单**（Win 8 个文件名 / Linux 10 个路径），
// 且经 `add_default_face()` 只挂进 `""` / `"sans-serif"` 两个键——**不产生任何族名**。故系统字体
// 目录扫描是本条从零新增的能力，不是「把已有的扫描暴露出来」。
//
// 为什么只存「族名 → 文件路径」而不预注册全部 FT_Face：Windows 字体目录有 150+ 个字体文件，
// 全量预注册会让进程启动即持有上百个 face。catalog 只记位置，`resolve_faces` 未命中时按目录
// **懒加载**该族；`list_font_families` 需要判等宽时才临时开面（量完即关）。

/// @brief 一个字体族的来源（文件 + face 下标）。
struct FontSource {
    std::string path;  ///< 字体文件绝对路径。
    int index = 0;  ///< 文件内的 face 下标（`.ttc` 集合字体；首版只取 0，见 scan 处注释）。
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::unordered_map<std::string, std::vector<FontSource>> g_catalog;
bool g_catalog_scanned = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// @brief 等宽判定的度量像素尺寸：只用于比较 advance 是否相等，取值本身无语义。
constexpr int kMonospaceProbePx = 16;

/// @brief 系统字体目录（按平台）。三平台同口径：目录扫描 + `FT_Face::family_name`。
/// @return 该平台的字体目录候选根（不存在者由调用方跳过）。
auto system_font_roots() -> std::vector<std::string> {
    std::vector<std::string> roots;
#ifdef AURORA_PLATFORM_WINDOWS
    // Windows 的系统字体目录是固定位置（`GetWindowsDirectory` 引入 shell32 依赖不值得，
    // 且与既有 `register_system_fallbacks()` 的写法保持一致）。
    roots.emplace_back("C:\\Windows\\Fonts");
#elif defined(AURORA_PLATFORM_LINUX)
    roots.emplace_back("/usr/share/fonts");
    roots.emplace_back("/usr/local/share/fonts");
    if (const char *home = std::getenv("HOME"); (home != nullptr) && (home[0] != '\0')) {
        roots.emplace_back(std::string(home) + "/.fonts");
        roots.emplace_back(std::string(home) + "/.local/share/fonts");
    }
#elif defined(AURORA_PLATFORM_MACOS)
    roots.emplace_back("/System/Library/Fonts");
    roots.emplace_back("/Library/Fonts");
    if (const char *home = std::getenv("HOME"); (home != nullptr) && (home[0] != '\0')) {
        roots.emplace_back(std::string(home) + "/Library/Fonts");
    }
#endif
    return roots;
}

/// @brief 是否是可扫描的字体文件扩展名。
/// @param path 文件路径。
/// @return 是 `.ttf` / `.ttc` / `.otf`（小写后比较）时返回 true。
[[nodiscard]] auto is_font_file(const std::filesystem::path &path) -> bool {
    const std::string ext = path.extension().string();
    return (ext == ".ttf") || (ext == ".ttc") || (ext == ".otf");
}

/// @brief 扫描系统字体目录，填 `g_catalog`（族名 → 来源）。幂等：只扫一次。
///
/// 只取每个文件的 **face 0**：`.ttc` 集合字体里同一族的 Regular / Bold / Italic 各占一个 face，
/// 全量遍历会把同一个族名重复登记多次且显著拉长扫描；首版取首面即够用（族名与等宽性都由它决定），
/// 需要按字重分裂时再扩。扫描失败（权限 / 目录不存在）静默跳过——字体目录不可读不是致命错误。
auto ensure_catalog_scanned() -> void {
    if (g_catalog_scanned) {
        return;
    }
    g_catalog_scanned = true;
    const FT_Library lib = ft_library();
    if (lib == nullptr) {
        return;
    }
    for (const std::string &root : system_font_roots()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec || !entry.is_regular_file(ec) || !is_font_file(entry.path())) {
                continue;
            }
            FT_Face probe = nullptr;
            if (FT_New_Face(lib, entry.path().string().c_str(), 0, &probe) != 0) {
                continue;
            }
            if ((probe->family_name != nullptr) && (probe->family_name[0] != '\0')) {
                g_catalog[probe->family_name].push_back(FontSource{.path = entry.path().string(), .index = 0});
            }
            FT_Done_Face(probe);
        }
    }
}

/// @brief 以**度量**判定一个面是否等宽。
///
/// 判据：同一像素尺寸下，代表性码点 `'i'`（窄）/ `'W'`（宽）/ `'0'`（数字）的 advance **全等**。
/// 只比 `'0'` 会把「数字等宽但字母不等宽」的比例字体误判成等宽，故三个码点都要量。
/// @param face 待判定的 FT_Face（须已可用）。
/// @return 度量可得且三者全等时 true；度量不可得时 false（由调用方决定要不要用族名兜底）。
[[nodiscard]] auto advances_are_uniform(FT_Face face) -> bool {
    if (FT_Set_Pixel_Sizes(face, 0, kMonospaceProbePx) != 0) {
        return false;
    }
    int reference = -1;
    for (const FT_ULong cp : {static_cast<FT_ULong>('i'), static_cast<FT_ULong>('W'), static_cast<FT_ULong>('0')}) {
        if (FT_Load_Char(face, cp, FT_LOAD_NO_HINTING) != 0) {
            return false;
        }
        const int advance = static_cast<int>(face->glyph->metrics.horiAdvance);
        if (reference < 0) {
            reference = advance;
        } else if (advance != reference) {
            return false;
        }
    }
    return reference >= 0;
}

/// @brief 族名是否像等宽族（**仅**作度量不可得时的补充命中）。
/// @param family 族名。
/// @return 名字含等宽线索时 true。
[[nodiscard]] auto family_name_looks_monospace(const std::string &family) -> bool {
    const std::string lower = [&] {
        std::string s = family;
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    return lower.find("mono") != std::string::npos;
}

/// @brief 判定一个族是否等宽：**以度量为准，族名只在度量不可得时补充命中**。
/// @param family 族名（量不到时的兜底依据）。
/// @param face 该族的一个面；nullptr 表示无面可用。
/// @return 是否等宽。
[[nodiscard]] auto family_is_monospace(const std::string &family, FT_Face face) -> bool {
    if (face == nullptr) {
        return family_name_looks_monospace(family);
    }
    // 度量可得即**只看度量**：名字叫 Mono 但度量非等宽的族必须判 false，否则测试只覆盖到名字表。
    return advances_are_uniform(face);
}

/// @brief 按 catalog 把一个系统字体族的面加载并注册进 `g_registry`。
/// @param family 族名。
/// @return 是否至少注册了一个面。
auto materialize_catalog_family(const std::string &family) -> bool {
    ensure_catalog_scanned();
    const auto it = g_catalog.find(family);
    if (it == g_catalog.end()) {
        return false;
    }
    bool any = false;
    for (const FontSource &src : it->second) {
        auto ff = make_face_from_file(src.path);
        if (ff) {
            push_face(family, ff);
            any = true;
        }
    }
    if (any) {
        invalidate_resolve_cache();
    }
    return any;
}

auto register_system_fallbacks() -> void;
}  // namespace

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
    const auto nf = make_face_from_memory(std::move(noto));
    if (nf) {
        // 同一 FT_Face 挂到多个逻辑名构成默认链。
        g_registry[""].push_back(nf);
        g_registry["sans-serif"].push_back(nf);
        g_registry["Noto Sans"].push_back(nf);
        g_registry["default"].push_back(nf);
    }
    // 内置 Cascadia Code（SIL OFL 1.1）：框架此前**没有任何内置等宽族**——内置族只有非等宽的
    // Noto Sans，于是「系统等宽字体枚举」这条能力在本仓拿不到一个正例（名字像等宽但度量非等宽
    // 的反例倒是可以造）。注册到**它自己的族名**下、不挂默认链：既有默认链行为零改动，
    // 需要等宽的消费方按族名 `"Cascadia Code"` 选即可。
    const auto cascadia_data = cascadia_code_ttf();
    std::vector<std::uint8_t> cascadia(cascadia_data.begin(), cascadia_data.end());
    if (const auto cf = make_face_from_memory(std::move(cascadia))) {
        g_registry["Cascadia Code"].push_back(cf);
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
    // 目录 catalog 的**自然失效点**：与注册表一起清空，下次调用重新扫描。调用方无需手动刷新。
    g_catalog.clear();
    g_catalog_scanned = false;
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

auto resolve_faces(const std::string &family, int weight) -> const std::vector<FontFace *> & {
    init_font_discovery();
    // 缓存键 = family + 请求字重（不同字重的排序结果不同，须分别缓存）。
    const std::string cache_key = family + "#" + std::to_string(weight);
    // 裸指针缓存命中：直接返回引用，零分配。
    const auto pit = g_resolve_ptr_cache.find(cache_key);
    if (pit != g_resolve_ptr_cache.end()) {
        return pit->second;
    }
    // shared_ptr 缓存命中：从 owned 重建裸指针 vector 并缓存。
    const auto cit = g_resolve_cache.find(cache_key);
    if (cit != g_resolve_cache.end()) {
        auto &ptrs = g_resolve_ptr_cache[cache_key];
        ptrs.reserve(cit->second.size());
        for (auto &sp : cit->second) {
            ptrs.push_back(sp.get());
        }
        return ptrs;
    }
    // 系统字体目录的族首次被请求时**按需加载**：`list_font_families()` 承诺「枚举出来的族一定
    // 解析得到属于该族的面」，而这个承诺对目录扫描出来的族只能靠这一步兑现。
    if (!family.empty() && (g_registry.find(family) == g_registry.end())) {
        materialize_catalog_family(family);
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
    auto &seen = g_resolve_ptr_cache[cache_key];
    for (auto &sp : owned) {
        if (std::ranges::find(seen, sp.get()) == seen.end()) {
            seen.push_back(sp.get());
            uniq.push_back(sp);
        }
    }
    // 字重感知排序：精确匹配请求字重的面排最前（保持注册序），其余按字重距离升序稳定排列。
    // find_glyph 取首个含该字形的面 → 有粗体面时粗体字优先命中，缺字仍回退其他字重/脚本回退面。
    // 注意：必须同步重排 `seen`（返回给调用方的裸指针序列），否则排序只作用于 owned 缓存、
    // 实际选面顺序不变，weight 参数形同虚设。
    std::ranges::stable_sort(uniq, [weight](const auto &a, const auto &b) {
        return std::abs(a->weight - weight) < std::abs(b->weight - weight);
    });
    for (std::size_t i = 0; i < uniq.size(); ++i) {
        seen[i] = uniq[i].get();
    }
    g_resolve_cache[cache_key] = std::move(uniq);
    return seen;
}

auto list_font_families(bool monospace_only) -> std::vector<FontFamilyInfo> {
    init_font_discovery();
    ensure_catalog_scanned();

    // 伪族名：默认链的键，不是族名（三者指向同一个 Noto Sans 面，列出来只会让下拉里出现
    // 三个等价项，其中一个还是空串）。
    const auto is_pseudo_family = [](const std::string &name) -> bool {
        return name.empty() || (name == "sans-serif") || (name == "default");
    };

    // 候选族 = 已注册族 ∪ 目录扫描到的族。后者此时尚未开面，等宽判定需要临时开一次。
    std::vector<std::string> names;
    names.reserve(g_registry.size() + g_catalog.size());
    for (const auto &key : g_registry | std::views::keys) {
        if (!is_pseudo_family(key)) {
            names.push_back(key);
        }
    }
    for (const auto &key : g_catalog | std::views::keys) {
        if (!is_pseudo_family(key) && (g_registry.find(key) == g_registry.end())) {
            names.push_back(key);
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    std::vector<FontFamilyInfo> out;
    out.reserve(names.size());
    const FT_Library lib = ft_library();
    for (const std::string &name : names) {
        // 已注册族直接取面；目录扫描到的族临时开一个面量完即关（不常驻，避免持有上百个 face）。
        FT_Face probe = nullptr;
        int count = 0;
        const auto reg = g_registry.find(name);
        if (reg != g_registry.end()) {
            count = static_cast<int>(reg->second.size());
            if (!reg->second.empty()) {
                probe = reg->second.front()->face;
            }
        } else {
            const auto cat = g_catalog.find(name);
            if ((cat != g_catalog.end()) && (lib != nullptr) && !cat->second.empty() &&
                (FT_New_Face(lib, cat->second.front().path.c_str(), 0, &probe) == 0)) {
                count = static_cast<int>(cat->second.size());
            }
        }
        const bool monospace = family_is_monospace(name, probe);
        if ((reg == g_registry.end()) && (probe != nullptr)) {
            FT_Done_Face(probe);
        }
        if ((count <= 0) || (monospace_only && !monospace)) {
            continue;
        }
        out.push_back(FontFamilyInfo{.family = name, .monospace = monospace, .face_count = count});
    }
    return out;
}

namespace {

auto register_system_fallbacks() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    // 系统字体目录；拉丁回退 + CJK 回退（确保 CJK 非 tofu）。
    struct SysFont {
        const char *file;
    };
    constexpr std::array<const char *, 2> roots = {"C:\\Windows\\Fonts", nullptr};
    constexpr std::array candidates = {
        SysFont{"segoeui.ttf"},  // 拉丁回退
        SysFont{"arial.ttf"},   SysFont{"msyh.ttc"},  // 中日韩（微软雅黑）
        SysFont{"msyh.ttf"},    SysFont{"simsun.ttc"},  // 中文（宋体）
        SysFont{"simsun.ttf"},  SysFont{"MSGOTHIC.TTC"},  // 日文
        SysFont{"malgun.ttf"},  // 韩文（微软雅黑韩文）
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
    // 数组大小必须与初始化项个数严格一致：多出的元素会被值初始化为 nullptr，
    // 而 make_face_from_file 的 const std::string& 形参会隐式构造 std::string(nullptr)
    // —— libstdc++ 对此无条件抛 "construction from null"，会让 init_font_discovery() 直接失败。
    constexpr std::array<const char *, 10> candidates = {
        // 拉丁回退
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",  // Fedora
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // Debian/Ubuntu
        "/usr/share/fonts/TTF/DejaVuSans.ttf",  // Arch
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",  // Fedora
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",  // Debian/Ubuntu
        // 中日韩（Noto CJK / 文泉驿）
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",  // Fedora
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",  // Debian/Ubuntu
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",  // Arch
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    for (const char *path : candidates) {
        if (path == nullptr) {
            continue;
        }
        auto ff = make_face_from_file(path);
        if (ff) {
            add_default_face(ff);
        }
    }
#else
    (void)0;
#endif
}

}  // namespace

}  // namespace aurora::render
