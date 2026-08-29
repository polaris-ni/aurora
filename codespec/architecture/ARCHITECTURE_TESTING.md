# 测试与 CI 架构（Testing & CI Architecture）

> 本文档描述 Aurora **测试体系的分层、组织约定与 CI 执行层**，属于架构层（architecture）。
> - 测试 **编写规则与命名约定**见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)（测试组织约定 §3 4.8–4.10）。
> - 性能基准属于 [`ARCHITECTURE_PERF.md`](./ARCHITECTURE_PERF.md) 范畴，本文档仅交叉引用。
> - 本文档不重复上述内容，只说明「测试在系统里如何分层、组织与被执行」。

---

## 14.1 测试体系目标

- **可回归**：每次变更可经 `ctest` 全量复跑，失败即阻断。
- **渲染零差异（golden）**：光栅输出像素级稳定，快速路径 / SIMD 路径必须与标量黄金路径逐位一致。
- **跨平台一致**：同一测试矩阵覆盖 Linux / Windows / macOS。

---

## 14.2 测试分层

### 14.2.1 单元测试（`tests/test_*.cpp`）

- 每个公共源文件对应一个 `test_*.cpp`（与 `examples/demos/demo_*.cpp` 同构：1 源文件 ↔ 1 测试 ↔ 1 demo）。
- 经 `cmake/AuroraTests.cmake` 收集（`file(GLOB CONFIGURE_DEPENDS)`），**全部用例链入单一可执行 `aurora_test_runner`**：用例用
  `AURORA_TEST()` 宏静态自注册（用例名 = 文件名 stem），`main()` 由 `tests/au_test_main.cpp` 唯一提供，测试文件不再自带 `main`。
  CTest 逐条以 `aurora_test_runner --run=<stem>` 注册（进程隔离与旧「一文件一可执行」等价），并由 `registry_integrity` 守护
  漏注册。构建从「每文件各自链接 libaurora」收敛为一次链接（全量构建提速的核心）。编写规则见 §14.3 与 CODING_STANDARDS §3 4.10a。
- 覆盖：布局求解、响应式信号、序列化往返、存储后端、错误码构造等核心逻辑。

### 14.2.2 Golden 测试（渲染像素级）

- 以 `test_offscreen`（整合了原 `test_golden`）为主：把 widget 树渲染到 `HeadlessSurface` 内存缓冲，与 golden 基准图逐像素比对。
- **依赖相对路径**：golden 等测试须从 **仓库根**运行（如 `./build/aurora_test_runner --run=test_offscreen`），因 `ctest` 已为其把
  CWD 设为仓库根，手工直跑亦须如此；可用 `AURORA_GOLDEN_DIR` 覆盖解析基准。
- **SIMD 双实现确定性**：`test_simd_parity` 以 37,805 比对用例（随机化 + 固定种子，覆盖 5 色对 / 非对齐宽 / 多 stop 等）逐位比对标量黄金路径与 SSE2/AVX2 快路径，一票否决（详见 [`ARCHITECTURE_PERF.md`](./ARCHITECTURE_PERF.md#10-性能检测体系performance-profiling)）。

### 14.2.3 性能基准（交叉引用）

- 衡量工具 `aurora::perf` 埋点与各项性能门槛见 [`ARCHITECTURE_PERF.md`](./ARCHITECTURE_PERF.md)；实施记录见 `architecture/ARCHITECTURE_PERF_LOG.md`。

---

## 14.3 测试组织约定

- **命名**：测试文件以 `test` 为 **前缀**（`test_*.cpp`，非 `_test` 后缀），与源文件同名主体。
- **运行**：`ctest -R <名>` 逐条拉起 `aurora_test_runner --run=<stem>`；从仓库根运行以保证相对路径解析；本地复跑建议以最高并行度
  执行（`ctest -j` 配满核心，见用户执行偏好）。共享资源竞争用例（剪贴板 `test_text`/`test_clipboard`、计时 `test_perf_display_list`）
  以 `RUN_SERIAL` **单独隔离错峰**，而非把整套退回串行。
- **新增约束**：新增公共 API / widget / 核心逻辑须配套单测并接入 CTest（见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)）。

---

## 14.4 CI 架构

CI 配置位于 `.github/workflows/`：

| 工作流        | 作用                                                                                                                                                             |
|---------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ci.yml`      | 三平台矩阵（ubuntu/gcc、windows/msvc、macos/clang），每推送/PR 跑 `configure → build（库+工具+测试）→ ctest --output-on-failure`；`concurrency` 取消旧运行以提速 |
| `release.yml` | 发布流程（构建产物 / 版本标签）                                                                                                                                  |

- **执行层职责**：CI 只负责「拉起构建 + 跑 CTest」，不承载测试设计；测试分层与设计见上文 §14.2–§14.3。
- **当前范围**：`ci.yml` 未设覆盖率门槛（覆盖率由本地约束）。如需把覆盖率门禁纳入 CI，属后续增强，不在当前架构内。

---

## 14.5 与其他文档的关系

| 主题                | 权威文档                                         | 本文档的角色                         |
|---------------------|--------------------------------------------------|--------------------------------------|
| 测试编写规则 / 命名 | [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)  | 设计模型概述 + 交叉引用              |
| 性能基准            | [`ARCHITECTURE_PERF.md`](./ARCHITECTURE_PERF.md) | 交叉引用，不重述                     |
| CI 配置（事实）     | `.github/workflows/ci.yml` / `release.yml`       | 引用，描述执行层职责                 |
