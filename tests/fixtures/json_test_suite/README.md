# JSONTestSuite 语料快照

本目录是 [nst/JSONTestSuite](https://github.com/nst/JSONTestSuite) 的**逐文件快照**，供自研 JSON
编解码器（`include/aurora/core/json.h`）做 RFC 8259 合规验收。它是外部权威基准，不是项目自产
断言 —— 目的正是让「解析器悄悄偏离规范」这件事被仓库之外的标准发现，而不是被自己写的用例放行。

## 来源与版本

| 项 | 值 |
|:---|:---|
| 上游仓库 | `https://github.com/nst/JSONTestSuite` |
| 快照 commit | `1ef36fa01286573e846ac449e8683f8833c5b26a`（2024-11-22） |
| 许可 | MIT（见同目录 `LICENSE`，版权归 Nicolas Seriot） |
| 快照内容 | `test_parsing/`（318 个）、`test_transform/`（22 个）、`LICENSE` |
| 未纳入 | 上游的 `parsers/`、`results/`、`article/` 与运行脚本 —— 与本库无关 |

## 目录语义

`test_parsing/` 下的文件名前缀声明了语料对**任何**解析器的要求：

| 前缀 | 条数 | 要求 |
|:---|:---|:---|
| `y_` | 95 | 必须接受 |
| `n_` | 188 | 必须拒绝 |
| `i_` | 35 | 实现自定，接受或拒绝均可（RFC 8259 不作要求） |

`test_transform/` 的 22 个文件是「各家理解可能不同」的结构（超大数、重复键、转义 NUL、非法码点），
用于检验解析与序列化的一致性，而非接受 / 拒绝本身。

## 消费方

`tests/unit/utest_json_conformance.cpp` 是唯一消费方：

- `y_` / `n_`：硬断言，违规即失败；
- `i_`：**行为快照** —— 测试内登记当前处置策略，行为漂移必须显式改表；
- `test_transform`：可解析者做 `parse → dump → parse` 值相等 + `dump` 幂等三重断言。

语料根可经环境变量 `AURORA_JSON_TEST_SUITE_DIR` 覆盖，指向本机另一份 checkout。

## 更新流程

语料按 commit 固定，**不随上游漂移**。确需更新时：

1. 拉取目标 commit，逐文件替换，保留 `LICENSE`；
2. 更新本文件的「快照 commit」行；
3. 同步 `utest_json_conformance.cpp` 里的清单规模常量与两张策略表 —— 表与语料清单双向比对，
   增删文件不更新表会直接失败；
4. 若新增语料暴露了真实实现缺陷，修实现而不是改表；改表只用于「有意变更的实现策略」，
   且须在提交信息里说明理由。
