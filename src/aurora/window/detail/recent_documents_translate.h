#pragma once

// 最近文档 OS 投影折算层：**内部头**（与 `system_drag_translate.h` / `ime_composition.h` 同列于 src/）。
//
// 为何单列且零平台依赖：OS 侧「最近文档」在每个平台上都只吃**一种特定形态**——Win32 的
// `SHAddToRecentDocs` 吃 UTF-16 路径串、macOS 的 `NSDocumentController` 吃 `NSURL`。而 store 给的
// 是一份「新 → 旧」的 UTF-8 字符串列表，里面可能混着空串、重复项、以及多于平台上限的条目。
// 「store 列表 → 平台可投递清单」这层折算若写在各座桥里，就会出现两套次序（谁先谁后、谁被丢弃
// 各处不同），而次序恰恰是本特性的语义。故折算收敛为**纯函数** + **可无头逐值单测**，两座（本次
// 一座）平台桥共用同一份计划。
//
// 本头不引任何 `<windows.h>` / Cocoa / Xlib 头：只有 `<string>` / `<vector>` / `<functional>` 与
// `core/result.h`，因此在任何 CI 作业（含无头 Linux）上都能编译并被单测覆盖。
//
// 不做的事：**路径规范化与存在性校验**。与 store 的字符串级口径同源（见
// `include/aurora/app/recent_documents.h` 类注释「路径口径」）——相对路径照常进入计划，由应用在
// `add` 前自行规范化。折算层一旦替应用猜绝对路径，猜错的代价是一条错记录留在系统列表里且无从撤销。
//
// 门控：无。本头在**所有**平台编译（含 wasm）。

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::recent_documents {

/// @brief store 列表 → **投影计划**（平台无关、可直接逐值断言的投递清单）。
///
/// 三条折算规则，按固定次序执行：
/// 1. **丢弃空串条目**——空串不是路径，推给 OS 只会生成一条无主记录（与 store 的「空路径为 no-op」
///    同口径；注意这与「相对路径照进计划」是两条不同处置，不是同一件事）；
/// 2. **去重并保持首次出现序**——同一路径的多次出现只投第一条，次序不重排；
/// 3. **按平台上限截断，新 → 旧优先**——`documents` 是「新 → 旧」序，超上限时**尾部（最旧）先丢**。
///
/// @param documents store 的当前列表（新 → 旧序；原样消费，本函数不改入参）。
/// @param max_entries 平台上限（由桥申报）；为 0 时产出空计划（调用方据此走 no-op）。
/// @return 投影计划（可为空——空列表产出空计划，这是合法结果而非错误）。
[[nodiscard]] auto build_projection_plan(const std::vector<std::string> &documents, std::size_t max_entries)
    -> std::vector<std::string>;

/// @brief 平台桥登记到折算层的**投影通路**（进程级单槽）。
///
/// 存在的理由：公共入口 `RecentDocuments::project_to_os()` 是进程级的、且必须在**无头 CI** 里被证到
/// 「显式推送 / 不隐式触发 / 失败隔离」。故折算层持一个可注入的函数表：真机上由平台桥登记，单测里由
/// 伪造后端登记，两侧走同一条代码路径——被测的就是产品代码本身，不是它的复刻。
struct ProjectionBackend {
    /// @brief 该平台的条目上限（折算层据此截断）。
    ///
    /// 由桥申报而非折算层写死：上限是平台事实（Win32 取用户「最近使用的项目数」默认值），
    /// 写死在公共逻辑里就是第二个真源。
    std::size_t max_entries = 0;
    /// @brief 把整份计划投递给 OS（计划已折算完毕，桥只做形态转换）。
    std::function<Result<void>(const std::vector<std::string> &plan)> project;
    /// @brief 撤销本进程贡献给 OS 的记录（语义与条目范围由各座桥自述，见其头注）。
    std::function<Result<void>()> clear;
};

/// @brief 登记投影通路（覆盖式；进程级单槽）。
/// @param backend 待登记的通路（`max_entries` 与两个回调由桥填）。
auto register_projection_backend(ProjectionBackend backend) -> void;

/// @brief 注销投影通路（清空单槽；幂等，未登记时为空操作）。
auto unregister_projection_backend() -> void;

/// @brief 当前登记的投影通路。
/// @return 已登记时指向该后端（进程生命周期内稳定）；未登记时为 nullptr。
[[nodiscard]] auto projection_backend() -> const ProjectionBackend *;

}  // namespace aurora::recent_documents
