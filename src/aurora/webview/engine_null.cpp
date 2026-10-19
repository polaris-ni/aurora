// webview/engine_null.cpp — Null 引擎（确定性桩）。
//
// 存在意义：让「无头 / WASM / 未开宏构建」这三类环境有一条**显式的**降级路径，而不是
// 「忘了装引擎」这种说不清的状态。它不加载任何库、不创建任何原生视图，`probe()` 恒
// `Disabled`，`create_view()` 恒返回 nullptr——因此走它的路径必然落到占位矩形。

#include <memory>

#include "aurora/webview/native_view_host.h"

namespace aurora::webview {

namespace {

class NullEngine final : public NativeViewEngine {
  public:
    /// @brief 引擎种类：Null。
    /// @return `WebViewEngine::Null`。
    [[nodiscard]] auto id() const -> WebViewEngine override { return WebViewEngine::Null; }

    /// @brief 能力探测：两种模式皆不可用，裁决为 `Disabled`。
    /// @return 全 false 且 `availability == Disabled` 的快照。
    [[nodiscard]] auto probe() const -> WebViewCapabilities override {
        return WebViewCapabilities{.interactive = false,
                                   .snapshot = false,
                                   .engine = WebViewEngine::Null,
                                   .availability = WebViewAvailability::Disabled};
    }

    /// @brief 创建视图：Null 引擎无原生视图可给。
    /// @param parent_window 未使用。
    /// @return 恒为空指针。
    [[nodiscard]] auto create_view(void * /*parent_window*/) -> std::unique_ptr<WebViewSink> override {
        return nullptr;
    }
};

}  // namespace

auto install_null_engine() -> void { install_engine(std::make_shared<NullEngine>()); }

}  // namespace aurora::webview
