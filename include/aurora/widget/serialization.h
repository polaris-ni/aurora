#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/widget/button.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"
#include "aurora/widget/yaml.h"

namespace aurora {

/**
 * @brief widget 树 ⇄ JSON 序列化 + JSON Patch 差分（需求 #13）。
 *
 * 设计要点：
 * - 每个 widget 通过 `serializeProps`/`deserializeProps`（虚函数）暴露自有属性，
 *   结构快照统一由 `toJson`/`fromJson` 驱动；新增 widget 只需覆写这两个方法并到
 *   `WidgetRegistry` 注册工厂即可被工具链消费（Inspector / 代码生成 / diff）。
 * - `diff(a, b)` 产出一组 RFC6902 风格的 `JsonPatchOp`，`apply` 可把补丁应用到 a 上
 *   得到 b，用于「实时编辑 → 定点刷新」「UI 描述版本差异」等场景。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
namespace serialization {

/// @brief 单个 JSON Patch 操作（"replace" / "add" / "remove"），path 为 JSON pointer。
struct JsonPatchOp {
    std::string op;  ///< "replace" | "add" | "remove"
    std::string path;  ///< JSON pointer，如 "/children/0/props/show"
    Json value;  ///< 操作值（remove 时为空）
};

/// @brief 把 widget 子树序列化为 JSON 结构快照。
[[nodiscard]] auto to_json(const Widget &w) -> Json;

/// @brief widget 工厂：从 props JSON 构造对应类型的空属性 widget。
using WidgetFactory = std::function<Result<std::shared_ptr<Widget>>(const Json &props)>;

/// @brief 类型名 → 工厂 的注册表（工具链据此把 JSON 反序列化为真实 widget）。
class WidgetRegistry {
  public:
    [[nodiscard]] static auto instance() -> WidgetRegistry & {
        static WidgetRegistry reg;
        return reg;
    }

    auto register_factory(const std::string &type, WidgetFactory fn) -> void { factories_[type] = std::move(fn); }

    [[nodiscard]] auto make(const std::string &type, const Json &props) const -> Result<std::shared_ptr<Widget>>;

    /// @brief 列出所有已注册 widget 类型名（工具链/API 生成器反射用）。
    [[nodiscard]] auto list_types() const -> std::vector<std::string>;

  private:
    std::map<std::string, WidgetFactory> factories_;
};

/// @brief 注册核心 widget 工厂（Text/Button/Column/Row）。幂等，可重复调用。
auto register_core_widgets() -> void;

/// @brief 从 JSON 结构快照重建 widget 子树。未知类型或结构非法返回 Error。
[[nodiscard]] auto from_json(const Json &j) -> Result<std::shared_ptr<Widget>>;

/// @brief 递归比较两个 JSON，产出把 a 变为 b 的补丁操作列表（追加到 out）。
auto diff_into(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void;

/// @brief 计算把 a 变为 b 的 JSON Patch（RFC6902 风格子集）。
[[nodiscard]] auto diff(const Json &a, const Json &b) -> std::vector<JsonPatchOp>;

/// @brief 把补丁应用到 target 上（原地修改）。remove 失败静默忽略（幂等）。
/// 命名为 applyPatch 以避免与 std::apply 经 ADL 冲突。
auto apply_patch(Json &target, const std::vector<JsonPatchOp> &patch) -> void;

/// @brief 生成单个组件的 schema（类型/容器性/属性键/线程约束），供 `gen_api` 与反射 API 复用。
/// 等价于 `aurora_api.json` 中某个 widget 条目的结构。需先 `register_core_widgets()`。
[[nodiscard]] auto component_schema(const std::string &name) -> Json;

/// Serialize widget tree to YAML format (output-only, no from_yaml).
/// Delegates to to_json(w) then yaml.h's to_yaml(Json).
[[nodiscard]] auto to_yaml(const Widget &w) -> std::string;

}  // namespace serialization

/// @brief 列出所有已注册组件类型名（反射，specification/08-tooling.md §2.3）。
[[nodiscard]] auto list_all_components() -> std::vector<std::string>;

/// @brief 返回单个组件的 schema（含 props/children/thread）；未知类型返回空 Json 对象。
[[nodiscard]] auto describe_component(const std::string &name) -> Json;

/// @brief 按名称子串（大小写不敏感）搜索组件，返回匹配组件的 schema 列表。
[[nodiscard]] auto search_components(const std::string &query) -> std::vector<Json>;

/// @brief 返回所有已注册组件的完整 schema（含 describe 元数据）。
[[nodiscard]] auto list_all_schemas() -> std::vector<Json>;

}  // namespace aurora
