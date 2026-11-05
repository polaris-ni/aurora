# CHG-069 幽灵门禁治理：补 SIMD 逐位比对实物并钉住现役声明

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-069 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | 在 `tests/unit` 下新建 `utest_simd_parity.cpp`（五组逐位比对实物）、在 `tools/check` 下新建 `check_declared_gates.py` 与其豁免表、`cmake/AuroraTests.cmake`（注册新门禁）、`include/aurora/render/detail/painter_simd.h` 与 `painter_simd.inl` 的注释、`codespec/ARCHITECTURE.md` / `codespec/BUILD_OPTIONS.md` / `codespec/CODING_STANDARDS.md` / `codespec/specification/08-tooling.md` 的门禁矩阵、`AGENTS.md` §3.1；**不改任何 SIMD 实现、分发策略与 `AURORA_ENABLE_SIMD` 默认值**；公共 API 结构无差异 |

## 动机

1. **被写成现役一票否决的门禁，实际零实现。** `codespec/ARCHITECTURE.md` §10.5 与 `codespec/BUILD_OPTIONS.md` §4.2 写「CI 由 `utest_simd_parity` 逐位比对，一票否决」，`codespec/CODING_STANDARDS.md` 写「每次改动后 `utest_simd_parity` 必须 0 failure」，`.github/workflows/ci.yml` 的 `simd-off` 作业注释也把它当比对基准；而全仓既无同名源文件、无用例、也无 CTest 注册。`cmake/AuroraTests.cmake` 按 `tests/*.cpp` 的文件 stem GLOB 注册，文件不存在即一次验证都没有。

2. **两道既有门禁都抓不到这类幽灵。** `tools/check/check_codespec_xref.py` 的 R4 只校验「带已知扩展名且含路径分隔符」的反引号 token，`utest_simd_parity` 是裸名，被当作符号提及跳过；`tools/check/check_test_registry.py` 比对的是 `aurora_test_runner --list` 与源码里的 `AURORA_TEST_CASE` 宏，完全不读文档。于是「文档声明为现役」到「CTest 真实注册」之间没有任何核对。

3. **实现侧其实万事俱备，只缺驱动。** `include/aurora/render/detail/painter_simd.inl` 文件头自陈「被 `painter.cpp` 与一个名为 `test_simd_parity.cpp` 的测试文件共同 include」，`painter_simd.h` 的注释也写「供 `test_simd_parity` 直接比对」，但两处所指文件都不存在。头里五组「标量黄金 ↔ SSE2 ↔ AVX2」三元组共 15 个函数同名同参、逐位一致的编码约定（禁 FMA、同序浮点、`cvtt` 截断）也写得很完整，缺的只是把它们放在同一份输入上跑一遍并逐字节比较的测试 TU。

4. **门禁不能用「名字不存在」自动判。** 第二批幽灵门禁全量审计在 21 个「无注册、也无同名源码」的名字里只确认这一个是真幽灵，其余 20 个是命名示例（`utest_xxx` 一类）、历史退场引用、提案中的待建名与正则误抓（`check_color` 是属性名、`check_linker_flag` 是 CMake 函数）。因此新门禁须以「显式审定的现役声明清单」为基准，并给历史 / 规划 / 示例名字一份带理由的豁免表，不能把裸名扫描直接当判据。

5. **本提案的边界。** 不修改任何 SIMD 内核实现、不改运行时 CPUID 分发、不改 `AURORA_ENABLE_SIMD` 缺省 ON 与「不 PUBLIC 传播」的既有装配，不新增公共头与错误码；只补实物测试、把声明与注册钉在一起，并订正两处指向不存在文件的注释。

## 变更内容

1. 新建 `utest_simd_parity.cpp`：直接包含 `painter_simd.inl`，对五组三元组各以同一份输入缓冲分别调用标量、SSE2、AVX2 版本，逐字节比较输出；扫描线两组的 SIMD 版本只填 4 / 8 的整数倍像素，按各自返回值裁剪比对区间后与标量结果对齐；混合与模糊两组覆盖非对齐长度与多组参数，避免判据恒真。`AURORA_ENABLE_SIMD` 关闭时这些函数不存在，同用例在**用例体内**以 `AURORA_TEST_SKIP` 退让（平台 / 能力分支不包裹用例声明，避免 `registry_integrity` 在另一种构建里单边失踪）。
2. 在 `tools/check` 下新增 `check_declared_gates.py` 与 `declared_gates_exemptions.txt`：以一份显式审定的「现役门禁声明清单」为基准，逐个核对名字是否出现在 CTest 注册集合中；清单为空、解析不到任何名字、或清单里的名字未注册即硬失败（「0 命中即硬失败」守卫）。示例 / 历史 / 规划 / 误抓名字列入豁免表并逐条写明理由，豁免表里的名字若哪天真的注册了则提示清理。
3. 在 `cmake/AuroraTests.cmake` 的 `check_*` 块注册新门禁；在 `codespec/ARCHITECTURE.md` §14.6 的门禁矩阵与 `AGENTS.md` §3.1、`codespec/specification/08-tooling.md` 的门禁实现说明里各补一条回写。新增门禁同步进 `check_agents_size` 所约束的仓库入口文档时控制体量，超限按 §5 下沉拆分。
4. 订正 `include/aurora/render/detail/painter_simd.inl` 与 `painter_simd.h` 注释里 `test_simd_parity.cpp` / `test_simd_parity` 的指向，改为真实存在的 `utest_simd_parity.cpp`；`codespec/ARCHITECTURE.md` §10.5 与 `codespec/BUILD_OPTIONS.md` §4.2 的现役声明保持不改（补实物后它们首次成立）。
5. `CHANGELOG.md` 的 `### Fixed` 记一条幽灵门禁治理；规格回写见下节。

## 验收判据

1. `utest_simd_parity` 在 `AURORA_ENABLE_SIMD=ON` 构建下真实出现在 `ctest -N` 中，五组三元组逐字节比对全绿；OFF 构建下同名用例以 `AURORA_TEST_SKIP` 退让且 `registry_integrity` 不漂移。
2. 变异自证：给任一 SIMD 实现注入一位偏差（例如把一处累加的 `+ 1` 改为 `+ 2`），对应组的逐位比对必须转红；还原后全绿。绿不算证据，转红才算。
3. 新门禁的守卫可证伪：把声明清单清空必须硬失败，把清单里某个名字改成不存在的名字必须硬失败，把一个已注册名字从注册集合里摘掉同样必须转红；正常配置下零 findings。
4. 门禁总数由 22 扩到 23 且全绿，`check_agents_size` 仍绿（`AGENTS.md` 不越 8 KiB）；既有门禁输出与回写落点逐项不变。
5. 改动 TU 的增量 lint、format-check、相关单测全绿；`aurora_api.json` 的公共 API 结构无差异。

## 回写落点

- `codespec/ARCHITECTURE.md`：§14.6 门禁矩阵补「文档声明的现役 CTest 名」一行，写明新门禁的判据与回写位置；§10.5 的现役声明补一句实物落点。
- `codespec/BUILD_OPTIONS.md`：§4.2 的确定性约束条目补写「实物比对用例的落点与声明核对门禁」，使缺省档与关闭档的口径一致。
- `codespec/CODING_STANDARDS.md`：验收条目同步补上门禁名，说明「声明现役即须真实注册」这条纪律。
- `codespec/specification/08-tooling.md`：补新门禁的实现位置、清单与豁免表的维护方式，以及「0 命中即硬失败」守卫。
