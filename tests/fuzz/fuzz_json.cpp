// tests/fuzz/fuzz_json.cpp — JSON DOM 解析、回写与 SAX 出口的覆盖引导模糊测试目标。
//
// 被测面：json::parse / dump 往返，以及 parse_sax 的独立事件出口；输入为任意字节串。
// 只在 AURORA_BUILD_FUZZ=ON 的独立 clang 构建里编译，不进产品库、不进 CTest。
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/core/json.h"

namespace {

class CountingHandler final : public aurora::json::SaxHandler {
  public:
    auto on_null() -> bool override { return record(); }
    auto on_bool(bool) -> bool override { return record(); }
    auto on_int(std::int64_t) -> bool override { return record(); }
    auto on_uint(std::uint64_t) -> bool override { return record(); }
    auto on_double(double) -> bool override { return record(); }
    auto on_raw_number(std::string_view) -> bool override { return record(); }
    auto on_string(std::string_view) -> bool override { return record(); }
    auto on_array_start() -> bool override { return record(); }
    auto on_array_end(std::size_t) -> bool override { return record(); }
    auto on_object_start() -> bool override { return record(); }
    auto on_object_key(std::string_view) -> bool override { return record(); }
    auto on_object_end(std::size_t) -> bool override { return record(); }

    [[nodiscard]] auto count() const -> std::size_t { return count_; }

  private:
    auto record() -> bool {
        ++count_;
        return true;
    }

    std::size_t count_ = 0;
};

}  // namespace

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const std::string_view payload{reinterpret_cast<const char *>(data), size};

    const auto parsed = aurora::json::parse(payload);
    if (parsed) {
        const auto text = aurora::json::dump(parsed.value());
        volatile std::size_t sink = text ? text.value().size() : std::size_t{0};
        (void)sink;
    } else {
        volatile std::size_t sink = parsed.error().message.size();
        (void)sink;
    }

    // SAX 与 DOM 共用字符级引擎，但出口不同；单独覆盖一遍。
    CountingHandler handler;
    const auto sax = aurora::json::parse_sax(payload, handler);
    volatile std::size_t sink2 = handler.count() + (sax ? std::size_t{0} : sax.error().message.size());
    (void)sink2;
    return 0;
}
