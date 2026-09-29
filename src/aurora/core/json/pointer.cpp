// JSON Pointer（RFC 6901 最小集）：只读寻址、写路径寻址、路径删除。
//
// 最小集范围：`~1` / `~0` 反转义、对象键与数组下标段、`-` 追加记号与「自动补齐中间容器」
// （后两者服务 RFC 6902 补丁的写路径）。不含 URI fragment 形态与相对 pointer。

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "aurora/core/json.h"

namespace aurora::json {

namespace {

/// @brief 段反转义：`~1` → `/`、`~0` → `~`（RFC 6901 §3）。
[[nodiscard]] auto unescape_token(std::string_view token) -> std::string {
    std::string out;
    out.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '0') {
                out.push_back('~');
                ++i;
                continue;
            }
            if (token[i + 1] == '1') {
                out.push_back('/');
                ++i;
                continue;
            }
        }
        out.push_back(token[i]);
    }
    return out;
}

/// @brief 取 pointer 的下一段原始文本（**未反转义**）。
/// @param pos 入口指向一个 `/`；出口指向下一段的 `/` 或 `pointer.size()`。
/// @return false = 已无段可取。
[[nodiscard]] auto next_token(std::string_view pointer, std::size_t &pos, std::string_view &token) noexcept -> bool {
    if (pos >= pointer.size() || pointer[pos] != '/') {
        return false;
    }
    const std::size_t start = pos + 1;
    const std::size_t slash = pointer.find('/', start);
    if (slash == std::string_view::npos) {
        token = pointer.substr(start);
        pos = pointer.size();
    } else {
        token = pointer.substr(start, slash - start);
        pos = slash;
    }
    return true;
}

/// @brief 段是否为合法数组下标（十进制数字；允许前导零，以对齐既有步进实现的行为）。
[[nodiscard]] auto parse_index(std::string_view token, std::size_t &out) noexcept -> bool {
    if (token.empty()) {
        return false;
    }
    const char *first = token.data();
    const char *last = token.end();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

/// @brief 段是否形如数组下标（含 `-` 追加记号），用于决定占位 null 展开成 Array 还是 Object。
[[nodiscard]] auto looks_like_index(std::string_view token) noexcept -> bool {
    std::size_t ignored = 0;
    return token == "-" || parse_index(token, ignored);
}

[[nodiscard]] auto step(const Value &value, std::string_view token) -> const Value * {
    if (value.is_object()) {
        return value.find(unescape_token(token));
    }
    if (value.is_array()) {
        std::size_t index = 0;
        if (!parse_index(token, index)) {
            return nullptr;
        }
        return value.at(index);
    }
    return nullptr;
}

[[nodiscard]] auto step(Value &value, std::string_view token) -> Value * {
    if (value.is_object()) {
        return value.find(unescape_token(token));
    }
    if (value.is_array()) {
        std::size_t index = 0;
        if (!parse_index(token, index)) {
            return nullptr;
        }
        return value.at(index);
    }
    return nullptr;
}

/// @brief pointer 语法非法（非空且不以 `/` 开头）。
[[nodiscard]] auto syntax_error(std::string_view pointer) -> Error {
    return make_error(ErrorCode::JsonParseError, "invalid JSON Pointer '" + std::string(pointer) + "'", ErrorParams{},
                      std::string("A non-empty JSON Pointer must start with '/' (RFC 6901)."));
}

/// @brief 段无法应用于当前值（对非容器取子项 / 对数组用非数字段 / 索引越界）。
[[nodiscard]] auto segment_error(std::string_view pointer, std::string_view detail) -> Error {
    return make_error(ErrorCode::JsonTypeMismatch,
                      "JSON Pointer '" + std::string(pointer) + "' cannot address a value: " + std::string(detail),
                      ErrorParams{}, std::string("Check the targeted value's type and the array index in range."));
}

}  // namespace

auto find_pointer(const Value &root, std::string_view pointer) -> const Value * {
    const Value *cur = &root;
    if (pointer.empty()) {
        return cur;  // 空 pointer 指向文档自身
    }
    if (pointer[0] != '/') {
        return nullptr;
    }
    std::size_t pos = 0;
    std::string_view token;
    while (next_token(pointer, pos, token)) {
        cur = step(*cur, token);
        if (cur == nullptr) {
            return nullptr;
        }
    }
    return cur;
}

auto find_pointer(Value &root, std::string_view pointer) -> Value * {
    Value *cur = &root;
    if (pointer.empty()) {
        return cur;
    }
    if (pointer[0] != '/') {
        return nullptr;
    }
    std::size_t pos = 0;
    std::string_view token;
    while (next_token(pointer, pos, token)) {
        cur = step(*cur, token);
        if (cur == nullptr) {
            return nullptr;
        }
    }
    return cur;
}

auto resolve_for_write(Value &root, std::string_view pointer) -> Result<Value *> {
    if (pointer.empty()) {
        return &root;
    }
    if (pointer[0] != '/') {
        return syntax_error(pointer);
    }
    Value *cur = &root;
    std::size_t pos = 0;
    std::string_view token;
    while (next_token(pointer, pos, token)) {
        const bool is_last = pos >= pointer.size();

        // 占位 null 按本段形态展开：段像下标（含 `-`）→ Array，否则 Object。
        // 这样 `"/a/0"` 会在补 a 时直接建数组，与补丁语义一致。
        if (cur->is_null()) {
            *cur = looks_like_index(token) ? Value::array() : Value::object();
        }

        if (cur->is_object()) {
            const std::string key = unescape_token(token);
            Value *slot = cur->find(key);
            if (slot == nullptr) {
                cur->set(key, Value(nullptr));
                slot = cur->find(key);
            }
            if (is_last) {
                return slot;
            }
            cur = slot;
            continue;
        }

        if (cur->is_array()) {
            if (token == "-") {  // RFC 6901 追加记号：只能作末段
                if (!is_last) {
                    return segment_error(pointer, "'-' may only appear as the final segment");
                }
                cur->push_back(Value(nullptr));
                return cur->at(cur->size() - 1);
            }
            std::size_t index = 0;
            if (!parse_index(token, index)) {
                return segment_error(pointer, "array segment is not a valid index");
            }
            if (index > cur->size()) {
                return segment_error(pointer, "array index out of range");
            }
            if (index == cur->size()) {
                if (!is_last) {
                    return segment_error(pointer, "an append position may only be the final segment");
                }
                cur->push_back(Value(nullptr));
                return cur->at(index);
            }
            Value *slot = cur->at(index);
            if (is_last) {
                return slot;
            }
            cur = slot;
            continue;
        }

        return segment_error(pointer, "cannot descend into a scalar value");
    }
    return &root;
}

auto erase_pointer(Value &root, std::string_view pointer) -> Result<bool> {
    if (pointer.empty()) {
        return make_error(ErrorCode::JsonParseError, std::string("cannot erase the JSON document root"), ErrorParams{},
                          std::string("A pointer that selects the root has no member to remove."));
    }
    if (pointer[0] != '/') {
        return syntax_error(pointer);
    }

    // 先收集全部段（原始文本），再走前 n-1 段定位父容器。
    std::vector<std::string> tokens;
    {
        std::size_t pos = 0;
        std::string_view token;
        while (next_token(pointer, pos, token)) {
            tokens.emplace_back(token);
        }
    }

    Value *parent = &root;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        parent = step(*parent, tokens[i]);
        if (parent == nullptr) {
            return false;  // 路径不存在：未命中，不是错误
        }
    }

    const std::string_view last = tokens.back();
    if (parent->is_object()) {
        return parent->erase(unescape_token(last));
    }
    if (parent->is_array()) {
        std::size_t index = 0;
        if (!parse_index(last, index)) {
            return segment_error(pointer, "array segment is not a valid index");
        }
        return parent->erase_at(index);
    }
    return segment_error(pointer, "cannot erase a member from a scalar value");
}

}  // namespace aurora::json
