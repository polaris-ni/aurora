# Commit Message 规范（Aurora）

> 本规范约定仓库提交信息的统一写法，确保 `git log` 可读、可自动归类，
> 并与本项目的 **SemVer 版本策略**（`CODING_STANDARDS.md` §8）和 **`CHANGELOG.json` 变更追踪** 对齐。
> 提交信息支持**简体中文或英文**（二选一，同一仓库内保持一致即可）。

---

## 1. 格式

采用 Conventional Commits 结构，头部一行 + 空行 + 可选正文 + 空行 + 可选脚注：

```
<type>(<scope>): <subject>

<body>

<footer>
```

- `type`：提交类型（英文小写，见 §3）。
- `scope`：受影响的模块 / 子系统（中文或英文均可，见 §4）。可省略。
- `subject`：一句话简述（简体中文或英文），**动词开头、不加句号、≤ 50 字/词**。
- `body`：说明**为什么**做此改动、**做了什么**。每行 ≤ 72 字，可多段。
- `footer`：破坏性变更、关联 Issue/PR、对应 `CHANGELOG.json` 条目等。

---

## 2. 完整示例

中文提交：

```
feat(painter): 新增圆角矩形填充接口

为支持卡片阴影与气泡背景，Painter 新增 fill_round_rect，
内部走 SDF 慢路径，与现有 fill_rect 共享裁剪逻辑。

Ref #142
对应 CHANGELOG.json: added Painter.fill_round_rect
```

英文提交：

```
feat(painter): add rounded-rect fill API

Add Painter::fill_round_rect for card shadows and bubble
backgrounds; uses the SDF slow path and shares clipping
logic with the existing fill_rect.

Ref #142
CHANGELOG.json: added Painter.fill_round_rect
```

破坏性变更示例：

```
refactor(widget)!: 统一 on_paint 入参为全局坐标

on_paint 接收的 bounds 现统一为相对窗口客户区的全局坐标，
子控件需基于 bounds.origin 计算绘制位置。

BREAKING CHANGE: 自定义 Widget 的 on_paint 实现须改用全局坐标，
否则绘制偏移错误。
对应 CHANGELOG.json: breakingChanges on_paint 坐标系变更
```

---

## 3. Type 列表

| type | 含义 | 版本影响 |
|------|------|----------|
| `feat` | 新增功能 / 公共 API | MINOR（次版本） |
| `fix` | 缺陷修复 | PATCH（补丁） |
| `docs` | 仅文档（`codespec/`、`README`、注释型文档） | PATCH |
| `perf` | 性能优化（不改变对外行为） | PATCH |
| `refactor` | 重构（不改变对外行为） | PATCH |
| `test` | 新增 / 修正测试（`tests/`） | PATCH |
| `build` | 构建系统 / 依赖（`CMakeLists.txt`、`cmake/`、`third_party/`） | PATCH |
| `ci` | CI / 工作流（`.github/`） | PATCH |
| `style` | 格式化（不影响逻辑，如 clang-format） | PATCH |
| `chore` | 杂务（版本号 bump、清理、非代码改动） | PATCH |
| `revert` | 回滚某次提交 | 视被回滚内容 |

> 破坏性变更：在 `type` 或 `type(scope)` 后加 `!`（如 `feat(widget)!:` / `refactor!:`），
> 并在 footer 写 `BREAKING CHANGE: <说明>`。破坏性变更须触发 **MAJOR** 版本 bump。

---

## 4. Scope 取值

优先取受影响的子系统名，保持与代码目录 / 命名空间一致：

- 渲染：`painter`、`render`、`image`、`font`
- 控件：`widget`、`button`、`scroll`、`lazy_list`、`chip`、`navigator`
- 布局：`layout`、`flex`
- 后端：`surface`、`win32`、`x11`、`wayland`、`glfw`、`wasm`、`macos`、`headless`
- 子系统：`storage`、`animation`、`event`、`core`、`logger`
- 工程：`cmake`、`tests`、`examples`、`tools`、`docs`、`ci`

多个模块受影响时可省略 scope，或取最关键的一个。

---

## 5. 编写要求

1. **中文或英文**：subject 与 body 使用简体中文或英文（二选一）；技术专有名词（`Painter`、API 名、CMake 选项）保留英文。同一仓库内尽量统一语言风格。
2. **动词开头**：中文用「新增 / 修复 / 重构 / 优化 / 移除 / 调整」等祈使句，如 `feat(storage): 新增异步写入接口`；英文用祈使句，如 `feat(storage): add async write API`。
3. **不写句号**：中文 subject 末尾不加 `。`；英文 subject 末尾不加 `.`。
4. **一行一个语义**：一次提交聚焦一件事；若含多类改动（如 feat + fix），拆成多次提交。
5. **关联可追溯**：涉及 Issue / PR 时在 footer 写 `Ref #<id>` / `Close #<id>`；API 变更须注明对应 `CHANGELOG.json` 条目。
6. **与版本策略对齐**：`feat` → 升 MINOR；`fix`/`perf`/`docs` 等 → 升 PATCH；带 `!` 或 `BREAKING CHANGE:` → 升 MAJOR，并写迁移说明。
7. **不提交无关文件**：仅纳入本次实际改动的业务文件；构建产物（`build*/`）、本地 AI 工具目录（`.codebuddy/` 等）已由 `.gitignore` 忽略，勿 `git add -A` 强行纳入。

---

## 6. 禁止事项

- 禁止 subject 为空或仅写 `update` / `fix bug` 这类无信息内容。
- 禁止把不相关的多个 feature / 修复混在同一提交（破坏 bisect 与 revert 粒度）。
- 禁止在 `style` 提交里夹带逻辑改动；格式化与逻辑改动分开提交。
- 禁止提交被 `.gitignore` 忽略的产物（见 §5.7）。
