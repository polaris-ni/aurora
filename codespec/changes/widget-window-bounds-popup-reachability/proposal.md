# CHG-005 `Popup` 挂普通容器时绘制与命中均不可达（零尺寸盒被闸整棵跳过）

| 项目 | 内容 |
|:---|:---|
| 变更编号 | CHG-005 |
| 提出日期 | 2026-10-07 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `Container` 的绘制 / 命中下降闸、`Widget` 追加命中盒家族、`Popup` 的挂载形态契约、`Rect::intersects` 的严格语义使用面 |

## 动机

**`Popup` 挂在普通容器（如裸 `Column` / `Row`）下整棵不可用：既画不出来，也点不到。** 独立探针实测（`AURORA_ENABLE_OCCLUSION_CULLING=ON`，本仓默认值）：

| 挂载形态 | 绘制出的深色像素 | `anchor_` 盒内点的命中链长度 |
|:---|:---|:---|
| 裸 `Column` 挂 `Popup` | **0** | **0** |
| `OverlayHost` 挂 `Popup` | 90 | 4 |

两个方向被同一个根因挡住：`Popup` 在常规流中占**零尺寸**盒（`Popup::on_layout` 恒返回 `c.constrain(Size{0,0})`，「浮层不参与父布局」是它的设计前提），而两条路径的闸都按「盒与区域有交集」判定：

- **绘制侧**：`Container::on_paint`（`widget.h`）的遮挡剔除闸 `if (!global.intersects(clip)) continue;`，而 `Rect::intersects` 是**严格**比较（`origin.x < o.right() && right() > o.origin.x && …`）⇒ 零尺寸盒代入恒假，整棵子树被跳过。
- **命中侧**：`Container::on_hit_test` / `on_hit_test_chain` 的下降闸 `cb.contains(local) || child.widget().covers_extra_hit_box(...)`，前者对零尺寸盒恒假；后者因 `Popup` 覆写 `covers_descendant_extra_hit_box` 为恒 `false`（`popup.h` 写明理由：「正面范式，不经祖先开闸」）亦假。

**既有契约本身不自洽。** `popup.h` 只声明了「命中侧不经祖先开闸」，却没提绘制侧仍要过基类的零尺寸闸——两条路径的契约口径分叉了。`OverlayHost` 之所以可用，是因为它**自己覆写了 `on_paint` 与 `on_hit_test_chain`、下降时无条件问每个子节点**（连闸都不设），恰好绕过了基类这两道闸。这也解释了为什么现有用例与 `demo_popup` 全走 `OverlayHost`：那条路径掩盖了这个缺口。

**失败完全静默。** 裸容器里挂 `Popup` 后既不抛错也不告警，`window_bounds()` 也返回合法值（内容仍可被事后查询），只有肉眼看「浮层没出来」+ 点不动。开发成本高，且极易误判为自己写错了 `anchor_`。

## 变更内容

### 公共面新增

`Widget` 新增一个 **protected virtual** 可达区钩子（`widget/widget.h`），与既有 `covers_descendant_extra_hit_box` 同族但语义不同：

```cpp
[[nodiscard]] virtual auto covers_remapped_descendant(const Point &local, const BuildContext &ctx,
                                                       const Point &ancestor_offset) const -> bool;
```

**缺省返回 `false`**，表示「本控件的子树没有坐标系重映射的可达区」——这是绝大多数控件的情形，**17 个现有 `covers_extra_hit_box` 调用点的行为逐位不变**（本钩子只被新增的两处闸额外问一次，缺省假值不改变任何既有判定）。

**与 `covers_descendant_extra_hit_box` 的分工**（写死在 `@brief` 里）：

| 钩子 | 回答的问题 | 坐标来源 |
|:---|:---|:---|
| `covers_descendant_extra_hit_box` | 后代申报的**追加命中盒**是否覆盖此点 | 本地坐标 + `ancestor_offset` 累加 |
| `covers_remapped_descendant` | 子树的**内容被摆在别处**时，该点是否落在那片内容上 | 控件自己换算（`bounds.origin + local` → 全局） |

后者服务「内容坐标系与布局盒不一致」的宿主：它必须**自己**把局部点换算到全局（这正是 `Popup` 的 `anchor_` 语义），故形参不给它别的出路。

### 容器闸并入

`Container` 的三处下降闸各并入一项 `child.widget().covers_remapped_descendant(local - cb.origin, ctx, bounds.origin + cb.origin)`：

1. `on_paint` 的遮挡剔除闸（`AURORA_ENABLE_OCCLUSION_CULLING` 编译期开）——**绘制侧放行是本提案的必需项**：只修命中会得到「画得出但点不到」的反向分叉。
2. `on_hit_test` 的下降闸。
3. `on_hit_test_chain` 的下降闸。

三处**必须一起改**，只改其一会让「绘制认、命中不认」或反之——与 CHG-002 对 `content_origin()` 的同款纪律。

⚠️ 绘制侧放行的**已知代价**：`Popup` 内容不再被父容器的 clip 裁剪（内容可能被裁到视口外仍照画）。这是「`anchor_` 是全局坐标」这一既有语义的必然推论——内容本就不属于父容器的裁剪坐标系。取舍记在 `05-event-navigation.md`，不在本提案引入新的裁剪概念。

### `Popup` 覆写

`Popup` 覆写该钩子：把 `local` 换算成全局点（`bounds.origin + local`），再判 `content_box.contains(global)`——与 `Popup::on_hit_test_chain` 的下降口径**逐字同构**（同一个 `content_box`、同一套换算）。关闭态 / 无子节点返回 `false`。`Popup::on_layout` / `on_paint` / `on_hit_test_chain` **一行不改**。

### 注释改口

- `popup.h` 类注释补「本控件可挂任意容器；裸容器下亦可绘制与命中」，并删去「不经祖先开闸」那句会误导的表述（改为「不经祖先的**追加命中盒**开闸」——它仍不申报追加盒，只是不再需要祖先额外开闸）。
- `Widget::covers_extra_hit_box` 的 `@brief` 里「`Popup` 覆写恒返回 `false`…不需要祖先开闸」一段同步改口。
- `codespec/specification/05-event-navigation.md` §3.2.1 / §3.2.2 登记新钩子与三处闸的关系。

### 破坏性说明

**纯新增 + 一处绘制行为变更，无破坏性变更。** 不删不改任何既有入口签名，既有调用点零改动。唯一行为变化是：`Popup`（及未来任何覆写新钩子的宿主）在普通容器下**从完全不可用变为可用**。`OverlayHost` 路径**逐位不变**（它不经过基类这三处闸）。绘制侧放行会使 `Popup` 内容不再被父 clip 裁剪——记为已知代价而非缺陷，因为该内容本就在全局坐标系。

## 验收判据

1. `popup_in_plain_container_is_painted_and_hittable`：裸 `Column` 挂打开的 `Popup`，**像素级**判据（画布预填白、内容落墨即有深色像素）+ 命中链在 `anchor_` 盒内非空。判据对「只修命中」敏感：只修命中时像素判据转红。
2. `plain_container_gate_still_culls_zero_sized_siblings`：负守卫——普通零尺寸兄弟节点（无覆写的哑控件）**仍被绘制闸与命中闸跳过**，证明新钩子缺省 `false` 未把「零尺寸一律放行」这条全局语义翻转引进来。
3. `popup_in_overlayhost_semantics_unchanged`：`OverlayHost` 路径的既有语义（`anchor_` 盒内命中 / 盒外不命中、关闭态不命中）逐位不变。
4. `popup_draw_and_hit_gates_agree`：`anchor_` 盒内一点**既被绘制又被命中**，盒外一点**两者都不**——钉住三处闸同改，否则出现「画得出点不到」或反向分叉。

**变异自证**：

- 只改绘制闸（`on_paint`）不改命中两处 ⇒ 判据 1 的命中段与判据 4 转红。
- 只改命中两处不改绘制闸 ⇒ 判据 1 的像素段转红（这正是「只修命中得到反向分叉」的证明）。
- 新钩子缺省改为 `true` ⇒ 判据 2 转红（零尺寸兄弟被放进来了）。
- `Popup` 覆写的换算写成「`local` 直接当全局」⇒ 判据 1 转红（`Popup` 在树上非零位置时错位）。

## 回写落点

- `codespec/specification/05-event-navigation.md`（§3.2.1 覆盖区闸、§3.2.2 追加命中盒的聚合深度：登记新钩子与三处闸的关系及绘制侧放行的代价）
- `codespec/specification/04-widget.md`（§6.3 附近：登记「坐标系重映射宿主」除窗口盒外还有绘制 / 命中两侧的闸）
