# CODING_STANDARDS.md

> 配套：功能规格见 `SPECIFICATIONS.md`；架构见 `ARCHITECTURE.md`；概念映射见 `CONCEPTS.md`；配方见 `GUIDELINE.md`。
> 本文件规定 Aurora 的编码与 API 设计硬规范，所有贡献必须遵守。
> 第 7 章「AI 友好性」汇集编码类条目；架构/设计类条目见 `architecture/ARCHITECTURE_AI.md` §12。

---


本文档已划分为以下子文档（位于 `./<主题>/` 下）：

- [CODING_ERRORS_NAMING.md](./coding/CODING_ERRORS_NAMING.md) — 错误处理 / 命名 / 文档与示例 / 元数据与可观测 / 契约与表达 / 约束总结
- [CODING_AI.md](./coding/CODING_AI.md) — AI 友好性（编码类条目）
- [CODING_VERSIONING.md](./coding/CODING_VERSIONING.md) — 版本与变更管理
- [CODING_SIGNATURE.md](./coding/CODING_SIGNATURE.md) — 函数签名 / 内部工具层约定


> 参考 [1. 错误处理（Error Handling，对应规格 §9）](./coding/CODING_ERRORS_NAMING.md#1-错误处理error-handling对应规格-9)。

> 参考 [2. 命名（Naming，对应规格 §2）](./coding/CODING_ERRORS_NAMING.md#2-命名naming对应规格-2)。

> 参考 [3. 文档与示例（对应规格 §12）](./coding/CODING_ERRORS_NAMING.md#3-文档与示例对应规格-12)。

> 参考 [4. 元数据与可观测（对应规格 §21）](./coding/CODING_ERRORS_NAMING.md#4-元数据与可观测对应规格-21)。

> 参考 [5. 契约与表达（对应规格 §23）](./coding/CODING_ERRORS_NAMING.md#5-契约与表达对应规格-23)。

> 参考 [6. 约束总结（Invariants 链接）](./coding/CODING_ERRORS_NAMING.md#6-约束总结invariants-链接)。

> 参考 [7. AI 友好性（编码类条目）](./coding/CODING_AI.md#7-ai-友好性编码类条目)。

> 参考 [8. 版本与变更管理（Versioning & Change Management）](./coding/CODING_VERSIONING.md#8-版本与变更管理versioning--change-management)。

> 参考 [9. 函数签名（参数与返回类型）](./coding/CODING_SIGNATURE.md#9-函数签名参数与返回类型)。

> 参考 [10. 内部工具层约定（Internal Utilities）](./coding/CODING_SIGNATURE.md#10-内部工具层约定internal-utilities)。
