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

/// @brief widget 树 ⇄ JSON 序列化 + JSON Patch 差分（需求 SPEC.FEAT.TOOLING.UI-SERIALIZATION.001）。
///
/// 设计要点：
/// - 每个 widget 通过 `serializeProps`/`deserializeProps`（虚函数）暴露自有属性，
/// 结构快照统一由 `toJson`/`fromJson` 驱动；新增 widget 只需覆写这两个方法并到
/// `WidgetRegistry` 注册工厂即可被工具链消费（Inspector / 代码生成 / diff）。
/// - `diff(a, b)` 产出一组 RFC6902 风格的 `JsonPatchOp`，`apply` 可把补丁应用到 a 上
/// 得到 b，用于「实时编辑 → 定点刷新」「UI 描述版本差异」等场景。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
namespace serialization {

/// @brief 单个 JSON Patch 操作（"replace" / "add" / "remove"），path 为 JSON pointer。
struct JsonPatchOp {
    std::string op;  ///< "replace" | "add" | "remove"
    std::string path;  ///< JSON pointer，如 "/children/0/props/show"
    Json value;  ///< 操作值（remove 时为空）
};

/// @brief 把 widget 子树序列化为 JSON 结构快照。
/// @param w 子树根 widget。
/// @return 结构快照：`type`/`props`/`children` 递归对象（属性经各 widget 的 `serializeProps`）。
[[nodiscard]] auto to_json(const Widget &w) -> Json;

/// @brief widget 工厂：从 props JSON 构造对应类型的空属性 widget。
using WidgetFactory = std::function<Result<std::shared_ptr<Widget>>(const Json &props)>;

/// @brief 类型名 → 工厂 的注册表（工具链据此把 JSON 反序列化为真实 widget）。
class WidgetRegistry {
  public:
    /// @brief 进程级注册表单例（首次调用时惰性构造）。
    /// @return 全局唯一 `WidgetRegistry` 的可变引用。
    [[nodiscard]] static auto instance() -> WidgetRegistry & {
        // 惰性构造的函数内 static：注册表由 `register_factory` 在运行期填充，本就不能常量初始化，
        // 且首建时刻与跨 TU 静态初始化顺序无关（本检查的担心面在此不存在）。仅浏览器口径命中
        // ——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
        // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
        static WidgetRegistry reg;
        return reg;
    }

    /// @brief 注册一个 widget 工厂（同名重复注册以最后一次为准）。
    /// @param type widget 类型名（JSON `type` 字段的取值）。
    /// @param fn 工厂函数：从 props JSON 构造空属性 widget。
    auto register_factory(const std::string &type, WidgetFactory fn) -> void { factories_[type] = std::move(fn); }

    /// @brief 按类型名经已注册工厂构造 widget（未注册类型返回 Error）。
    /// @param type widget 类型名。
    /// @param props 属性 JSON（原样传给工厂，由工厂回填 widget 属性）。
    /// @return 成功为构造出的 widget；类型未注册为错误 `Result`。
    [[nodiscard]] auto make(const std::string &type, const Json &props) const -> Result<std::shared_ptr<Widget>>;

    /// @brief 列出所有已注册 widget 类型名（工具链/API 生成器反射用）。
    /// @return 类型名列表。
    [[nodiscard]] auto list_types() const -> std::vector<std::string>;

  private:
    std::map<std::string, WidgetFactory> factories_;
};

/// @brief 注册核心 widget 工厂（Text/Button/Column/Row）。幂等，可重复调用。
auto register_core_widgets() -> void;

/// @brief 从 JSON 结构快照重建 widget 子树。未知类型或结构非法返回 Error。
/// @param j 结构快照（须为含字符串 `type` 字段的对象，`children` 递归同形；嵌套深度受限）。
/// @return 成功为子树根 widget；类型未注册 / 结构非法 / 深度超限为错误 `Result`。
[[nodiscard]] auto from_json(const Json &j) -> Result<std::shared_ptr<Widget>>;

/// @brief 递归比较两个 JSON，产出把 a 变为 b 的补丁操作列表（追加到 out）。
/// @param a 基线 JSON。
/// @param b 目标 JSON。
/// @param path 当前节点的 JSON pointer 前缀。
/// @param out 输出参数：补丁操作按遍历顺序追加到此列表。
auto diff_into(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void;

/// @brief 计算把 a 变为 b 的 JSON Patch（RFC6902 风格子集）。
/// @param a 基线 JSON。
/// @param b 目标 JSON。
/// @return 补丁操作列表（根路径起点，应用后可把 a 变为 b）。
[[nodiscard]] auto diff(const Json &a, const Json &b) -> std::vector<JsonPatchOp>;

/// @brief 把补丁应用到 target 上（原地修改）。remove 失败静默忽略（幂等）。
/// 命名为 applyPatch 以避免与 std::apply 经 ADL 冲突。
/// @param target 被原地修改的 JSON。
/// @param patch 要应用的补丁操作列表。
auto apply_patch(Json &target, const std::vector<JsonPatchOp> &patch) -> void;

/// @brief 生成单个组件的 schema（类型/容器性/属性键/线程约束），供 `gen_api` 与反射 API 复用。
/// 等价于 `aurora_api.json` 中某个 widget 条目的结构。需先 `register_core_widgets()`。
/// @param name 组件类型名。
/// @return schema JSON 对象。
[[nodiscard]] auto component_schema(const std::string &name) -> Json;

/// @brief 把 widget 树序列化为 YAML 文本（仅输出，无 from_yaml 逆过程）。
///
/// 先经 `to_json(w)` 取结构快照，再委托 yaml.h 的 `to_yaml(Json)` 落文本。
/// @param w 子树根 widget。
/// @return YAML 文本。
[[nodiscard]] auto to_yaml(const Widget &w) -> std::string;

}  // namespace serialization

/// @brief 列出所有已注册组件类型名（反射，specification/08-tooling.md §2.3）。
/// @return 类型名列表。
[[nodiscard]] auto list_all_components() -> std::vector<std::string>;

/// @brief 返回单个组件的 schema（含 props/children/thread）；未知类型返回空 Json 对象。
/// @param name 组件类型名。
/// @return 组件 schema JSON。
[[nodiscard]] auto describe_component(const std::string &name) -> Json;

/// @brief 按名称子串（大小写不敏感）搜索组件，返回匹配组件的 schema 列表。
/// @param query 名称子串。
/// @return 匹配组件的 schema 列表。
[[nodiscard]] auto search_components(const std::string &query) -> std::vector<Json>;

/// @brief 返回所有已注册组件的完整 schema（含 describe 元数据）。
/// @return 全量 schema 列表。
[[nodiscard]] auto list_all_schemas() -> std::vector<Json>;

}  // namespace aurora
