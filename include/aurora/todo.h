#pragma once

#include <functional>
#include <string>

#include "aurora/core/diagnostics.h"

namespace aurora {

/**
 * @brief 占位回调：用于"部分代码容错"（需求 #23）。
 *
 * 任何需要回调的位置都可写 `au::TODO("稍后实现")`，它在编译期可转换为**任意**
 * `std::function<Signature>`（签名自动推导），运行时被调用时仅产生一条结构化警告
 * 而不崩溃。这让 AI 生成的占位代码也能直接编译运行，逐步替换。
 *
 * 例：
 * @code
 *   au::Button("OK").set_on_click(au::TODO("接线到保存逻辑"));
 *   window->setOnKey(au::TODO("处理快捷键"));
 * @endcode
 */
struct TODO {
    std::string m_what;

    explicit TODO(std::string what) : m_what(std::move(what)) {}

    /// @brief 转换为任意回调签名；调用时记录警告（不抛异常、不崩溃）。
    template<typename Signature>
    operator std::function<Signature>() const // NOLINT：故意的隐式转换
    {
        std::string desc = m_what;
        using Ret = std::function<Signature>::result_type;
        return std::function<Signature>{ [desc](auto &&...) -> Ret {
            Diagnostics::warn("TODO", desc);
            if constexpr (!std::is_void_v<Ret>) {
                return Ret{};
            } else {
                return;
            }
        } };
    }
};

} // namespace aurora
