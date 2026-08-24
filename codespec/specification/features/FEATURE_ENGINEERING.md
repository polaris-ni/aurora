# G. 工程约束（#24）

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 相关核心子系统实现（H 系列）见 [`../subsystems/`](../subsystems)（H.1–H.10c 信号/动画/环境/事件/渲染/窗口/平台）与 [`../subsystems_api/`](../subsystems_api)（H.11–H.17 + Log + AI-First 序列化/布局/控件/Inspector/工具/日志）。

#### #24 Token 效率 + 编译速度约束

**核心目标：** AI 迭代循环效率

**Token 效率：**

```cpp
// ❌ Token 浪费（冗余前缀）
aurora::Widgets::Button::ButtonConfig config;
config.setButtonTextColor(aurora::Colors::ColorValue::Red);

// ✅ Token 高效（短而不歧义）
au::Button(au::ButtonProps{ .label = "OK" });  // 使用主题默认 primary 配色（见 §H.5）

// ✅ 减少重复
using namespace aurora;
using namespace aurora::colors;
```

**量化目标：** 表达一个 10 组件的表单，代码量 ≤ 40 行 / ≤ 800 tokens。

**编译速度：**

```text
设计约束：
- 核心头文件 include 后，冷编译 ≤ 2 秒（10 组件的简单 UI）
- 利用预编译头（PCH）与实现下沉 `src/aurora/*.cpp` 减少重复解析
- 模板深度 ≤ 3 层（避免模板元编程导致的编译爆炸）
- 错误信息必须在第一个错误点就给出（不要级联错误）
```

```cpp
// ❌ 模板地狱（编译慢 + 错误信息不可读）
template<typename... Children>
auto make_vstack(Children&&... children) { /* 100行SFINAE */ }

// ✅ 简单接口（编译快 + 错误信息清晰）
auto make_vstack(std::vector<au::Widget> children) -> au::Column;
```

**编译速度基准测试（roadmap）：**

> 注：`aurora bench-compile` 工具当前未提供（已移出范围）。CI 编译速度回归测试为未来规划；当前以 `cmake --build` 实际耗时为准。

---
