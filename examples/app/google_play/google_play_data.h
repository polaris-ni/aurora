#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"

#include "nlohmann/json.hpp"

namespace gp {

using aurora::Color;
using aurora::Image;
using nlohmann::json;

/// @brief 单条用户评价。
struct Review {
    std::string user;
    float rating = 0.0f;
    std::string text;
    std::string date;
};

/// @brief 应用/媒体条目（数据层核心模型）。
struct AppItem {
    std::string id;
    std::string name;
    std::string developer;
    std::string category; ///< 顶层类目：Apps / Games / Movies / Books
    std::string subcategory;
    float rating = 0.0f; ///< 0..5
    int rating_count = 0;
    std::string downloads; ///< 如 "1M+"
    float size_mb = 0.0f;
    std::string version;
    std::string updated;
    Color color_a{ 0x1A, 0x73, 0xE8, 0xFF };
    Color color_b{ 0x34, 0xA8, 0x53, 0xFF };
    Image icon; ///< 程序化合成图标
    std::vector<Image> screenshots;
    std::string description;
    bool is_app = true; ///< true=应用/游戏，false=影音/图书
};

/// @brief 数据请求（被 hook 消费，模拟 API endpoint + 查询参数）。
struct DataRequest {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;
};

// ---- 程序化图像合成（纯像素数学，无需字体光栅）----
inline auto make_gradient(int w, int h, Color a, Color b, uint32_t seed) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    std::mt19937 rng((seed * 2654435761u) + 12345u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float t = static_cast<float>(y) / static_cast<float>(std::max(1, h - 1));
            const auto r = static_cast<uint8_t>(static_cast<float>(a.m_r) + (static_cast<float>(b.m_r - a.m_r) * t));
            const auto g = static_cast<uint8_t>(static_cast<float>(a.m_g) + (static_cast<float>(b.m_g - a.m_g) * t));
            const auto bl = static_cast<uint8_t>(static_cast<float>(a.m_b) + (static_cast<float>(b.m_b - a.m_b) * t));
            const size_t idx = ((static_cast<size_t>(y) * static_cast<size_t>(w)) + x) * 4;
            img.pixels[idx] = r;
            img.pixels[idx + 1] = g;
            img.pixels[idx + 2] = bl;
            img.pixels[idx + 3] = 255;
        }
    }
    (void)rng;
    return img;
}

inline auto set_px(Image &img, int x, int y, Color c) -> void {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) {
        return;
    }
    const size_t idx = ((static_cast<size_t>(y) * img.width) + x) * 4;
    img.pixels[idx] = c.m_r;
    img.pixels[idx + 1] = c.m_g;
    img.pixels[idx + 2] = c.m_b;
    img.pixels[idx + 3] = c.m_a;
}

/// @brief 把颜色 c（自带 alpha）以覆盖度 a(0..1) 合成到像素上，得到抗锯齿边缘。
inline auto blend_px(Image &img, int x, int y, Color c, float a) -> void {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) {
        return;
    }
    a *= static_cast<float>(c.m_a) / 255.0f; // 叠加 motif 自身透明度
    if (a >= 1.0f) {
        set_px(img, x, y, Color{ c.m_r, c.m_g, c.m_b, 0xFF });
        return;
    }
    if (a <= 0.0f) {
        return;
    }
    const size_t idx = ((static_cast<size_t>(y) * img.width) + x) * 4;
    const float sa = a;
    const float da = static_cast<float>(img.pixels[idx + 3]) / 255.0f;
    const float out_a = sa + (da * (1.0f - sa));
    if (out_a > 0.0f) {
        img.pixels[idx] = static_cast<uint8_t>(
            ((static_cast<float>(c.m_r) * sa) + (static_cast<float>(img.pixels[idx]) * da * (1.0f - sa))) / out_a);
        img.pixels[idx + 1] = static_cast<uint8_t>(
            ((static_cast<float>(c.m_g) * sa) + (static_cast<float>(img.pixels[idx + 1]) * da * (1.0f - sa))) / out_a);
        img.pixels[idx + 2] = static_cast<uint8_t>(
            ((static_cast<float>(c.m_b) * sa) + (static_cast<float>(img.pixels[idx + 2]) * da * (1.0f - sa))) / out_a);
    }
    img.pixels[idx + 3] = static_cast<uint8_t>(out_a * 255.0f);
}

/// @brief 抗锯齿实心圆：基于距离场 + 像素覆盖度（消除边缘毛刺）。
inline auto fill_circle(Image &img, int cx, int cy, int rad, Color c) -> void {
    const auto r = static_cast<float>(rad);
    const int m = rad + 1;
    for (int y = cy - m; y <= cy + m; ++y) {
        for (int x = cx - m; x <= cx + m; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - static_cast<float>(cx);
            const float dy = (static_cast<float>(y) + 0.5f) - static_cast<float>(cy);
            const float d = std::sqrt((dx * dx) + (dy * dy));
            const float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_px(img, x, y, c, cov);
            }
        }
    }
}

/// @brief 抗锯齿圆角矩形：基于有符号距离场（SDF）+ 像素覆盖度。
inline auto fill_rounded_rect(Image &img, int x0, int y0, int x1, int y1, int r, Color c) -> void {
    const float cx = static_cast<float>(x0 + x1) * 0.5f;
    const float cy = static_cast<float>(y0 + y1) * 0.5f;
    const float hx = static_cast<float>(x1 - x0) * 0.5f;
    const float hy = static_cast<float>(y1 - y0) * 0.5f;
    const auto rad = static_cast<float>(r);
    const int pad = r + 2;
    for (int y = y0 - pad; y <= y1 + pad; ++y) {
        for (int x = x0 - pad; x <= x1 + pad; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float dx = std::fabs(px - cx) - (hx - rad);
            const float dy = std::fabs(py - cy) - (hy - rad);
            const float ox = std::max(dx, 0.0f);
            const float oy = std::max(dy, 0.0f);
            const float sd = std::sqrt((ox * ox) + (oy * oy)) - rad; // <0 在内部
            const float cov = std::clamp(0.5f - sd, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_px(img, x, y, c, cov);
            }
        }
    }
}

/// @brief 抗锯齿填充三角形（任意朝向），基于三角形 SDF。
inline auto fill_triangle(Image &img, float ax, float ay, float bx, float by, float dx, float dy, Color c) -> void {
    const float e0x = bx - ax;
    const float e0y = by - ay;
    const float e1x = dx - bx;
    const float e1y = dy - by;
    const float e2x = ax - dx;
    const float e2y = ay - dy;
    const float sign = ((e0x * e2y) - (e0y * e2x)) >= 0.0f ? 1.0f : -1.0f;
    const float minx = std::fmin(std::fmin(ax, bx), dx);
    const float maxx = std::fmax(std::fmax(ax, bx), dx);
    const float miny = std::fmin(std::fmin(ay, by), dy);
    const float maxy = std::fmax(std::fmax(ay, by), dy);
    const int x0 = static_cast<int>(std::floor(minx)) - 1;
    const int x1 = static_cast<int>(std::ceil(maxx)) + 1;
    const int y0 = static_cast<int>(std::floor(miny)) - 1;
    const int y1 = static_cast<int>(std::ceil(maxy)) + 1;
    const auto proj = [](float vd, float ed) -> float {
        const float t = ed > 0.0f ? vd / ed : 0.0f;
        return std::clamp(t, 0.0f, 1.0f);
    };
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float v0x = px - ax;
            const float v0y = py - ay;
            const float v1x = px - bx;
            const float v1y = py - by;
            const float v2x = px - dx;
            const float v2y = py - dy;
            const float c0 = proj((v0x * e0x) + (v0y * e0y), (e0x * e0x) + (e0y * e0y));
            const float c1 = proj((v1x * e1x) + (v1y * e1y), (e1x * e1x) + (e1y * e1y));
            const float c2 = proj((v2x * e2x) + (v2y * e2y), (e2x * e2x) + (e2y * e2y));
            const float pq0x = v0x - (e0x * c0);
            const float pq0y = v0y - (e0y * c0);
            const float pq1x = v1x - (e1x * c1);
            const float pq1y = v1y - (e1y * c1);
            const float pq2x = v2x - (e2x * c2);
            const float pq2y = v2y - (e2y * c2);
            const float d0 = (pq0x * pq0x) + (pq0y * pq0y);
            const float d1 = (pq1x * pq1x) + (pq1y * pq1y);
            const float d2 = (pq2x * pq2x) + (pq2y * pq2y);
            const float w0 = sign * ((v0x * e0y) - (v0y * e0x));
            const float w1 = sign * ((v1x * e1y) - (v1y * e1x));
            const float w2 = sign * ((v2x * e2y) - (v2y * e2x));
            const float md = std::fmin(std::fmin(d0, d1), d2);
            const float mw = std::fmin(std::fmin(w0, w1), w2);
            const float sd = -std::sqrt(md) * (mw >= 0.0f ? 1.0f : -1.0f);
            const float cov = std::clamp(0.5f - sd, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_px(img, x, y, c, cov);
            }
        }
    }
}

/// @brief 合成应用图标：渐变底 + 白色几何 motif（按 motif 取不同形状）。
/// 注意：白色 motif 仅作点缀，尺寸须明显小于图标，否则图标会看起来像"白方块"。
/// 源分辨率取 128（而非 192）：网格图标显示 64px、hero 96px、banner 80px 均向下采样，
/// 128 已足够锐利；较原 192 把 catalog 常驻从 ~27MB 降到 ~12MB（192 个图标 × 144KB→64KB）。
/// 仅在 150%+/2x 高 DPI 下 hero 会轻微发软，属可接受的取舍。
inline auto make_app_icon(Color a, Color b, int motif) -> Image {
    constexpr int side = 128;
    Image img = make_gradient(side, side, a, b, static_cast<uint32_t>(motif + 1));
    constexpr Color white{ 0xFF, 0xFF, 0xFF, 0xD9 };
    constexpr int cx = side / 2;
    constexpr int cy = side / 2; // 居中（96,96）
    switch (motif % 4) {
    case 0: // 圆
        fill_circle(img, cx, cy, 34, white);
        break;
    case 1: { // 三角/菱形（播放键），抗锯齿
        fill_triangle(img, static_cast<float>(cx), 64.0f, static_cast<float>(cx - 30), 96.0f,
                      static_cast<float>(cx + 30), 96.0f, white);
        fill_triangle(img, static_cast<float>(cx - 30), 96.0f, static_cast<float>(cx + 30), 96.0f,
                      static_cast<float>(cx), 128.0f, white);
        break;
    }
    case 2: { // 聊天气泡：针对 "Chat" 等应用，避免像白方块
        // 气泡主体：横向圆角矩形（已抗锯齿）
        fill_rounded_rect(img, 68, 72, 124, 108, 12, white);
        // 左下小尾巴（抗锯齿三角形）
        fill_triangle(img, 80.0f, 108.0f, 96.0f, 108.0f, 68.0f, 124.0f, white);
        break;
    }
    default: // 双圆
        fill_circle(img, cx - 22, cy, 22, white);
        fill_circle(img, cx + 22, cy, 22, white);
        break;
    }
    return img;
}

/// @brief 合成截图预览（渐变 + 简单形状装饰）。
inline auto make_screenshot(Color a, Color b, int idx) -> Image {
    Image img = make_gradient(280, 160, a, b, static_cast<uint32_t>((idx * 31) + 7));
    constexpr Color white{ 0xFF, 0xFF, 0xFF, 0x55 };
    constexpr Color dark{ 0x20, 0x21, 0x24, 0x66 };
    // 顶部栏
    for (int y = 0; y < 18; ++y) {
        for (int x = 0; x < img.width; ++x) {
            set_px(img, x, y, dark);
        }
    }
    // 中部卡片装饰
    fill_circle(img, 70 + ((idx % 3) * 70), 95, 22, white);
    for (int y = 120; y < 150; ++y) {
        for (int x = 20; x < img.width - 20; ++x) {
            set_px(img, x, y, white);
        }
    }
    return img;
}

/// @brief 合成 Featured 横幅（宽幅渐变 + 留白）。
inline auto make_banner(Color a, Color b, int idx) -> Image {
    Image img = make_gradient(460, 200, a, b, static_cast<uint32_t>((idx * 17) + 3));
    constexpr Color white{ 0xFF, 0xFF, 0xFF, 0x33 };
    for (int i = 0; i < 3; ++i) {
        fill_circle(img, 80 + (i * 150), 100 + ((i % 2) * 30), 30, white);
    }
    return img;
}

using CatalogPtr = std::shared_ptr<const std::vector<AppItem>>;
/// @brief 数据 HOOK：仓库底层数据源。返回目录（含程序化 Image）。
/// 默认实现本地确定性合成，绝不联网；可经 PlayRepository::set_data_hook 替换。
using DataHook = std::function<CatalogPtr(const DataRequest &)>;

inline auto palette_of(int seed) -> std::pair<Color, Color> {
    static const std::vector<std::pair<Color, Color>> pal = {
        { { 0x1A, 0x73, 0xE8, 0xFF }, { 0x34, 0xA8, 0x53, 0xFF } }, // 蓝-绿
        { { 0xEA, 0x43, 0x35, 0xFF }, { 0xFB, 0xBC, 0x04, 0xFF } }, // 红-黄
        { { 0x42, 0x85, 0xF4, 0xFF }, { 0x1A, 0x73, 0xE8, 0xFF } }, // 蓝-蓝
        { { 0x34, 0xA8, 0x53, 0xFF }, { 0xFB, 0xBC, 0x04, 0xFF } }, // 绿-黄
        { { 0x9C, 0x27, 0xB0, 0xFF }, { 0xEA, 0x43, 0x35, 0xFF } }, // 紫-红
        { { 0x00, 0x96, 0x88, 0xFF }, { 0x42, 0x85, 0xF4, 0xFF } }, // 青-蓝
        { { 0xFB, 0xBC, 0x04, 0xFF }, { 0xEA, 0x43, 0x35, 0xFF } }, // 黄-红
        { { 0x5F, 0x63, 0x68, 0xFF }, { 0x42, 0x85, 0xF4, 0xFF } }, // 灰-蓝
    };
    return pal[static_cast<size_t>(seed) % pal.size()];
}

inline auto subcategories_of(const std::string &cat) -> std::vector<std::string> {
    if (cat == "apps") {
        return { "社交", "效率", "工具", "摄影", "通讯", "财务" };
    }
    if (cat == "games") {
        return { "动作", "益智", "策略", "休闲", "竞速" };
    }
    if (cat == "movies") {
        return { "新上线", "热门", "高分", "喜剧", "动作" };
    }
    if (cat == "books") {
        return { "小说", "非虚构", "漫画", "教材", "言情" };
    }
    return {};
}

inline auto default_local_catalog() -> CatalogPtr {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> name_parts = {
        { "apps",
          { "Chat", "Photo", "Note", "Maps", "Mail", "Clock", "Weather", "Wallet", "Scanner", "Browser", "Calendar",
            "Music" } },
        { "games", { "Quest", "Blast", "Puzzle", "Racer", "Empire", "Dash", "Heroes", "Galaxy", "Ninja", "Tower" } },
        { "movies", { "Horizon", "Legacy", "Shadow", "Spark", "Voyage", "Echo", "Rally", "Storm" } },
        { "books", { "Saga", "Chronicle", "Tales", "Manual", "Prose", "Atlas", "Verse", "Codex" } },
    };
    static const std::vector<std::string> devs = { "Aurora Labs", "Nimbus", "Pixel Forge", "BlueStack",
                                                   "Orbit Inc",   "Quasar", "Vertex",      "Lumen" };

    // NOLINTNEXTLINE(bugprone-random-generator-seed) 固定种子：demo 目录数据需确定性可复现
    std::mt19937 rng(20260802u);
    auto cat_list = std::vector<std::string>{ "apps", "games", "movies", "books" };
    std::vector<AppItem> items;
    int n = 0;
    for (const auto &cat : cat_list) {
        const auto &parts = name_parts[static_cast<size_t>(std::ranges::find(cat_list, cat) - cat_list.begin())].second;
        const auto subs = subcategories_of(cat);
        constexpr int per_cat = 48;
        for (int i = 0; i < per_cat; ++i) {
            AppItem a;
            const int part = static_cast<int>(rng() % parts.size());
            a.id = cat.substr(0, 1) + std::to_string(n);
            a.name = parts[part] + " " + std::to_string(1 + (i / parts.size()));
            a.developer = devs[static_cast<size_t>(rng() % devs.size())];
            a.category = cat;
            a.subcategory = subs[static_cast<size_t>(rng() % subs.size())];
            a.rating = 3.4f + (static_cast<float>(rng() % 160) / 100.0f); // 3.4..5.0
            a.rating_count = 100 + static_cast<int>(rng() % 90000);
            a.downloads = std::to_string(1 + (rng() % 500)) + "M+";
            a.size_mb = 8.0f + static_cast<float>(rng() % 240);
            a.version = "1." + std::to_string(rng() % 20) + "." + std::to_string(rng() % 10);
            a.updated = "2026-0" + std::to_string(1 + (rng() % 7)) + "-1" + std::to_string(1 + (rng() % 8));
            auto pal = palette_of(static_cast<int>(rng()));
            a.color_a = pal.first;
            a.color_b = pal.second;
            a.icon = make_app_icon(pal.first, pal.second, static_cast<int>(rng()));
            a.is_app = (cat == "apps" || cat == "games");
            a.description = "这是一款" + a.category + "类应用：" + a.name + "。它提供流畅的体验与精美的界面，由 " +
                            a.developer + " 精心打造。";
            items.push_back(std::move(a));
            ++n;
        }
    }
    return std::make_shared<const std::vector<AppItem>>(std::move(items));
}

/// @brief 真实数据层：Repository 形态与真实 API 一致，仅数据源被 hook 替换（不联网）。
class PlayRepository {
    DataHook m_hook = [](const DataRequest &) -> CatalogPtr { return default_local_catalog(); };
    mutable CatalogPtr m_catalog;
    mutable std::unordered_map<std::string, std::vector<Image>> m_shot_cache;

    void ensure() const {
        if (!m_catalog) {
            m_catalog = m_hook ? m_hook({ .endpoint = "catalog", .params = {} }) : default_local_catalog();
        }
    }

  public:
    /// @brief 注入替换数据源 hook（默认本地合成）。
    void set_data_hook(DataHook h) {
        m_hook = std::move(h);
        m_catalog.reset();
        m_shot_cache.clear();
    }

    [[nodiscard]] auto all() const -> std::vector<AppItem> {
        ensure();
        return *m_catalog;
    }
    [[nodiscard]] auto featured() const -> std::vector<AppItem> {
        auto v = all();
        std::ranges::sort(v, [](const AppItem &x, const AppItem &y) -> bool { return x.rating > y.rating; });
        if (v.size() > 10) {
            v.resize(10);
        }
        return v;
    }
    [[nodiscard]] auto list_by_category(const std::string &cat) const -> std::vector<AppItem> {
        auto v = all();
        std::vector<AppItem> out;
        for (auto &a : v) {
            if (a.category == cat) {
                out.push_back(a);
            }
        }
        return out;
    }
    [[nodiscard]] auto list_by_subcategory(const std::string &cat, const std::string &sub) const
        -> std::vector<AppItem> {
        auto v = all();
        std::vector<AppItem> out;
        for (auto &a : v) {
            if (a.category == cat && a.subcategory == sub) {
                out.push_back(a);
            }
        }
        return out;
    }
    [[nodiscard]] auto search(const std::string &q) const -> std::vector<AppItem> {
        auto v = all();
        std::vector<AppItem> out;
        std::string ql;
        ql.reserve(q.size());
        for (char c : q) {
            ql.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        for (auto &a : v) {
            std::string n;
            std::string d;
            for (char c : a.name) {
                n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            for (char c : a.developer) {
                d.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (n.find(ql) != std::string::npos || d.find(ql) != std::string::npos) {
                out.push_back(a);
            }
        }
        return out;
    }
    [[nodiscard]] auto detail(const std::string &id) const -> AppItem {
        ensure();
        for (const auto &a : *m_catalog) {
            if (a.id == id) {
                return a;
            }
        }
        return AppItem{};
    }
    // 保持实例方法：与其余查询 API 统一，消费方经实例调用
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] auto reviews(const std::string &id) const -> std::vector<Review> {
        std::vector<Review> out;
        std::mt19937 rng(std::hash<std::string>{}(id));
        static const std::vector<std::string> users = {
            "Alex", "Sam", "李雷", "韩梅", "Jordan", "Priya", "Tom", "小林"
        };
        static const std::vector<std::string> texts = { "非常好用，界面漂亮！", "有些小 bug，但整体不错。",
                                                        "必备应用，强烈推荐。", "加载偶尔慢，希望优化。",
                                                        "功能丰富，超出预期。", "设计很舒服，体验流畅。" };
        const int k = 3 + static_cast<int>(rng() % 3);
        for (int i = 0; i < k; ++i) {
            Review r;
            r.user = users[static_cast<size_t>(rng() % users.size())];
            r.rating = 3.0f + (static_cast<float>(rng() % 20) / 10.0f);
            r.text = texts[static_cast<size_t>(rng() % texts.size())];
            r.date = "2026-0" + std::to_string(1 + (rng() % 7)) + "-0" + std::to_string(1 + (rng() % 8));
            out.push_back(std::move(r));
        }
        return out;
    }
    /// @brief 懒生成并缓存截图（详情页使用）。
    [[nodiscard]] auto screenshots_for(const std::string &id) const -> std::vector<Image> {
        const auto it = m_shot_cache.find(id);
        if (it != m_shot_cache.end()) {
            return it->second;
        }
        const AppItem a = detail(id);
        std::vector<Image> shots;
        shots.reserve(4);
        for (int i = 0; i < 4; ++i) {
            shots.push_back(make_screenshot(a.color_a, a.color_b, i));
        }
        m_shot_cache[id] = shots;
        return shots;
    }
};

/// @brief 全局单例（demo / 测试使用）。
inline auto repository() -> PlayRepository & {
    static PlayRepository repo;
    return repo;
}

} // namespace gp
