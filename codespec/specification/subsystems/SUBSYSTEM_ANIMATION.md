# H.3 动画框架

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.3**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.3 动画框架

核心目标：用声明式补间把"时间"映射到属性，AI 无需手写逐帧更新。

- **`AnimationController`**：驱动一条归一化进度（0→1）。`forward(from=-1)` 正向播放、`reverse()` 反向、`reset(t=0)` 复位、
  `value()` 取当前进度（时长在构造 `AnimationController(duration_seconds, value)` 时确定，无 set_duration）。
- **`Tween<T>`**：补间函数。`Tween<T>(a, b, curve)`（或 `Tween<T>(value)` 常量），`value(t)` 按曲线在 a→b 间插值；支持
  `int/float/Size/Point/Color` 等。
- **`Keyframes<T>`**：关键帧序列。`Keyframes<T>(stops)`（每帧 `Stop{time, value, curve}`），`value(t)` 在分段间插值。
- **`Curve` / `Curves`**：缓动曲线。`Curves::linear()`、`ease_in()`、`ease_out()`、`ease_in_out()`、`ease_in_out_cubic()`
  等（Curves 无 steps 工厂）。
- **`SpringDescription`**：弹簧物理参数（`stiffness` / `damping` / `mass`），配合 `SpringSimulation` 用于物理感动画。
- **`Animator`**：`bind(controller, tween, state)` 把控制器的进度写入目标 `State<T>`；每帧 `tick(dt)` 推进并触发刷新；
  `add_binding(fn)` 追加任意帧回调（在控制器推进后、clear_dirty 前执行）。
  `drive()` / `bind()` 登记的是 **非拥有裸指针**，`Animator` 不延长控制器与目标 `State` 的生命周期。 故 **
  `remove(const AnimationController&)`**（与 `drive`/`bind` 配对）用于注销该控制器及其全部绑定： 凡「控制器是某 widget
  的成员、却注册进 `Application` 全局 `Animator`」的场景（典型 `NavigatorHost`）， **必须**在析构函数中调用，否则 widget
  析构后下一帧 `tick` 就会写入已释放内存（use-after-free）。 对未登记过的控制器调用是无操作，可重复调用。
- **`AnimatedValue<T>`**（T3 重构）：把 `State<T>` + 一条 `Tween` + 控制器收拢一处； **内部以 `shared_ptr`
  持有驱动载荷（pimpl），因此句柄可按值返回/自由拷贝，且 `attach(animator)`
  后即使原句柄离开作用域，帧循环仍安全持有驱动载荷（不悬垂）**。提供 `forward(from)` / `tick(dt)`（自驱动一帧）/`progress()` /
  `current()`（目标 State 当前值）/ `is_completed()` / `on_completed(cb)`（到达终点一次性回调）/`attach(Animator&)`（接入帧循环）。
- **`animate(target, tween, duration_s[, animator])`**（T3 统一入口，收敛动画 API 面）：自由函数，返回已起步（`forward(0)`）的
  `AnimatedValue<T>` 句柄。两重载：① 不接 Animator，调用方自行每帧 `handle.tick(dt)`；② 额外传入 `Animator&` 则自动 `attach`
  接入其帧循环。兼容既有 `AnimationController` / `AnimatedValue` 直接构造（不删除）。

```cpp
au::State<float> opacity{0.0f};
// 最常用形态：统一入口 + 自动接入帧循环
au::Animator anim;
auto anim_handle = au::animate(opacity, au::Tween<float>(0.0f, 1.0f, au::Curves::ease_in_out()), 0.3, anim);
anim_handle.on_completed([](){ /* 动画结束 */ });
// 每帧：anim.tick(dt);  →  opacity 从 0 渐变到 1，组件自动重绘

// 或：无 Animator 时手动自驱动
auto h2 = au::animate(opacity, au::Tween<float>(1.0f, 0.0f, au::Curves::linear()), 0.3);
// 每帧：h2.tick(dt);
```

设计要点：动画只改变 `State<T>`（视觉属性如 opacity / transform）， **不改变布局盒模型**（与 §20 规则 7 一致）；布局快照在动画前后一致。
