# CODING_VERSIONING

> 本文件由 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) 划分而出（版本与变更管理）。章节编号保持原样。
> 返回主线见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)。

**本文包含章节：**

- [8. 版本与变更管理（Versioning & Change Management）](#8-版本与变更管理versioning--change-management)

## 8. 版本与变更管理（Versioning & Change Management）

> 版本与变更管理规范。公开 API 的稳定性直接影响生成代码的可用性，故单列。

Aurora 采用 **语义化版本（SemVer 2.0）**：`主.次.补`（MAJOR.MINOR.PATCH）。

### 规则

- **次要版本只增不删**：新增 API/类型/属性可在 MINOR 增加；不得删除或破坏既有公开 API（保持向后兼容）。
- **主版本允许破坏性变更**：破坏性修改（删除/改语义）只能进 MAJOR，并附迁移指南。
- **补丁版本**：缺陷修复、文档、性能，不引入 API 变更。
- **变更追踪**：所有公开 API 变更写入 `CHANGELOG.json`（类型化，版本真相以 `currentVersion` 为准）；`aurora_api.json` 无顶层版本字段，仅按条目 `since` 标注引入版本。
- **AI 友好性影响**：API 删除会让依赖旧签名的生成代码失效，因此非 MAJOR 不删。

### 流程

1. 改动公开 API → 更新 `CHANGELOG.json` 条目。
2. 运行 `cmake --build build --target aurora_api_json`（内部执行 `gen_api_tools aurora_api.json`）刷新 API 描述。
3. 破坏性变更 → 写迁移说明并 bump MAJOR。

---

