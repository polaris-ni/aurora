#include "aurora/i18n/message_catalog.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "aurora/core/log.h"
#include "aurora/i18n/builtin_messages.gen.h"
#include "aurora/i18n/detail/message_engine.h"

namespace aurora {
namespace {

constexpr std::uint32_t AURORA_BIN_POOL_MISSING = 0xFFFFFFFFU;  // 缺译哨兵（与 AURORA_INVALID_MESSAGE_ID_VALUE 同值）

/// @brief 从字节流读 little-endian u16/u32（现役目标全 LE；非 LE 输入经 version 校验拦截）。
/// @param b 至少 2（u16）/ 4（u32）字节的窗口；越界判定由调用侧的 need() 先行完成。
/// @return 按 LE 拼出的无符号值。
auto read_u16(std::span<const std::byte> b) -> std::uint32_t {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[1])) << 8U);
}

auto read_u32(std::span<const std::byte> b) -> std::uint32_t {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[3])) << 24U);
}

/// @brief 词表区间（`[base, end)` + 规范名表）。
struct VocabBlock {
    std::uint32_t base = 0;
    std::uint32_t end = 0;
    std::vector<std::string> names;
};

/// @brief 单条词条：模板原文 + 惰性解析缓存（resolve 为 const，缓存字段 mutable）。
/// 拷贝语义 = 模板深拷贝、AST 缓存丢弃（目录快照的载体，按需重解析）。
struct CatalogEntry {
    std::string tmpl;
    mutable std::unique_ptr<detail::MessageNode> ast;
    mutable bool parsed = false;
    CatalogEntry() = default;
    CatalogEntry(const CatalogEntry &other) : tmpl(other.tmpl) {}
    auto operator=(const CatalogEntry &other) -> CatalogEntry & {
        if (this != &other) {
            tmpl = other.tmpl;
            ast.reset();
            parsed = false;
        }
        return *this;
    }
    // 显式写出移动两件（缺省实现即转移模板与 AST 缓存）：声明了拷贝构造就得把移动也说明白，
    // 否则隐式移动被抑制、载入路径的每条模板都退化成深拷贝。析构同理由 `= default` 补白，
    // 五件套齐列（special-member-functions 口径），成员本身仍是零手写。
    CatalogEntry(CatalogEntry &&) noexcept = default;
    auto operator=(CatalogEntry &&) noexcept -> CatalogEntry & = default;
    ~CatalogEntry() = default;
};

/// @brief 单 locale 词条存储（按全局 Id 直排；区间外槽位为 nullopt）。
struct LocaleStore {
    std::vector<std::optional<CatalogEntry>> entries;
};

}  // namespace

/// @brief 目录实现：词表 / 名字索引 / locale 存储 / 物化视图。
struct MessageCatalog::Impl {
    std::vector<VocabBlock> vocab_blocks;  ///< 已登记词表区间（数量级为个位数，线性查找足够）
    std::unordered_map<std::string, MessageId> name_to_id;  ///< 规范名 → Id
    std::unordered_map<std::string, LocaleStore> stores;  ///< locale tag → 存储
    Locale default_locale{};  ///< 回退链末端
    std::unordered_map<std::string, std::vector<const CatalogEntry *>> views;  ///< 物化视图（tag → Id 直排指针）

    /// @brief 词表全域上界（各区间 end 的最大值；空目录为 0）。
    [[nodiscard]] auto max_id() const -> std::uint32_t {
        std::uint32_t top = 0;
        for (const VocabBlock &block : vocab_blocks) {
            top = std::max(top, block.end);
        }
        return top;
    }

    /// @brief 取（或建）某 locale 的存储，并保证尺寸覆盖全域。
    auto store_for(const std::string &tag) -> LocaleStore & {
        LocaleStore &store = stores[tag];
        if (store.entries.size() < max_id()) {
            store.entries.resize(max_id());
        }
        return store;
    }

    /// @brief 找 Id 所属词表（区间包含；未命中 nullptr）。
    [[nodiscard]] auto vocab_of(MessageId id) const -> const VocabBlock * {
        for (const VocabBlock &block : vocab_blocks) {
            if (id.value >= block.base && id.value < block.end) {
                return &block;
            }
        }
        return nullptr;
    }

    /// @brief 结构变更后丢弃全部物化视图（惰性重建）。
    auto invalidate() -> void { views.clear(); }
};

namespace {

/// @brief 解析请求 locale 的回退阶梯（先丢 region 后丢 script，缺子标签的档自动折叠去重）。
auto fallback_tags(const Locale &loc, const Locale &default_locale) -> std::vector<std::string> {
    std::vector<std::string> tags;
    auto push = [&tags](const std::string &tag) {
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
    };
    const Locale lang_script{.language = loc.language, .script = loc.script};
    const Locale lang_region{.language = loc.language, .region = loc.region};
    const Locale lang_only{.language = loc.language};
    push(loc.tag());
    push(lang_script.tag());
    push(lang_region.tag());
    push(lang_only.tag());
    if (default_locale.language != loc.language || default_locale.script != loc.script ||
        default_locale.region != loc.region) {
        const Locale default_lang{.language = default_locale.language};
        push(default_locale.tag());
        push(default_lang.tag());
    }
    return tags;
}

/// @brief 同基址词表的重载兼容判定：既有名单须为新名单的前缀（词表只增不减）。
///
/// 空名是保留区空洞占位（生成词表把内置区与应用区并入同一 Id 区间时的未分配槽），
/// 前缀比对按串逐位相等，故空洞位置也必须逐位一致。
auto vocab_names_prefix_compatible(const std::vector<std::string> &existing, const std::vector<std::string> &next)
    -> bool {
    return existing.size() <= next.size() && std::equal(existing.begin(), existing.end(), next.begin());
}

/// @brief 把词条模板过一遍引擎解析（惰性，失败保留原文回退口径）。
auto ensure_parsed(const CatalogEntry &entry) -> const detail::MessageNode * {
    if (!entry.parsed) {
        entry.ast = detail::parse_message_template(entry.tmpl);
        entry.parsed = true;
    }
    return entry.ast.get();
}

/// @brief 聚合输出一条缺译 WARN（明细走 missing_translations 查询，不逐条刷屏）。
auto warn_missing(const MessageCatalog &catalog, const Locale &loc) -> void {
    const auto missing = catalog.missing_translations(loc);
    if (!missing.empty()) {
        AURORA_LOG_WARN("i18n", missing.size(), " message(s) missing translations for locale '", loc.tag(), "'");
    }
}

}  // namespace

MessageCatalog::MessageCatalog() : impl_(std::make_unique<Impl>()) {}

MessageCatalog::~MessageCatalog() = default;

MessageCatalog::MessageCatalog(const MessageCatalog &other) : impl_(std::make_unique<Impl>(*other.impl_)) {
    impl_->views.clear();  // 视图指针指向旧目录条目，快照一律惰性重建
}

auto MessageCatalog::operator=(const MessageCatalog &other) -> MessageCatalog & {
    if (this != &other) {
        *impl_ = *other.impl_;
        impl_->views.clear();
    }
    return *this;
}

MessageCatalog::MessageCatalog(MessageCatalog &&) noexcept = default;

auto MessageCatalog::operator=(MessageCatalog &&) noexcept -> MessageCatalog & = default;

auto MessageCatalog::register_vocabulary(std::uint32_t id_base, std::vector<std::string> names) -> Result<void> {
    if (names.empty()) {
        return Result<void>{};
    }
    const auto count = static_cast<std::uint32_t>(names.size());
    if (id_base > AURORA_INVALID_MESSAGE_ID_VALUE || count > AURORA_INVALID_MESSAGE_ID_VALUE - id_base) {
        return make_error(ErrorCode::I18nIdOutOfRange,
                          {{"base", std::to_string(id_base)}, {"end", std::to_string(id_base + count)}});
    }
    const std::uint32_t end = id_base + count;
    VocabBlock *same_base = nullptr;
    for (VocabBlock &block : impl_->vocab_blocks) {
        if (block.base == id_base) {
            same_base = &block;  // 同词表重载：登记须可重复，否则「后到覆盖」无从发生
            continue;
        }
        if (id_base < block.end && block.base < end) {
            return make_error(ErrorCode::I18nVocabularyOverlap,
                              {{"base", std::to_string(id_base)}, {"end", std::to_string(end)}});
        }
    }
    // 同基址重载要求既有名单是新名单的前缀（词表只增不减；缩减按冲突拒绝）。
    // 空名 = 保留区空洞占位（生成词表把内置区与应用区并入同一区间时的未分配槽），不进名字索引。
    if (same_base != nullptr && !vocab_names_prefix_compatible(same_base->names, names)) {
        return make_error(ErrorCode::I18nVocabularyOverlap,
                          {{"base", std::to_string(id_base)}, {"end", std::to_string(end)}});
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        if (names[k].empty()) {
            continue;
        }
        const auto existing = impl_->name_to_id.find(names[k]);
        if (existing != impl_->name_to_id.end() && existing->second.value != id_base + k) {
            return make_error(ErrorCode::I18nNameDuplicate, {{"name", names[k]}});
        }
    }
    // 提交：既有同基址块就地扩列，新区间追加块；两处都补齐 name → Id 映射。
    if (same_base != nullptr) {
        for (auto k = static_cast<std::uint32_t>(same_base->names.size()); k < count; ++k) {
            if (!names[k].empty()) {
                impl_->name_to_id.emplace(names[k], MessageId{id_base + k});
            }
        }
        same_base->names = std::move(names);
        same_base->end = end;
    } else {
        VocabBlock block;
        block.base = id_base;
        block.end = end;
        block.names = std::move(names);
        for (std::uint32_t k = 0; k < count; ++k) {
            if (!block.names[k].empty()) {
                impl_->name_to_id.emplace(block.names[k], MessageId{id_base + k});
            }
        }
        impl_->vocab_blocks.push_back(std::move(block));
    }
    for (auto &[tag, store] : impl_->stores) {
        if (store.entries.size() < impl_->max_id()) {
            store.entries.resize(impl_->max_id());
        }
        (void)tag;
    }
    impl_->invalidate();
    return Result<void>{};
}

auto MessageCatalog::load_binary(std::span<const std::byte> bytes) -> Result<void> {
    constexpr std::size_t header_size = 20;  // magic4 + version4 + id_base4 + id_count4 + locale_count4
    const std::size_t size = bytes.size();
    if (size < header_size) {
        return make_error(ErrorCode::I18nBinTruncated,
                          {{"offset", "0"}, {"needed", std::to_string(header_size - size)}});
    }
    if (std::memcmp(bytes.data(), "AUM1", 4) != 0) {
        return make_error(ErrorCode::I18nBinMagicFailed);
    }
    // 全函数唯一的字节→字符重解释：AUM1 的规范名表与模板池存的都是 UTF-8 文本，这里建立的只是
    // 只读视图（不回写、不延长生命周期——bytes 由调用方在整个调用期内持有）。std::byte 与 char
    // 之间没有静态转换路径，`std::as_string_view` 一类桥接件尚未进标准库。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char *>(bytes.data()), size};
    const std::uint32_t version = read_u32(bytes.subspan(4, 4));
    if (version != 1) {
        // 字节序自检：LE 解读非 1 而字节反序恰为 1 → 大端词表；否则按版本不支持处理。
        const std::uint32_t swapped =
            (version >> 24U) | (((version >> 8U) & 0xFFU) << 8U) | ((version & 0xFFU) << 8U) | (version << 24U);
        if (swapped == 1) {
            return make_error(ErrorCode::I18nBinEndiannessInvalid, {{"version", std::to_string(version)}});
        }
        return make_error(ErrorCode::I18nBinVersionUnsupported, {{"version", std::to_string(version)}});
    }
    const std::uint32_t id_base = read_u32(bytes.subspan(8, 4));
    const std::uint32_t id_count = read_u32(bytes.subspan(12, 4));
    const std::uint32_t locale_count = read_u32(bytes.subspan(16, 4));

    // 第一遍：全量解析到临时结构（任何结构化失败都不落库，保证载入原子性）。
    std::size_t cursor = header_size;
    auto need = [&](std::size_t extra) -> Result<void> {
        if (cursor + extra > size) {
            return make_error(ErrorCode::I18nBinTruncated,
                              {{"offset", std::to_string(cursor)}, {"needed", std::to_string(cursor + extra - size)}});
        }
        return Result<void>{};
    };
    std::vector<std::string> names;
    names.reserve(id_count);
    for (std::uint32_t k = 0; k < id_count; ++k) {
        if (auto check = need(2); !check) {
            return check;
        }
        const std::uint32_t len = read_u16(bytes.subspan(cursor, 2));
        cursor += 2;
        if (auto check = need(len); !check) {
            return check;
        }
        names.emplace_back(text.substr(cursor, len));
        cursor += len;
    }
    struct LocaleBlob {
        std::string tag;
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint32_t> lengths;
    };
    std::vector<LocaleBlob> locales;
    locales.reserve(locale_count);
    for (std::uint32_t l = 0; l < locale_count; ++l) {
        if (auto check = need(1); !check) {
            return check;
        }
        const auto tag_len = std::to_integer<std::uint32_t>(bytes[cursor]);
        cursor += 1;
        if (auto check = need(tag_len + (static_cast<std::size_t>(id_count) * 8U)); !check) {
            return check;
        }
        LocaleBlob blob;
        blob.tag.assign(text.substr(cursor, tag_len));
        cursor += tag_len;
        blob.offsets.reserve(id_count);
        blob.lengths.reserve(id_count);
        for (std::uint32_t k = 0; k < id_count; ++k) {
            blob.offsets.push_back(read_u32(bytes.subspan(cursor, 4)));
            blob.lengths.push_back(read_u32(bytes.subspan(cursor + 4, 4)));
            cursor += 8;
        }
        locales.push_back(std::move(blob));
    }
    const std::size_t pool_start = cursor;
    const std::size_t pool_size = size - pool_start;
    for (const LocaleBlob &blob : locales) {
        for (std::uint32_t k = 0; k < id_count; ++k) {
            if (blob.offsets[k] == AURORA_BIN_POOL_MISSING) {
                continue;
            }
            if (static_cast<std::size_t>(blob.offsets[k]) + blob.lengths[k] > pool_size) {
                return make_error(ErrorCode::I18nBinTruncated,
                                  {{"offset", std::to_string(pool_start + blob.offsets[k])},
                                   {"needed", std::to_string(static_cast<std::size_t>(blob.offsets[k]) +
                                                             blob.lengths[k] - pool_size)}});
            }
        }
    }

    // 第二遍：提交（词表登记的区间/名字校验失败时目录保持原状）。
    if (auto registered = register_vocabulary(id_base, names); !registered) {
        return registered;
    }
    for (const LocaleBlob &blob : locales) {
        LocaleStore &store = impl_->store_for(blob.tag);
        for (std::uint32_t k = 0; k < id_count; ++k) {
            if (blob.offsets[k] == AURORA_BIN_POOL_MISSING) {
                continue;  // 缺译槽不动（保留该 locale 已有值，覆盖语义只作用于实际携带的词条）
            }
            CatalogEntry entry;
            entry.tmpl.assign(text.substr(pool_start + blob.offsets[k], blob.lengths[k]));
            store.entries.at(id_base + k) = std::move(entry);
        }
    }
    impl_->invalidate();
    // 缺省 locale 已设时，对它做一次缺译聚合提示（明细走 missing_translations 查询）。
    warn_missing(*this, impl_->default_locale);
    return Result<void>{};
}

auto MessageCatalog::add(const Locale &loc, std::string_view canonical_name, std::string tmpl) -> Result<void> {
    const auto found = impl_->name_to_id.find(std::string(canonical_name));
    if (found == impl_->name_to_id.end()) {
        return make_error(ErrorCode::I18nUnknownName, {{"name", std::string(canonical_name)}});
    }
    CatalogEntry entry;
    entry.tmpl = std::move(tmpl);
    impl_->store_for(loc.tag()).entries.at(found->second.value) = std::move(entry);
    impl_->invalidate();
    return Result<void>{};
}

auto MessageCatalog::set_default_locale(const Locale &loc) -> void {
    impl_->default_locale = loc;
    impl_->invalidate();
    warn_missing(*this, loc);
}

auto MessageCatalog::resolve(const LocalizedString &ls, const Locale &loc) const -> std::string {
    if (!ls.localize || ls.id == AURORA_INVALID_MESSAGE_ID) {
        return ls.text;
    }
    const std::string tag = loc.tag();
    auto view_iter = impl_->views.find(tag);
    if (view_iter == impl_->views.end()) {
        std::vector<const CatalogEntry *> view(impl_->max_id(), nullptr);
        for (const std::string &tier : fallback_tags(loc, impl_->default_locale)) {
            const auto store_iter = impl_->stores.find(tier);
            if (store_iter == impl_->stores.end()) {
                continue;
            }
            const std::size_t common = std::min(store_iter->second.entries.size(), view.size());
            for (std::size_t k = 0; k < common; ++k) {
                if (view[k] != nullptr) {
                    continue;  // 已有更靠前档命中
                }
                // 把槽位绑成局部引用再判空：守卫与解引用跨在同一个复合条件里时，分析器看不出
                // 「has_value 先于 `*`」（Emscripten 的 libc++ optional 尤其如此）。
                const std::optional<CatalogEntry> &slot = store_iter->second.entries[k];
                if (slot.has_value()) {
                    view[k] = &*slot;  // 条目指针（模板 + AST 缓存同体）
                }
            }
        }
        view_iter = impl_->views.emplace(tag, std::move(view)).first;
    }
    const std::vector<const CatalogEntry *> &view = view_iter->second;
    if (ls.id.value >= view.size() || view[ls.id.value] == nullptr) {
        return ls.text;  // 回退链穷尽 → 字面回退
    }
    // 视图指针在 invalidate（结构变更）时整体丢弃，条目地址只在视图有效期内被使用，口径自洽。
    const CatalogEntry *entry = view[ls.id.value];
    const detail::MessageNode *ast = ensure_parsed(*entry);
    if (ast == nullptr) {
        return entry->tmpl;  // 解析失败回退原文
    }
    return detail::format_message_node(*ast, ls.args, loc);
}

auto MessageCatalog::canonical_name(MessageId id) const -> std::string_view {
    const VocabBlock *block = impl_->vocab_of(id);
    if (block == nullptr) {
        return {};
    }
    return block->names[id.value - block->base];
}

auto MessageCatalog::find_by_name(std::string_view canonical_name) const -> MessageId {
    const auto found = impl_->name_to_id.find(std::string(canonical_name));
    return found != impl_->name_to_id.end() ? found->second : AURORA_INVALID_MESSAGE_ID;
}

auto MessageCatalog::missing_translations(const Locale &loc) const -> std::vector<MessageId> {
    const auto own_iter = impl_->stores.find(loc.tag());
    std::vector<MessageId> missing;
    const std::size_t total = impl_->max_id();
    for (std::size_t k = 0; k < total; ++k) {
        const bool own_has = own_iter != impl_->stores.end() && own_iter->second.entries.size() > k &&
                             own_iter->second.entries[k].has_value();
        if (own_has) {
            continue;
        }
        for (const auto &[tag, store] : impl_->stores) {
            if (tag == loc.tag() || store.entries.size() <= k || !store.entries[k].has_value()) {
                continue;
            }
            missing.push_back(MessageId{static_cast<std::uint32_t>(k)});
            break;
        }
    }
    return missing;
}

auto default_catalog() -> MessageCatalog & {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)：wasm 口径命中，native 遍同一份代码不报
    static MessageCatalog instance = [] {
        MessageCatalog catalog;
        std::vector<std::string> names;
        names.reserve(detail::AURORA_BUILTIN_MESSAGE_NAMES.size());
        for (std::string_view name : detail::AURORA_BUILTIN_MESSAGE_NAMES) {
            names.emplace_back(name);
        }
        if (!catalog.register_vocabulary(detail::AURORA_BUILTIN_MESSAGE_ID_BASE, std::move(names))) {
            // 内置词表是静态数据，登记失败只可能是构建产物损坏——按可观测口径 ERROR，不中止进程。
            AURORA_LOG_ERROR("i18n", "builtin vocabulary registration failed");
        }
        return catalog;
    }();
    return instance;
}

}  // namespace aurora
