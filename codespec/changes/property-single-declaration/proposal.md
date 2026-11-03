# CHG-056 属性单一声明：Props 类即属性对象、权威表驱动发射面、链式构造为形态二

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-056 |
| 提出日期 | 2026-10-31 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.API.DECLARATIVE-DUAL-API.001 |
| 影响面 | 全部 76 个自描述控件的属性面（18 个 `XxxProps` 声明、其中 17 处 `public XxxProps` 公有继承；`aurora_api.json` 现收 76 控件 / 639 属性）；`XxxProps` 由「公有字段聚合」改为「私有字段类 + 权威表」，控件继续公有继承它、其四行发射 override（`describe_static` / `serialize_props` / `deserialize_props` / `collect_signals`）函数体退役为一行转调（基线：`describe_static` 255 处 / 74 文件、`serialize_props` 349 处 / 77 文件、`deserialize_props` 144 处 / 60 文件、`collect_signals(StateSink` 62 处 / 50 文件）；新增公共声明头一个（计划/待建的 include/aurora/widget/props.h：`AbstractProps<Derived>` + `PropRow` + 擦除后的 `PropSlot` + `make_prop_rows`），伞头 `aurora/aurora.h` 与 `tools/check/umbrella_manifest.txt` 随之同步；`Widget` 基类的移动面（`Widget(Widget &&) = default` / `operator=(Widget &&) = default`，`include/aurora/widget/widget.h`）改为库内一处定义以承载宿主重绑；状态层需新增一个**库内**写通道（`include/aurora/state/read_only_state.h` 的 `ReadOnlyState` 现无公共写入路径，反序列化「写当前源、不断绑」依赖它）；控件构造签名由 `Button(ButtonProps)` 逐字段搬入基类子对象 + 尾部 `bind(*this)`；**破坏性**：`obj.<属性字段> = v` 与 `XxxProps{ .a = x, .b = y }` 指定初始化器两种写法退出公共 API（构造改为链式 `XxxProps().set_a(x).set_b(y)`）、`XxxProps` 的公有字段变私有（`defaults()` 返回值只能经同名 getter 读）、换源 setter 的 `_source` 后缀并入同名过载、**每个属性行必须给回落点（活令牌成员指针或库内默认提供者），全部非必填字段随之可空——值型 `std::optional<T>`、来源型 `std::optional<ReadOnlyState<T>>`（替换现状 `ReadOnlyState<std::optional<T>>` 的双重空态）；公共签名一律裸 `T` / 裸句柄，`std::optional` 不再外泄**、公共读面按**回落点种类**给两件或一件：无参 `x()` 对全部行同形（取表态值，未表态时取回落点基线值），带上下文的 `x(ctx)` **只给回落点是主题档成员指针的行**（常量回落点的行二者恒等，故不发那个名字）；撤表态**不逐属性生成 `reset_x()`**，公共面改为全库一份的 `clear(row)`（必填行传入即编译期拒绝，擦除后的行句柄类型随之进入公共面）；表态位读件 `has_x()` 退出公共面（改由库内 `present(row)` 与 schema 消费）、「有活令牌回落的行在绘制 / 布局路径内不得用无参读件」由门禁判、`Widget` 的 `width` / `height` / `show` 描述条目由 30 份手写归一为库内一份；工具链发射形态（`src/aurora/widget/codegen` / LSP / `generate_ui` 的 `DesignatedInit` 风格，见 `codespec/specification/08-tooling.md` §10.3）与其 golden（`tests/integration/itest_to_code.cpp`、`itest_cli.cpp`、`itest_mcp.cpp`）随之改写；迁移面实测：`XxxProps{` 构造点 include 83 / src 12 / tests 178 / tools 56 / examples 152 = 481 处（146 文件），其中指定初始化器风格 379 处；`codespec/` 文档内同名写法 82 处；tests 内 4 个控件单测直读 `defaults()` 的字段；`CHANGELOG.md` 破坏性条目。**签名不变项**：`describe_static()` / `serialize_props()` / `deserialize_props()` / `collect_signals()` 的函数签名与契约、`PropDescriptor` 结构体字段集、既有 setter 的名字与参数类型、`aurora_api.json` 的**属性键集合与顺序**及既有条目字段（schema 结构本轮只新增一个布尔位 `source`，见第 3 条；其余字段集与层级不动）。 |

## 动机

> 本提案面向**不读源码**的审查者。动机第 1–3 条把现状描述清楚（数字与测量口径均可就地复核），第 4–7 条给出该现状直接产生的四处缺陷，第 8 条说明为什么必须一起改，第 9 条给出本轮新增的语言层事实——它决定了本提案最终形态的边界，并把早期几种写法排除在外。

1. **背景 · 一个属性的事实散在四个手写地点**。Aurora 的控件属性以「公有数据成员」交付：每个控件声明一个 `XxxProps` 聚合结构（字段带声明处默认值），控件本体写作 `class Xxx : public LeafWidget, public XxxProps` —— Props 的字段就是控件自身的公有字段，不再用私有 `m_*` 重复声明（这是 `codespec/CODING_STANDARDS.md` §11.1 为「三种构建形态语义等价」定的机制）。实测：18 个 `XxxProps` 声明、17 处公有继承（`TextInput` 声明了 `TextInputProps` 但本体未继承）、`aurora_api.json` 收录 76 控件 / 639 属性。除① 结构体字段与声明处默认值本身，每个属性还要在三个地点各表达一次：② `describe_static()` 里的 `PropDescriptor` 条目（名称 / 类型 / 默认值显示串 / 枚举 / 上下界 / `required` / `json_type` / `note`），供 LSP、MCP、CLI、`validate_ui`、`codegen`、Inspector 消费；③ `serialize_props(Json&)` 里一条「取值并写入键」分支；④ `deserialize_props(const Json&)` 里一条「查键并写回字段」分支。实测出现次数：`describe_static` 255 处（74 文件）、`serialize_props` 349 处（77 文件）、`deserialize_props` 144 处（60 文件）；描述符里手写 `enum_values` 102 处、手写 `min_value` / `max_value` 117 处。四份副本之间没有任何相互校验 —— 一份写错、漏写、或改名不同步，编译与测试都不会响。JSON 侧已被同一根因咬过：属性事实曾在 `aurora_api.json` 里存了四份并互相漂移，事后才收口为单一 `props` 数组；本提案处理的是**代码侧那份尚未收口的同源重复**。

2. **背景 · 刷新有两条并行通道，其中一条是手写的**。通道甲是**手标脏**：写属性时挨个调 `mark_needs_layout()` / `mark_needs_paint()`（两者都是 `Widget` 的公开入口）。实测 411 处调用点（`mark_needs_layout()` 132 处、`mark_needs_paint()` 321 处出现总数）。`mark_needs_paint()` 的动作是三合一：置本控件绘制脏位、沿布局父链失效祖先 Display List 缓存、请求一次重绘帧；Display List 缓存默认开启，所以**漏掉这一句的写操作既不触发重绘、也让祖先继续回放旧命令**。通道乙是**信号接线**：`ReadOnlyState<T>` 型属性经控件覆盖 `collect_signals(StateSink&)` 登记，基类按登记逐个订阅，任一来源变更时回调 `on_signal_dirty()`（缺省实现为「标布局脏 + 标绘制脏」）。通道乙不需要手写标脏，但粒度粗：`StateSink` 只登记来源**身份**（其契约明文「采集只登记来源身份，不附加属性名或作用域标签」），框架无法知道「变的是绘制档还是几何档」，只能取并集一律双标；实测 `collect_signals(StateSink` 62 处（50 文件），而 `on_signal_dirty()` 的覆盖**全库只有一处**（`Provider`，为换值后追加一次整子树缓存清零）。结论：一条本应数据驱动的语义，现在同时以「每控件手写虚函数覆盖」和「每 setter 手写标脏」两种形态存在，后者是 411 处、会漂移的那一条。

3. **背景 · 主题相关默认值靠一套四步仪式手工复刻**。`Theme` 现由色角色集（`ColorRoles`）、排版 15 档（`TypeScale`）、窗口装饰配色、明暗档与类型安全扩展注册表组成 —— **没有任何形状 / 间距令牌**。于是「颜色跟随主题」这一类属性只能这样写：字段声明为 `std::optional<T>`（`include/aurora/widget` 内 `std::optional<` 218 处）、取值时才回退（`inherit_theme(` 68 处 / 36 文件）、序列化时「未设置就不写这个键」（widget 头文件内 `.has_value()` 173 处）、描述符里把默认值写成**显示串** `"theme.primary"` 而非可求值的值（`include/aurora/widget` 内 `"theme.` 字面量 50 处 / 22 文件），JSON 侧再靠 `default_expr` 单独承载这一形态（schema 现收 77 条，`default` 条目 558 条）。这套仪式目前是纯手工的：每加一个主题敏感属性就要四步各写一遍，且没有任何机制保证四步说的是同一件事。

4. **缺陷一 · 手写标脏已经在漏，且漏在用户最常调的属性上**。以 `Button` 为样本：表征 UI 的链式 setter 中只有 `set_label`（含两个常量重载）、`set_background_source`（含转发的 `background`）与 `set_enabled` 标脏；其余 —— `text_color`、`set_corner_radius`、`set_padding`、`set_hover_color`、`set_pressed_color`、`set_border`、`set_disabled_colors`、`set_min_size` —— **只写字段、不标任何脏**。按第 2 条的机制，挂载后调用这些 setter：颜色改了不上屏、圆角改了不上屏、内边距与最小尺寸改了既不重排也不重绘，直到同帧内别的事件（如指针进入触发的 `on_hover_change`）顺带把该子树标脏才偶然刷新。同一批键在 `deserialize_props` 里连这套不完整的纪律都没有：除 `label` / `color` 两键转手走 setter 外，其余键一律**直接写字段、函数体内不做任何标脏** —— 于是 Inspector 的单属性热回写（`set_widget_prop`，其文档因此只能写「可能标脏重绘」）对这些键同样不刷新。漏标不是 Button 独有，而是 411 处手写的必然分布 —— 每新增一个 setter，都要靠人记得「这是绘制档还是几何档」。

5. **缺陷二 · 形态三在类型层根本拦不住，来源型属性还会句柄分裂**。C++ 没有属性语法，对公有数据成员的赋值观察不到、拦不住 —— 这是语言事实，不是实现疏忽。后果分两种：值型属性（如 `corner_radius`）`btn.corner_radius = 4.0F;` 改的就是绘制读的存储，于是「值改了但没人标脏」= 静默旧画面；来源型属性更严重 —— `Button` 为换源纪律把 `label` / `color` 各复制了一份私有镜像（`label_` / `color_`，公有字段与镜像是两个句柄对象，共享同一个来源节点），`ReadOnlyState` 的赋值是**换绑句柄**而非改源，于是 `btn.color = other_source;` 只改了那个不再被读的公有句柄，控件与 `collect_signals` 仍固定旧源 —— 这次赋值**永久无效**，而且完全编译通过、无任何诊断。项目已经验证过唯一零成本的拦截形态：`Button` 给 `label` 提供了同名只读访问器 `label()`，派生作用域的函数名遮蔽基类字段，使 `btn.label = x` 直接编译失败并点名「这是成员函数」；但这一手法目前**只用在一个字段上**，其余数百个字段仍是可静默误写的公有成员。业界同样如此：没有任何主流框架把「可变的公有数据成员」当响应式的写面 —— 拦截一律放在访问器（Qt 属性 / Setter、Swift `didSet`、Kotlin 委托属性、JS `Proxy`），或者干脆不给你可变字段（Flutter 全 immutable + 重建，ImGui 每帧重建）。

6. **缺陷三 · 表征性默认值无法跟随主题，且没有可扩展的落点**。`corner_radius = 6.0F`、`padding = {12, 6, 12, 6}`、各图表 `dot_radius = 2.0F / 3.0F / 4.0F` 这类**外观与几何常量**现在硬编码在字段声明处；换 `ThemeScope` 换肤时颜色全变、圆角与间距纹丝不动。要修就得先给 `Theme` 加形状 / 间距令牌组，而每加一个令牌敏感属性，就要把第 3 条那套四步手工仪式原样再写一遍 —— 现状既没有令牌的存放处，也没有「默认值来自令牌」的机器可读表达（描述符只能存显示串）。本变更的**主题档**消费端限于**与档位同型的标量属性**（圆角档 → `corner_radius` / `dot_radius`，间距档 → `gap` 一类），四边结构的 `EdgeInsets` 属性不在内，理由见变更内容第 6 条；但按第 6 条的强制回落规则，它仍必须有回落点 —— 用的是**库内默认提供者**形态而非主题档，于是「有无回落」不再是属性之间的差异，差异只在回落到活令牌还是落到常量。

7. **缺陷四 · 三形态等价性目前是「靠约定保证」的弱承诺**。`codespec/CODING_STANDARDS.md` §11.1 声明形态一（链式 setter）≡ 形态二（`*Props` 具名聚合）≡ 形态三（逐行赋值），等价性由「控件继承 Props、字段即存储」保证。但按第 5 条，来源型属性的形态三在语义上并不等价（公有句柄与私有镜像分叉），值型属性的形态三在效果上不等价（缺标脏），文档承诺与真实行为已经背离 —— 对以「AI 易生成、生成即可信」为定位的库，这是最坏的一类缺口：模型按文档写出的代码能编译、不报错、画错画面。

8. **根因是同一个，所以必须一起改**。三件事的共同根：**属性是数据，却按代码分散表达** —— 名称与默认值在结构体、元数据在描述符条目、读写在两条分支、刷新语义在 411 处手写调用、主题回退在取值点的一次 `value_or`。只要「一条属性的事实」仍散在六个手写地点，任何局部修补（例如给 Button 补 8 处 `mark_needs_paint()`）都只是把漂移的下一次发生往后推。把「属性」重新表达成**一条住在 `XxxProps` 类里的完整声明**（字段私有、档位与约束写在同一张权威表的行上、读写件与发射面全部由该行派生），机制实现**每档语义全库只写一份**（`AbstractProps<Derived>`，零虚函数），控件侧只剩「公有继承 + 四行转调 + 构造尾部一次 `bind`」，则缺陷一、二、四在结构上消失，缺陷三获得唯一说得清的落点（行上的令牌引用 + `Theme` 的令牌组）。三项已锁定裁决作为本提案的前提：① 进响应层的是「可配置的、表征 UI 的属性」，逻辑类（`on_click`、`on_scroll_changed` 等回调，widget 头文件内 `std::function<` 142 处）不进；② 刷新语义数据驱动、由表行给定，不逐控件写虚函数覆盖 —— 全库唯一一处覆盖恰好只表达一个传播位，而虚函数是「替换语义」，不可枚举也不可审计；③ 属性写入面收口到访问器，公有字段退出公共 API。

9. **本轮新增的语言层事实（探针实测，见「变更内容」末附录），它把早期几种写法排除在外**。其一，**声明了任何 virtual 的类不是聚合**，「Props 带虚函数 + 聚合指定初始化器」两者不能共存（探针 9：两编译器同报非聚合）—— 于是「Props 层零虚函数、多态留在 Widget 层」是硬约束而非风格选择。其二，**在自身类体内取本类私有成员的指针，写成 `static constexpr` 数据成员会编译失败，写成 `static constexpr` 成员函数才成立**（探针 1 / 2 / 3：字段声明在其后的数据成员形态被三套编译器一致拒为「不是成员」，把字段提前则通过，函数形态无论位置都通过）—— 权威表必须是函数形态的行，否则要么把私有字段顶到类最前面（与「公共 API 在前」的既有排版纪律冲突），要么放弃 constexpr。其三，**指定初始化器不能指名继承来的成员**（探针 8），而「公有字段 + 继承 Props」的形态二恰好依赖它 —— 即便退让到公有字段，形态二也已比现状窄一格。其四，**属性名与换源句柄可以同名过载**（探针 10 通过），但**字符串型属性的字面量调用必须保留便利重载**（探针 11 在三套编译器上一致报「无匹配函数」；今天 `include/aurora/widget/button.h` 的三个 `set_label` 重载正是这条规则的既存实现）。其五，值型属性的**全默认参数构造**不可行：默认实参只能从尾部省略，且相邻同型参数（`Button` 有 5 个连续可选色字段、`TextProps` 16 字段）静默错位 —— 那是第 5 条缺陷二的同类且更隐蔽，本轮不予采纳（该结论无探针，是语言规则的直接推论，附录只收有编译证据的条目）。

## 变更内容

0. **定稿形态一览**。属性对象就是 `XxxProps` **类**：字段私有、读写件与权威表同住这个类、零虚函数（CRTP 基类提供机制）；控件**继续公有继承它**（继承面即公共 setter / getter 面，控件侧零转发样板）；发射面四行转调基类通用实现；构造面为链式 `XxxProps().set_a(x)`；宿主引用经 `bind` 建立、由 `Widget` 的移动面单点重绑。机制一份、真源一处、访问器手写、字段不可直写 —— 第 5 条的静默旁路从「门禁拦」升级为「编译拦」，连控件自己的类内代码也只能走访问器。**回落点强制、位与值同位**：每个属性行都有回落点 `.fallback(...)`（活令牌成员指针或库内默认提供者，第 6 条），于是**可空 ⇔ 非必填** —— 不再按「这行碰巧有没有令牌」分叉；`std::optional` 只存在于私有字段，公共签名一律裸 `T`；公共读面是**无参 `x()` 全行同形**（表态值 / 未表态时取回落点基线值），**带上下文的 `x(ctx)` 只给回落点为主题档成员指针的行**（当前 47 行，见第 2 条）；撤表态是**全库一份公共件 `clear(row)`**，不逐属性生成 `reset_x()`。换肤时「系统给的」与「用户给的」由表态位机械判读（库内 `present(row)` + schema 的 `overridden`），不靠值猜。**本轮改调**：上一条提交定的是「读面同名两件对全部 639 行同形 + 非必填行各发一个 `reset_x()`」，实测该口径要给每个非基类属性行净发两件新面（`x(ctx)` 与 `reset_x()` ≈ 351 + 340 个方法名），而这两件里有一件语义恒等、另一件只是既有能力的换名，按「全库方法名总量」这条判据收窄为上述形态（判据与代价见第 2 条）。

1. **单一声明点：`XxxProps` 类 + 每属性一行的权威表（函数形态的行）**。以 `Button` 为样本（15 个属性，此处示 7 行，其余同型）：

    ```cpp
    // include/aurora/widget/button.h
    class ButtonProps : public AbstractProps<ButtonProps> {   // 零虚函数（CRTP）
      public:
        // ---- 权威表：键名 / 成员指针 / 档位 / 约束 / 回落点只在这里各出现一次 ----
        // 每行二选一：`.fallback(...)`（回落点：主题档成员指针或库内默认提供者）或 `.required()`（回落点即自身）。
        static constexpr auto label_row() { return prop_row<&ButtonProps::label_>("label").layout().required(); }
        static constexpr auto color_row() { return prop_row<&ButtonProps::color_>("color").paint().source()
                                                .fallback(&ColorRoles::primary).note("Background color"); }
        static constexpr auto on_color_row() { return prop_row<&ButtonProps::on_color_>("on_color").paint()
                                                     .fallback(&ColorRoles::on_primary); }
        static constexpr auto font_size_row() { return prop_row<&ButtonProps::font_size_>("font_size").paint()
                                                     .json_type("number").bounds(0.0F, 4096.0F)
                                                     .fallback(&ButtonProps::font_size_default); }
        static constexpr auto corner_radius_row() { return prop_row<&ButtonProps::corner_radius_>("corner_radius").paint()
                                                         .bounds(0.0F, 1.0e7F).fallback(&ShapeScale::medium)
                                                         .note("Corner radius (dp)"); }
        static constexpr auto padding_row() { return prop_row<&ButtonProps::padding_>("padding").layout()
                                                  .fallback(&ButtonProps::padding_default); }
        static constexpr auto enabled_row() { return prop_row<&ButtonProps::enabled_>("enabled").paint()
                                                .fallback(&ButtonProps::enabled_default); }

        // ---- 表本体：只列行名，不再重复键名与成员指针 ----
        static constexpr auto rows() {
            return make_prop_rows(label_row(), color_row(), on_color_row(), font_size_row(),
                                  corner_radius_row(), padding_row(), enabled_row());
        }

        // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----
        auto set_label(LocalizedString v) -> ButtonProps & { return assign(label_row(), std::move(v)); }
        auto set_label(const std::string &v) -> ButtonProps & { return set_label(LocalizedString{v}); }
        auto set_label(const char *v) -> ButtonProps & { return set_label(LocalizedString{std::string{v}}); }
        auto set_label(ReadOnlyState<LocalizedString> v) -> ButtonProps & { return rebind(label_row(), std::move(v)); }
        auto set_color(Color v) -> ButtonProps & { return assign(color_row(), v); }              // 装入自有常量源
        auto set_color(ReadOnlyState<Color> v) -> ButtonProps & { return rebind(color_row(), std::move(v)); }
        auto set_on_color(Color v) -> ButtonProps & { return assign(on_color_row(), v); }
        auto set_font_size(float v) -> ButtonProps & { return assign(font_size_row(), v); }
        auto set_corner_radius(float v) -> ButtonProps & { return assign(corner_radius_row(), v); }
        auto set_padding(EdgeInsets v) -> ButtonProps & { return assign(padding_row(), v); }
        auto set_enabled(bool v) -> ButtonProps & { return assign(enabled_row(), v); }
        auto set_border(Color c, float width = 1.5F) -> ButtonProps & {   // 组合便利 setter：逐行落账
            set_border_color(c);
            set_border_width(width);
            return *this;
        }

        // ---- 撤表态：不逐属性发名字。公共面是全库一份的 AbstractProps::clear(row)，
        //      写法 `props.clear(ButtonProps::color_row())`；必填行（label_row()）传入即编译期拒绝。

        // ---- 读面两条规则：无参 x() 对全部行同形；x(ctx) 只给「回落点是主题档成员指针」的行 ----
        // x(ctx)：当前作用域下的生效值 —— 那些行在布局 / 绘制 / 命中路径里的唯一入口。
        // x()   ：表态值；未表态时取回落点的基线值（活令牌档取裸主题明色基线，常量档取库内默认）。
        [[nodiscard]] auto label() const -> LocalizedString { return get(label_row()); }
        [[nodiscard]] auto label_source() const -> ReadOnlyState<LocalizedString> { return read(label_row()); }
        [[nodiscard]] auto color() const -> Color { return stated(color_row()); }
        [[nodiscard]] auto color(const BuildContext &ctx) const -> Color { return resolve(color_row(), ctx); }
        [[nodiscard]] auto color_source() const -> ReadOnlyState<Color> { return read(color_row()); }   // 现状 background_source() 的归并位
        [[nodiscard]] auto on_color() const -> Color { return stated(on_color_row()); }
        [[nodiscard]] auto on_color(const BuildContext &ctx) const -> Color { return resolve(on_color_row(), ctx); }
        [[nodiscard]] auto corner_radius() const -> float { return stated(corner_radius_row()); }
        [[nodiscard]] auto corner_radius(const BuildContext &ctx) const -> float {
            return resolve(corner_radius_row(), ctx);
        }
        // font_size / padding / enabled 的回落点是库内默认提供者：x(ctx) 与 x() 恒等，故不发第二个名字。
        [[nodiscard]] auto font_size() const -> float { return stated(font_size_row()); }
        [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }
        [[nodiscard]] auto enabled() const -> bool { return stated(enabled_row()); }
        [[nodiscard]] auto font() const -> Font {   // 合成读件：不进表、不占键、不计入覆盖率；
            return assemble_font(font_size_row(), font_weight_row(), font_family_row());
        }                                      // 三行接入 TypeScale 活令牌档后本件随之获得 (ctx) 形态（第 6 条末注）

      private:
        // 回落点的库内默认形态：与该行裸值同型的静态函数（声明在类内、体在库头/源里，返回裸 T）。
        [[nodiscard]] static auto font_size_default() -> float { return 14.0F; }
        [[nodiscard]] static auto padding_default() -> EdgeInsets {
            return EdgeInsets{.left = 12.0F, .top = 6.0F, .right = 12.0F, .bottom = 6.0F};
        }
        [[nodiscard]] static auto enabled_default() -> bool { return true; }

        // 存储层：**可空 ⇔ 非必填**（第 6 条强制回落后不再按「有无令牌」分叉），且 `std::optional`
        // 只活在这里，不出现在任何公共签名上。
        ReadOnlyState<LocalizedString> label_;            ///< 必填件：不可空、无 reset、回落点即自身
        std::optional<ReadOnlyState<Color>> color_;       ///< 来源型：句柄整体可空（单一空态）
        std::optional<Color> on_color_;                   ///< 值型
        std::optional<float> font_size_;                  ///< 一属性一字段一表态位（投影件 `.project` 退役，见第 1 条末注）
        std::optional<float> corner_radius_;              ///< 值型 + 活令牌回落
        std::optional<EdgeInsets> padding_;               ///< 不同型属性也强制回落，落到库内默认提供者
        std::optional<bool> enabled_;                     ///< 值型 + 库内默认回落
        // ... hover_color_ / pressed_color_ / border_color_ / border_width_ /
        //     disabled_color_ / disabled_text_color_ / min_width_ / min_height_
    };

    class Button : public LeafWidget, public ButtonProps {   // 公有继承保留（现状机制原样）
      public:
        explicit Button(ButtonProps p = ButtonProps()) : LeafWidget(), ButtonProps(std::move(p)) { bind(*this); }

        auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }  // 一行
        [[nodiscard]] static auto describe_static() -> WidgetDescriptor { return props_descriptor<ButtonProps>("Button"); }
        auto serialize_props(Json &props) const -> void override { props_serialize(props); }
        auto deserialize_props(const Json &props) -> void override { props_deserialize(props); }

      protected:
        // 行为代码：渲染路径里「回落点是主题档成员指针」的属性一律读 x(ctx)（无参件在该路径取到的是不随
        // 换肤的基线值，门禁按行拦）；常量回落点的属性本就没有 x(ctx) 名字，直接 x() 即生效值。
        auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    };
    ```

    - 行构造器 `prop_row<&P::f>("key")` 把成员指针与键名钉在**同一个实参位**，漂移距离为零；链式旁注只在**不可派生**时出现：`.layout()` / `.paint()`（档位）、`.source()`（来源型）、`.fallback(...)`（**回落点，必填件除外每行必备**，第 6 条）、`.bounds(min, max)`、`.json_type("…")`、`.note("…")`、`.requires("other_key")`（跨字段关系，如 `border_color` 依赖 `border_width`）、`.shows("…")`（**降级为显示别名**：默认串一律由回落点派生，仅当回落点的值不可格式化时才手写覆盖）。`.required()` 与 `.fallback(...)` **二选一且必选其一** —— `.required()` 是「回落点即自身」的退化写法（字段不可空、公共件 `clear(row)` 对它编译期拒绝），门禁按这条判完备性。
    - **`.project(&Field::sub)` 与 `.token<...>` 两个旁注在本轮退役**。前者退役的理由是它与「逐键独立表态位」直接冲突：一个 `Font` 字段承载 `font_size` / `font_weight` / `font_family` 三个键时，表态位必须也按三键分，否则「只改字号」无法与「整体换字体」区分、单键撤表态（`clear(font_size_row())`）无处可撤。裁决是三键各自成行、各自一个可空字段（键集合与 schema 顺序不变，见验收判据第 1 条），聚合形态 `Font` 改由**合成读件** `font()` 现场组装（三键当前都是常量档回落，故本件也没有 `ctx` 形态；任一键接入 `TypeScale` 活令牌档，本件随之改带 `ctx`，与第 2 条同规则）—— 合成读件不进表、不占属性键、不参与覆盖率计数，与 `set_border(c, w)` 那类组合便利写件同属「词汇糖」层。后者退役的理由是它被 `.fallback(...)` 的**活令牌形态**完全包含，同一事实不该留两个拼写。
    - `.fallback(...)` 接受**两种实参形态、一个擦除槽**：主题档成员指针（`&ColorRoles::primary` / `&ShapeScale::medium`，随作用域换肤）与库内默认提供者（`&ButtonProps::padding_default`，`static auto () -> T` 的函数指针，常量档）。采纳函数指针形态而非值形态（`.fallback(EdgeInsets{...})`）的理由是**值 NTTP 对含 `std::string` 的类型不成立**——`Font` 因 `std::string` 成员而非 literal type，既不能作 NTTP 也不能进 `std::optional` 之外的常量对象；函数指针形态对任意 `T` 一致成立。值 NTTP 的短写法列为**待测腿**（见附录后「未测项」），测通则对 literal 类型放宽为可选糖，不影响本条结论。
    - 行必须是 `static constexpr` **成员函数**而非数据成员（第 9 条其二，探针实测）；私有字段保持在类末尾，与既有排版纪律一致。
    - **子节点槽不进表**：`children` / `child` 一类节点槽留在 Props 作普通私有成员与 `set_children(...)` 一类具名写件，不占属性行、不进序列化属性段（走既有 children 通道）、无档位。逻辑回调（`on_click` 等）按裁决①留在控件本体公有成员，公共写法不变。
    - `rows()` 的顺序即 schema 里属性数组的顺序（现状为字段声明序），据此保持 639 条目的既有次序不变（见验收判据第 1 条）。

2. **读写面收口在 Props 类内：字段直写编译即红，`_source` 后缀并入同名过载**。字段私有后，派生作用域（控件本体）**也访问不到基类私有成员**，于是 `props_.corner_radius_ = v` 一类的类内旁路连同外部 `btn.corner_radius = v` 一起被编译器封死，第 5 条缺陷的「内部半侧」不留残余、不需要 grep 门禁。写面全部经 `assign` / `rebind` / `clear` 三个通用件（各全库一份实现，非虚函数）：`assign(row, v)` = 读现值判等 → 写字段（不可空行写字段本身；来源行写「自有常量源」；可空行同时置表态位）→ 经宿主链接落该行档位；`rebind(row, src)` = 换绑句柄 + 落档 + 转调 `resubscribe_signals()`（既有控件侧通道）；`clear(row)` = 撤表态、退回该行的回落点、落档（**只对非必填行实例化**；必填行传入即编译期拒绝）。读面是**四件对全部行同形的通用件**：`resolve(row, ctx)` 给生效值、`stated(row)` 给表态值或未表态时的回落点基线值（公共面 `x()`）、`get(row)` 给不可空必填行的裸值（`stated` 的退化特例，公共面仍是 `x()`）、`read(row)` 给句柄（公共面 `x_source()`）。第五件 `present(row)`（表态位）**只供库内**序列化与 Inspector 消费，不进任何公共签名。
    - **通用件升公共面的唯一判据：这一件没有逐属性替代**。`set_x` / `x()` / `x_source()` 都有具名件，故 `assign` / `rebind` / `resolve` / `stated` / `get` / `present` 全部留库内；只有撤表态不发具名件，于是 `clear(row)` 成为公共面上唯一的通用件，形参是擦除后的行句柄类型（`PropSlot`，见影响面那条新增声明头清单），写法 `props.clear(ButtonProps::color_row())` —— 行本来就是 public `static constexpr` 函数，所以这个调用**完全可 grep、可跳转、零推导**，比逐属性 `reset_x()` 更透明；代价是 `PropSlot` 随之进入公共面（一个类型 + 一个函数，非 340 个名字）。
    - **公共读面两条规则：无参 `x()` 对全部行同形；带上下文的 `x(ctx)` 只给「回落点是主题档成员指针」的行**。无参件答「用户表了什么态；没表态时给回落点的基线值（活令牌档取裸主题明色基线，常量档取库内默认）」，带 ctx 件答「当前作用域下的生效值」。这两个答案**只在回落点随作用域变的行上真的不同**：常量回落点的行 `x(ctx) ≡ x()` 恒等，给它们发第二个名字是纯噪声。实测这一类占多大面：639 条属性 / 351 条非基类行里，schema 中带 `default_expr = "theme.*"` 的只有 47 条，现状头文件里也只有 33 个不同名的 `resolve_*` 函数。所以 `x(ctx)` 的面随 `Theme` 令牌组扩张而增长（`ShapeScale` / `SpacingScale` 接入后按现状字段估约 80～100 行），而不是按 351 行全发。代价如实记：渲染路径不再有「一律写 `x(ctx)`」这一句口诀，给常量档行误写 `padding(ctx)` 时**编译即红、当场自纠**（不是静默取到旧主题画面），换来的是全库方法名总量少约三百个，以及门禁判据由「渲染路径内禁一切无参读件」收窄为「渲染路径内，该行有活令牌回落才禁无参读件」（误报面更小，见第 9 条）。**这条与本提案上一轮修订「两件对全部 639 行同形」的裁决相反，是对撞而非补充**：那次治的是「每属性三件方法」的心智负担，口径是**每行的件数**；本条的口径是**全库方法名总量**，而模型枚举「有什么可用」时读的是全库符号面（`AGENTS.md` 硬规则 4 的两条事实源就是头文件 grep 与 `aurora_api.json`），故以后者为准；每行件数在本条落地后仍是二～四件，未回退到三件以上。`resolve_` 前缀仍退役；现状 `Button::resolve_background()` 一类私有派生函数改为转调 `color(ctx)` 的调用点糖。
    - **可空 ⇔ 非必填，`std::optional` 只活在私有存储层**。第 6 条把回落点变成每行必备之后，可空性不再是「这行有没有令牌」的函数，只剩下「这行是不是必填件」这一个分叉；而必填件（`Button` 的 `label`、`Text` 的 `content`）本就不该有回落——「未填的文本」回落成空串是假值而不是默认值。所以判据更简单、也仍机械可判：`required()` 行裸 `T` / 裸句柄不可空，且公共件 `clear(row)` 对它编译期拒绝；其余行一律可空、一律可被公共件 `clear(row)` 撤表态（不发逐属性 `reset_x()` 的理由见上方「通用件升公共面的唯一判据」那段）。无回落点的裸 `T` + 声明处默认值这一形态本轮**取消**（原样保留会留下「同一事实两处住处」：默认值在字段初始化器、回落点在行上）。
    - **来源 + 回落的复合形态写作 `std::optional<ReadOnlyState<T>>`，不写现状的 `ReadOnlyState<std::optional<T>>`**。后者有两重空态（句柄未绑 / 句柄内值为空），组合出「句柄存在但值为空」这种既非表态也非回落的状态，公共签名里还带 optional；前者只有一重（未表态），句柄一旦存在其 `get()` 就是生效值。这是**破坏性**类型变更，随第 10 条迁移。
    - 被否决的三种「不用 optional」写法，各自的失败模式是：**哨兵值**（`0` / `Color{}` / `-1` 表示未设置）—— `corner_radius = 0` 是合法的直角、`Color{}` 是合法的不透明黑，哨兵与真值不可区分，正是本条要消灭的那类假阳；**旁挂存在位**（裸 `T` + `bool set_` 或一张位图）—— 两份状态可各说各话，写值忘改位即骗人，且序列化 / 判等 / 发射三处都要各自记得读位；**`std::variant<token_ref, T>`** —— 它与「可空 + 回落点在行上」信息量相同，但把同一件事分在两处表达（变体里的令牌引用 vs 行上的 `.fallback`），冲突时无从裁决。判等短路（`set` 内）仍按现状推广为通用件里的统一规则。
    - 同样否决**引入属性封装类型 `Property`（薄封装，自带 getter / setter / reset / row）**：它替代不了任何一行公共代码（C++ 生成不了方法名，除非上第 10 条明确不做的构建期代码生成），只把 `std::optional` 改名成封装类型并把「公共面不得外泄可空性」这条门禁改成还得再拦一层；而它成立的前提（键名与回落点进类型、即 class-type NTTP 自述）本轮未实测。第 6 条的强制回落落地后，形态族已由五类降到两类（值型 / 来源型，各乘是否必填），封装类型能省的只剩「异构 optional 的模板噪声」一点收益，不足以抵上述代价。**采纳其中一条洞见**：正因为 `assign` / `resolve` 要跨两种可空形状做 `if constexpr` 分叉，探针 13 才撞上「const 成员函数里取字段类型必须 `std::remove_cvref_t`」这个真错形 —— 该纪律保留为落码要求。
    - **同名过载成立**：值型写件与换源写件共用一个名字（`set_color(Color)` / `set_color(ReadOnlyState<Color>)`），实参为值时精确匹配胜过用户定义转换，不产生二义（探针 10）。现状的 `set_background_source` / `background` 两个异名入口并入 `set_color`；`_source` 后缀从公共 API 退役（`label_source()` 一类读件后缀保留，读面不存在二义）。
    - **字符串型属性必须保留便利重载**：`ReadOnlyState` 的转换构造刻意非 `explicit`，故以 `const char *` 或 `std::string` 实参调用时值型与句柄型两条用户定义转换序列同rank，编译器报无匹配（探针 11，三套编译器一致）。解法即现状 `button.h` 已采用的形态：`set_x(LocalizedString)` / `set_x(const std::string &)` / `set_x(const char *)` / `set_x(ReadOnlyState<LocalizedString>)` 四重载，前两个转调第一个。门禁按属性类型校验重载齐备（见第 7 条）。

3. **发射面由表派生：控件侧只剩四行转调，基类属性条目 30 份归一**。`props_serialize` / `props_deserialize` / `collect_prop_sources` 在 `AbstractProps<Derived>` 里各一份实现（遍历 `Derived::rows()` 的擦除槽，槽内读写对是 `constexpr` 函数指针，非虚函数）；`props_descriptor<Derived>(type_name)` 是 `include/aurora/widget/descriptor.h` 侧的发射函数（返回 `WidgetDescriptor`，与 `PropDescriptor` 结构体字段集不变）。描述符每个字段的归宿：`name` = 行键名；`type` / `json_type` 由行的裸值类型经 `include/aurora/widget/props_io.h` 既有 `<type>_to_json` / `json_to_<type>` 惯例自动绑定（detection 查名，零新增逐属性代码）、`.json_type(...)` 旁注覆盖；`default_value` = 由行上的**回落点**派生，两种形态各有一条自动路径：活令牌形态（`.fallback(&ShapeScale::medium)`）经该 Scale 的「成员指针 → 短名」反查表出可求值的 `default_expr`；库内默认提供者形态（`.fallback(&ButtonProps::padding_default)`）在发射期调用该提供者、经同一 codec 格式化（**不实例化 Props**，避免来源型字段构造出共享节点）。`.shows(...)` 只作显示别名覆盖，不再承担默认值本体。`enum_values` 由**类型侧登记表**给出：每个枚举类型（基线 102 处手写展开）在其头文件就近登记「枚举量 → 短名」对照，与 Scale 的令牌对照表同族、由同一门禁校验表 ↔ 实际字段一致，行上不新增旁注 —— 这是对「C++20 无枚举反射、值名列表不可能由行派生」这一语言事实的正面处理，早先把它记作「由行给定」是含混表述，本轮纠正。`min_value` / `max_value`（基线 117 处）由 `.bounds(...)` 给定、`pattern`（`Color` 自动 `"color-rgba"`）、`required` 由行给定；**新增一个布尔位 `source`**（来源型行的标记，schema 本轮唯一的扩面，理由见本条末）；`note` = 行 `.note(...)`；`constraint` / `requires_props`（各 2 条）留在行旁注。**基类属性 `width` / `height` / `show`** 的描述条目现状由 30 个控件头各手写一遍，本变更收口为库内一份基类槽表，发射时并入每控件的条目（次序不变）。控件若有**非属性**序列化物（容器 children、`TextInput` 选区），其覆盖体在转调基类后自行补键。**序列化口径由「令牌行未表态不写键」扩为「全部非必填属性未表态不写键」**：落盘的是意图，不是某一刻的生效值 —— 现状把裸 `T` 属性的声明处默认值也一并写盘，于是一次保存就把用户从未要求的默认值永久烘进配置文件。`Button` 的 `on_paint` 等既有行为代码改走 `x(ctx)`；无 ctx 处（事件回调、单测、`defaults()` 比对）走 `x()`，其语义是「表态值 / 回落基线值」而非当前作用域生效值 —— 这条分工由第 9 条的门禁判据守，不靠人记，且判据范围随第 2 条收窄为「有活令牌回落的行」。现状 `Button::resolve_background()` 一类私有派生函数保留为行为侧的调用点糖，函数体改为转调 `color(ctx)`。**schema 只给「推导不出来的那位」，不给名字清单**：曾评估把每个属性的公共访问器全名逐条写进 `aurora_api.json`（模型据此一次读全公共面，不必 grep 头文件），按实测口径否决 —— 现 schema 为 206,097 字节 ≈ 51.5k tokens，`check_api_budget` 的天花板是 75k，而按现格式逐条追加五个方法名估 +70 KB（≈ 69k tokens），吃掉剩余额度的八成；这些名字又全可由「词根 = 属性键名」+ `required` + `source` + 「回落点是主题档成员指针还是库内默认提供者」推出。所以本轮 schema 的扩面就是 `source` 一位（约 +2 KB），而 `default_expr` 已经承载「哪一行跟随主题」这一事实：第 2 条决定 `x(ctx)` 发不发、第 9 条决定门禁判不判，读的都是它，不新增第二个字段。

4. **反序列化写当前源、不断绑（一处真实缺陷的修复，附带一个状态层前置依赖）**。现状 `Button::deserialize_props` 对 `color` 键转手 `set_background_source(...)`，把控件**换绑成常量源** —— 用户绑定的 `State<Color>` 被一次 Inspector 热回写悄悄断绑。本变更规定：`props_deserialize` 逐键走 `assign`，**写当前源的现值**，换源只走 `set_xxx(ReadOnlyState<...>)`。但 `ReadOnlyState<T>` 按设计**只有 `get()` 与 `subscribe()`，没有任何写入路径**（其实测形态见 `include/aurora/state/read_only_state.h`：来源节点 `source_` 私有，仅友元 `detail::HandleAccess` 与 `StateSink`），故「写当前源」需要状态层新增一个**库内**通道（`detail::HandleAccess` 增一个写形参的静态成员，或把该通道友元授予 props 层）。这条列为实施前置项。曾评估并否决两个替代：其一，把字段类型改为 `State<T>` —— 那会让所有属性变成外部可写，直接违反句柄权限纪律；其二，保留「反序列化即换绑常量源」 —— 即把上面那个已被点名的静默断绑缺陷原样留下，不可接受。

5. **标脏经宿主链接落档，不新增标脏代数**。行的档位（`Layout` / `Paint`）编译期给定，`assign` / `rebind` 通过 `Widget` 既有的公开入口 `mark_needs_layout()` / `mark_needs_paint()` 落账 —— 本变更**不引入**新的 `mark_for(Dirty)` 入口与格合并代数：实测 `mark_needs_layout_impl` 现状无「已置位即返回」短路，批量写入的落账次数与今天逐处手写同量，性能口径不回退也不新宣称收益；若后续要做幂等短路，另案处理。`mark_needs_*` 的**非属性写入**用法（hover / focus / 光标 / 滚动偏移 / 自驱动动画的 `on_paint` 末尾自调度）原样保留，迁移后剩余每一处必须属非属性写入，由门禁逐处点名。**事件粒度**：`collect_signals` 由表派生为控件侧一行，`on_signal_dirty()` 虚函数与「并集双标」粒度**本轮保持不动**（`Provider` 的唯一覆盖与整子树清零不受影响），逐源精确档位需 `StateSink` 新增按键回调的接口，属状态层扩面，按 §11.3 最小 API 原则不在本变更内做（列为后续候选，触发条件：出现「只绘制档来源属性变更却触发整树重排」的实测性能证据）。

6. **回落点强制：每个属性行要么给 `.fallback(...)`、要么标 `.required()`；`Theme` 增设形状 / 间距令牌组；活令牌是成员指针、延迟解析；显示串只在序列化边界**。前半个分句是本条的新增裁决，它把「有无回落」从**属性之间的差异**降为**同一机制的两种实参**：`.fallback(&ShapeScale::medium)`（活令牌，随作用域换肤）与 `.fallback(&ButtonProps::padding_default)`（库内默认提供者，常量档）。这样做的理由不是审美 —— 只要「有些行可空、有些行裸 `T`」这个分叉存在，存储面就必须按行分叉（裸 `T` 还是 `optional`）、写件与门禁也跟着按行分叉（`clear` 能不能实例化、判等要不要先看表态位），而 `assign` 就要在三种字段形状上做 `if constexpr`，这几处分叉说的其实是同一件事。**强制回落后，存储面与序列化面的分叉只剩「是否必填」一处**，而 `required` 已经是 schema 里导出的布尔位，两面的公共件与门禁都只按它分一次。读面另有**一处按「回落点种类」的分叉**（第 2 条：只有活令牌行发 `x(ctx)`），它不落进字段形状、也不要求逐属性记忆 —— 该事实已由 schema 的 `default_expr` 导出，发不发由表判、门禁也按表判。`Theme` 增加 `ShapeScale`（圆角档）与 `SpacingScale`（间距档），与既有 `ColorRoles` / `TypeScale` 同构（强类型字段、逐字段声明处默认值），「裸主题 = 明色基线」的 `with_defaults()` 口径不回退 —— 这条口径是活令牌行的无参读件 `x()` 能诚实返回「基线值」的前提。`.fallback` 的活令牌形态在编译期烧成一次成员指针解引用，满足 `codespec/specification/07-environment-modifier.md` §5「stringly 只允许存在于序列化边界，不进渲染路径」的既有裁决，**令牌改名即编译失败**（成员指针失配），而现状 50 处 `"theme.` 显示串改名的只有静默回落；`default_expr` 由发射侧据行上的回落点自动派生（每个 Scale 类型在自身头文件就近登记一张「成员指针 → 短名」编译期对照表，每类型一次约十行，门禁校验表 ↔ 实际字段一致），落盘走 `src/aurora/widget/serialization.cpp` 既有分支，schema 结构不变。可空行的字段可空（第 2 条：值型 `std::optional<T>`、来源型 `std::optional<ReadOnlyState<T>>`）：**布局 / 绘制取值时**经 `inherit_theme(ctx)` 解析，**绝不在构造时烘进值**；未表态不序列化；**生效值读件是带 ctx 的 `x(ctx)`，且只有该行回落点是主题档成员指针时才存在**（常量档二者恒等，第 2 条）。无参的 `x()` 对全部行存在，但它答的是另一个问题（表态值 / 回落点基线值），**在渲染路径内对活令牌行使用它即为缺陷**，由第 9 条的门禁按函数作用域切段拦下；拦的对象是「该行有没有令牌回落」这一条表上的事实，不是逐属性的记忆。同构先例仍是 Flutter `ButtonStyle`：它的每个样式字段都可空、统一经 `resolveWith(style, defaultStyleOf(context))` 合并 —— 本轮把 Aurora 收拢到同一形态；Flutter 缺的是「显式表态成同值」与「未表态」的区分（Dart 的 `copyWith` 无法区分显式 `null` 与未传参，这是其已知缺陷），正好由本条的表态位补上。
    - **活令牌形态的成员类型 = 该行裸值类型**（`.fallback(&ShapeScale::medium)` 与字段类型不匹配时成员指针解引用编译即红，这是强类型形态的固有约束，不是新增规则）。因此间距档 `SpacingScale` 的直接消费端是 `Column` / `Row` 的 `gap`、`Grid` 的 `gap`、`Form::set_gap`、`ReorderableList` 的 `gap` 一类 **float** 属性（实测库内 6 处同型字段）；`EdgeInsets` 型属性（如 `padding`）**不接活令牌** —— 四边结构与单一标量档不同型，要接必须先决定「一个档展开成四边同值」还是「一组四元档」，那是新的语义而非本变更的遗漏，按 `CODING_STANDARDS.md` §11.3 最小 API 原则另案。注意这与强制回落不冲突：`padding` 用库内默认提供者形态给回落点，字段照样可空、照样能被公共件 `clear(row)` 撤表态、照样在未表态时不写键；代价是它**没有 `x(ctx)` 这个名字**（第 2 条），因为二者恒等。于是「接不接得了活令牌」不再只是观感问题而是有公共面后果的判定，后续给 `EdgeInsets` / 字号族接档时（另案）必须连同「该行是否随之获得 `x(ctx)`」一起裁。字号 / 字重 / 字体族三个键同属此列 —— 它们各自成行（第 1 条末注），回落点本轮取库内默认提供者，接 `TypeScale` 的活令牌档要等「档成员类型与裸值类型同型」这条约束被满足（`TypeScale::body_medium` 是 `Font`，不是 `float`）。

7. **宿主绑定与移动：一处纠正 —— 不用相对偏移重绑**。Props 需要一个 `Widget *owner_` 才能落脏。`Widget &` 不可行（聚合式构造无初值、且 `Widget` 是移动型对象）；早期结论「`AbstractProps` 的移动构造按构造期记录的**相对偏移**自动重绑」经探针复核**被否**：偏移法在「把已绑定的 Props 从活控件里切出来搬移」时会在宿主之外算出一个地址并当作 owner 使用，即伪造指针 —— 那比它要修的缺陷更糟。本提案改为两点、同样零控件手写：① `Widget` 增加一个指向其属性对象的**库内链接位**（`PropsLink *`，非模板、非虚），`bind(host)` 在控件构造尾部写下它并记 owner；② `Widget(Widget &&)` 与 `operator=(Widget &&)` 从 `= default` 改为库内一处定义，把链接位的 host 指向新对象，属性子对象自身的移动构造再把链接位指回自己 —— 于是「整控件移动 / 移动赋值」自动保脏（探针 12：连续两次移动与移动赋值后 setter 仍命中新宿主）。配套规则：**拷贝不携带绑定**（属性对象的拷贝构造刻意丢 owner，拷贝即新 DTO），**控件构造尾部必调 `bind(*this)`**（模式门禁逐控件校验），「把已绑定的 Props 切出宿主搬移」不在支持面内（无悬垂、无伪造指针，最坏退化为不标脏，且公共写法到不了这条路）。附带收益：脱离宿主的 DTO 上 `owner_ == nullptr`，链式构造因此天然不产生任何标脏 —— 构造装入不需要 init 通道，也不需要早期形态里的「Dirty 返回值 + 落账仪式」。

8. **构造面：链式即形态二的新定义**。`XxxProps` 类化后不再是聚合，`Button(ButtonProps{.label = "OK"})` 这条 §11.1 现文推荐的写法编译失败（探针 9 的同族事实：字段私有 + 基类存在，两条路都不通），替代为链式：

    ```cpp
    auto btn = au::Button(au::ButtonProps().set_label("OK").set_corner_radius(8.0F).set_enabled(false));
    btn.set_on_click(fn);                                   // 回调仍按现状（形态一，逐名、顺序无关）
    auto col = au::Column(au::ColumnProps().set_children({au::Text("A"), au::Text("B")}).set_gap(8.0F));
    ```

    逐属性显式名、顺序无关、漏写者回退该行的回落点（第 6 条）—— §11.1 形态二的精神（AI 只需记「属性名 + `*Props`」、可分块生成）完整保住，且写件名与第 2 条的写面同名同物，不新增第二套词汇。位置式**首参便利重载**保留（`Button("OK")`、`Slider(State<double>, on_changed)` 一类跨类型、无跳位需求者，现状已在用）。`defaults()` 仍在（返回默认构造的属性对象，即**全部非必填件均未表态**），但字段私有后只能经 getter 读，且无参 getter 读到的是回落点基线值（tests 内 4 个控件单测的 `const ButtonProps d = Button::defaults();` 形态随之改写）。**破坏性口径**：`codespec/CODING_STANDARDS.md` §11.1 与 §2 的「具名聚合优先」条目、`codespec/specification/04-widget.md` §2.5、`codespec/specification/09-cli.md`、`codespec/specification/08-tooling.md` §10.3 的 `DesignatedInit` 发射风格一并改述；codegen / LSP 的 golden 与 `examples` 字面串（`describe_static` 里的 `.examples = {...}`）同步重写；迁移以编译器指认为准（`XxxProps{` 实测 481 处构造点 / 146 文件，其中指定初始化器风格 379 处；`codespec/` 文档内 82 处写法随文档改）。迁移期可用一次性脚本辅助批量改写，脚本不进 `tools/`、不入库。

9. **schema 与门禁：新增位只 `source` 一个布尔，一致性全部机械判定**。档位 / 响应 / 投影是库内实现事实，无库外消费端，不进 `aurora_api.json`；`source` 是本轮唯一的 schema 扩面（第 3 条末：公共名可推导所必需的最小位，约 +2 KB，现量 206,097 字节 ≈ 51.5k tokens 对 `check_api_budget` 的 75k 天花板仍在额度内）。门禁增改：
    - `check_props_table_coverage`（新增，纯文本判定）：三方计数 —— 私有字段数 ↔ `*_row()` 行数 ↔ `rows()` 里被点名的行数；`x_row()` ↔ `set_x(...)` ↔ 读件按命名对应校验，且每行必须命中 `.fallback(...)` 与 `.required()` **二选一**（都不写即红）。读面的词根规则**两条**：`x()` 对每一行都必须存在且只此一名；`x(const BuildContext &)` **当且仅当该行给的是活令牌回落**（`.fallback(&Scale::member)`，即 schema 里出 `default_expr` 的那 47 行起）时必须存在 —— 常量档行**不得**出现 `x(ctx)`，恒等就是冗余，出现即红（这一句正是「不发那三百个名字」的机械守卫，也是名字可推导性成立的前提）。撤表态**不得有具名件**：全库出现任何 `reset_<属性名>()` 声明即红，退回回落点只有公共件 `clear(row)` 一条通道（必填行传入它编译期拒绝，因此不需要门禁参与）。合成读件（`font()`、`set_border(c, w)` 一类多行糖）不在本门禁的逐行计数内，但必须点名登记，避免「有键无件」与「有件无键」两类漏判。
    - `check_no_contextless_property_read_in_render_path`（新增，范围随第 2 条收窄）：`on_paint` / `on_layout` / 命中测试等以 `BuildContext` 为参的行为函数体内，**不得对「有活令牌回落的行」使用无参读件**（`x()` / `x_source()`）—— 无参读件答的是「表态值 / 基线值」，在渲染路径里用它就是不随换肤的旧画面。判定集比一刀切小得多：逐控件只检查其权威表里带 `default_expr` 的那些行（当前全库 47 行、现状同名 `resolve_*` 函数 33 个，可判性与误报率仍列为**待测项**，见附录后「未测项」）；常量档行本就没有 `x(ctx)`，取到恒等值不构成缺陷，也不再是门禁的搜索对象。若切段判据在既有代码上误报不可控，退路是把「活令牌行必带 ctx」降级为 `codespec/CODING_STANDARDS.md` §6 的书面纪律 + 评审清单，并在提案状态里注明这条防线由机械判据改为约定判据；**此时 `x(ctx)` 只给活令牌行这条收窄依然成立**（它靠「误写即编译红」自纠，不依赖门禁）。
    - `check_no_optional_in_public_signature`（新增）：`include/aurora/**` 公共面的属性写件参数与读件返回值里不得出现 `std::optional`，**也不得出现任何属性封装类型**（豁免仅限本就承载「可空」语义的非属性接口，逐处登记）。这条把第 2 条的「optional 只活在存储层」变成机械判据，顺带拦住的正是 `ReadOnlyState<std::optional<T>>` 这一类双重空态。
    - **公共面通用件白名单**（并入 `check_naming_conventions` 的判定段，不新起脚本）：`include/aurora/**` 的 public 区里，来自 `AbstractProps` 的通用件名字**只允许 `clear` 一个**；`assign` / `rebind` / `resolve` / `stated` / `get` / `read` / `present` 任一出现在公共面即红。这一条守的是第 2 条「只有无逐属性替代的通用件才升公共面」那条判据 —— 它一旦失守，读面就会从「具名件 + 一位 `source` 可推导」退化成「通用件 + 行选择器」，而那正是被否决过的形态（每属性查声明才知道怎么读）。
    - `check_widget_bind_pattern`（新增）：每个公有继承 Props 的控件，其构造尾部必须出现一次 `bind(*this)`；缺失即红（这是对第 7 条唯一手写仪式的守护）。
    - `check_no_direct_field_write` 不再需要：字段私有，绕过写件的代码编译即红。
    - 手写 `mark_needs_*` 剩余清单逐处登记成因，未登记的新调用点即红；`"theme.` 字面量在 widget 源码中归零（仅令牌对照表与 schema 发射层允许）。
    - 令牌对照表 ↔ Scale 字段一致、枚举登记表 ↔ 枚举量一致；渲染路径（layout / paint 取值链）grep 无令牌名字面量。
    - `check_doc_comments` 覆盖新增公共头、行工厂与手写访问器；`check_naming_conventions`、`check_code_doc_sync`、`check_codespec_xref`、`check_change_proposals`、`check_umbrella_header`（新头入伞头与 manifest 基线）等既有门禁不回退。

10. **范围边界（明确不做）**：不引入构建期代码生成（Qt moc / Unreal UHT 形态）、**不引入属性封装类型**（`Property` 一类的薄封装自带 getter / setter / reset / row；理由见第 2 条末注 —— 它省不掉公共代码，只把可空性换个名字），不引入运行时反射库、不引入 `mark_for` 与合并代数、不改渲染管线与布局算法、不改 `State` / `ReadOnlyState` 的公共值语义与判等（第 4 条那个库内写通道除外，且它不改公共面）、不给 `Theme` 加明暗以外的第三色列、不动 `third_party/`、不改 `PropDescriptor` 结构、不改 schema 层级与属性键集合及顺序（**条目字段集只新增一位 `source`**，理由与实测字节口径见第 3 条末；除此之外字段集不变，口径变化只发生在既有条目的取值：`default_value` 改由回落点派生、未表态属性的键从落盘输出里消失）、不删 `on_signal_dirty()`、不给 `EdgeInsets` 一类不同型属性接活令牌（回落点走库内默认提供者形态）。一次性示例确实无法配套单测的按 `AGENTS.md` 硬规则 7 就地标注。C++26 静态反射成熟后，行上的键名字符串半区可由反射补出，其余一切不变。

### 附录：语言层探针取证

探针为纯语言层最小程序（不链 aurora 库、无平台 API、全 ASCII），落点为本目录旁的临时构建树 `build/props-probe/`（不入版控）；下表「命令」列即复现口径。三套编译器：`g++ (MinGW-Builds x86_64-win32-seh-rev1) 16.1.0`、`clang version 22.1.2 (LLVM, target x86_64-pc-windows-msvc)`、`g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`（WSL）。统一 `-std=c++20 -Wall -Wextra`。

| # | 判据 | 命令要点 | gcc 16.1 | clang 22.1 | gcc 15.2 | 裁决 |
|:---|:---|:---|:---|:---|:---|:---|
| 1 | 类体内 `static constexpr` **数据成员**取本类私有成员指针，字段声明在其**后** | `g++ -c probe_a_table.cpp -DFIELD_LAST_DATA=1` | FAIL：`'alpha_' is not a member of 'a::Props'` | FAIL：`no member named 'alpha_'` | 同 FAIL | 数据成员形态不可用（类未完整时的可见性问题，非访问控制） |
| 2 | 同上但字段声明在其**前** | `... -DFIELD_FIRST_DATA=1` | PASS | PASS | 未单测 | 佐证第 1 行成因是声明序；不为它牺牲排版纪律 |
| 3 | `static constexpr` **函数**形态的行 + `make_prop_rows(...)` 擦除成 `std::array<PropSlot, N>` + `static_assert` 读表 + 运行期遍历 | `g++ -c probe_a_table.cpp -DSLOT_CAST_CONST=1`；运行输出 `serialize=196 rows=2` | PASS + 运行断言全绿 | PASS | PASS | **采用形态**（行 = 函数；表 = `make_prop_rows`） |
| 4 | CRTP 基类经行里的成员指针读写派生类**私有**字段（无 `friend`） | 同探针 3 文件内 `AbstractProps::assign` / `read` | PASS | PASS | PASS | 访问控制只判指针形成点；基类机制无需友元声明 |
| 5 | 行 → 擦除槽的 `operator Slot()` 未标 `constexpr` | `g++ -c probe_d_aggregate.cpp -DCASE_TO_ARRAY_SAME=1` | FAIL：`call to non-'constexpr' function` | FAIL：`must be initialized by a constant expression` | 同 FAIL | 转换算子必须 `constexpr`（已并入形态定义） |
| 6 | `std::to_array({异型行…})` | `... -DCASE_TO_ARRAY_HETERO=1` | FAIL：`no matching function for call to 'to_array'` | FAIL：同因 | 同 FAIL | 否掉 `to_array` 写法，改可变参数 `make_prop_rows` |
| 7 | 公有字段 + 非空基类的聚合，指定初始化器**跳过基类**：`Derived d{.y = 1};` | `... -DCASE_BASE_SKIP=1` | PASS | PASS | 未单测 | 「公有字段路线」的语言假设其实成立；它被缺陷二的静默旁路否掉，非被编译器否掉（记录以免误记为编译不过） |
| 8 | 指定初始化器指名**继承来**的成员：`Derived d{.x = 1};` | `... -DCASE_BASE_DESIGNATOR=1` | FAIL：`'Derived' has no non-static data member named 'x'` | FAIL：`field designator 'x' does not refer to any field` | 同 FAIL | 形态二即便走公有字段也已残缺 |
| 9 | Props 声明任一 `virtual` + 指定初始化器 | `... -DCASE_VIRTUAL_PROPS=1` | FAIL：`designated initializers cannot be used with a non-aggregate type` | FAIL：`initialization of non-aggregate type` | 同 FAIL | **「Props 层零虚函数」是硬约束**；多态留在 Widget 层 |
| 10 | 同名过载 `set_x(Color)` 与 `set_x(ReadOnlyState<Color>)`（镜像其刻意非 `explicit` 的模板转换构造） | `g++ -c probe_c_overload.cpp` | PASS | PASS | PASS | 值型与换源写件**可同名**，`_source` 后缀退役成立 |
| 11 | 字面量实参下 `set_x(LocalizedString)` 与 `set_x(ReadOnlyState<LocalizedString>)` 竞争 | `g++ -c probe_e_ambiguity.cpp` / `-DFIX_STRING_OVERLOAD=1` | FAIL：`no matching function for call to 'set_label(const char [3])'` → 加 `const char *` 与 `std::string` 便利重载后 PASS | 同左（FAIL→PASS） | 同左（FAIL→PASS） | 字符串型属性**必须**保留便利重载（即现状 `button.h` 的重载组形态） |
| 12 | 绑定 / 移动：`bind` 写链接位 + `Widget` 移动构造与移动赋值重绑 + 拷贝丢绑定 + 判等短路 | `g++ probe_b_owner.cpp -o b.exe && ./b.exe` | 编译无告警、断言全绿（含连续两次移动、移动赋值、切出宿主路径） | 断言全绿 | PASS（编译，断言逻辑同） | **采纳**；相对偏移重绑法撤回（切出场景会算出宿主外的地址当 owner，伪造指针） |

| 13 | 「可空性只活在存储层」的读写面：`assign` / `rebind` / `clear` / `present` / `resolve` 五件通用件同时服务三种字段形状（不可空裸 `T` = 必填件、`std::optional<T>` = 非必填值件、`std::optional<Handle<T>>` = 非必填来源件），公共签名全裸 | `g++ -std=c++20 -Wall -Wextra probe_f_storage_shape.cpp -o f.exe && ./f.exe`（clang / WSL 同命令） | PASS，输出 `marks=5`（判等短路生效：重复写同值不落账） | PASS，`marks=5` | PASS，`marks=5` | **采用**（该探针的「裸 `T` 行」在本轮定调下即必填件，另两型即全部非必填件；分叉由 `required` 一位决定）；附带一条落码纪律：const 成员函数里取字段类型必须 `std::remove_cvref_t` 而非 `remove_reference_t`（后者留下 const，`if constexpr` 的分支选择随之错判 —— 探针实测到该错形） |

未测项如实标注（本轮定调新增四条，全部是**实施前置**取证，不是可延后的观察项）：其一，**`.fallback(...)` 的落码形态**——活令牌成员指针与库内默认提供者的函数指针要在同一个擦除槽里共存（按实参种类分派），且函数指针 NTTP 要在「类体内 `static constexpr` 成员函数中取本类另一 `static` 函数的地址」这一形态下成立（探针 1 / 3 只证了取本类**数据成员**指针的同一位置）；顺带测 literal 类型能否放宽为值 NTTP 短写法 `.fallback(EdgeInsets{...})`，测通则作糖、测不通则本条函数指针形态就是唯一写法。其二，**全量可空化的 `sizeof(XxxProps)` 增长**（`std::optional<bool>` 由 1 变 2、`std::optional<EdgeInsets>` 由 16 变 24 一类填充差），须在迁移前逐控件记录并与现状对比，作为验收判据第 10 条的量化前提。其三，**`check_no_contextless_property_read_in_render_path` 的可判性与误报率**——范围已随第 2 条收窄为「只判有 `default_expr` 的行」（全库 47 行），但按函数作用域切段判「渲染路径体内对这些行使用无参读件」仍须先在现状代码（尚无该形态）与一份人工构造的违例样本上双跑，误报不可控即按第 9 条写明的退路降级为书面纪律。其四，**`PropSlot` 进入公共面的语言层形态**（本轮新增，属实施前置）：`clear(row)` 的公共形参要能接住类外写下的 `props.clear(ButtonProps::color_row())`，须同时证三件事 —— 行 → 擦除槽的隐式转换在公共形参下仍是常量表达式（探针 5 的同族事实）、对必填行（`.required()`）传入时是 `static_assert` 编译期拒绝而不是运行期分支、以及 MSVC 两阶段名查找下 `AbstractProps<Derived>::clear` 在派生作用域可见且不必额外 `using` 声明；这三点在 g++ / clang 上可自证，MSVC 腿与 emscripten 腿同样未跑。既有两条未测腿继续挂着：emscripten / wasm 腿与 MSVC 腿本轮未跑（探针不含平台 API，风险限于「标准库版本差异」，但按项目口径未测即标未测）——实施阶段必须在 CI 的 wasm 与 Linux 两腿复跑同一组命令并记录结果，作为验收判据第 10 条的一部分。

## 验收判据

1. **派生等价基线（含变异自证）**：迁移前后对全部 76 控件逐条比对 `aurora_api.json` —— 控件集合、每控件属性键集合与**顺序**、每键的 `type` / `json_type` / `enum` / `minimum` / `maximum` / `required` / `note` 完全一致，`source` 位（本轮新增）与行的 `.source()` 旁注逐键一致；**五类点名许可**差异：默认值表示改为回落点派生（活令牌档出 `default_expr`、常量档出格式化值，覆盖全部非必填属性而非仅主题属性）、`.examples = {...}` 字面串改为链式写法、漏标脏属性的行为修正、**未表态属性不再写盘**（序列化输出是意图集合，键数下降属预期）。序列化比对口径随之改为语义等价而非逐字节：**显式表过态的键逐键逐值一致**；现状写盘而迁移后消失的键，必须能由回落点求值复原出与旧输出相同的渲染结果（每控件至少一条「旧全量键 JSON → 新意图 JSON → 逐属性生效值相等」的往返用例，用 `HeadlessSurface` 快照佐证）。**变异自证**：删掉某控件一行 `*_row()`、或把某行 `.layout()` 改 `.paint()`、或从 `rows()` 里漏点名一行、或删掉某非必填行的 `.fallback(...)` 且不补 `.required()`，覆盖门禁或派生比对必须转红；给一个**常量档**行补写 `x(const BuildContext &)`、或声明任何一个 `reset_<属性名>()`、或把 `assign` / `resolve` 之一提到公共面，第 9 条里对应的三条判据必须各自转红（这三条是自证「窄面」不是空转的关键注入）。
2. **单一声明判据（正反向各一条）**：正向 —— 在某控件加「私有字段 + 一行 `x_row()` + 在 `rows()` 点名 + 写件 `set_x` + 无参读件 `x()`」，**不改引擎任何其他代码**，编译通过且该属性在 getter / 描述符 / serialize / deserialize / `aurora_api.json` 全部出现；该行若给活令牌回落，再补 `x(ctx)` 一件；来源行另加句柄重载与 `x_source()`；**没有任何 `reset_x()` 要写**（撤表态是公共件 `clear(row)`，逐属性零新增名字）。按此计数，一个常量档值型行的全部公共件是 2 件、活令牌值型行 3 件、活令牌来源行 5 件。反向 —— 加了行忘记点名进 `rows()`，`check_props_table_coverage` 必须红；把 `x_row()` 与 `set_x` 的词根改得不一致，同门禁必须红；活令牌行漏掉 `x(ctx)`、常量档行**多写** `x(ctx)`、全库任何位置出现一个 `reset_<属性名>()` 声明、把 `assign` 或 `resolve` 提到公共面，四条各由第 9 条的对应判据转红；行上既无 `.fallback(...)` 又无 `.required()`，覆盖门禁必须红；把 `on_paint` 体内**活令牌行**的生效值读取改成无参 `x()`，`check_no_contextless_property_read_in_render_path` 必须红；在渲染路径对常量档行写 `padding(ctx)` 一类的名字则直接编译红（这一条不需要门禁，也是「窄面」的自证：少发的那三百个名字并没有留下任何静默错形，写错当场就报）。
3. **写入面收口判据（编译即红，无门禁参与）**：`obj.<属性字段> = ...` 对全部属性不再合法（全量编译天然证明）；**控件自身**的类内直写同样编译失败（基类私有字段在派生作用域不可达）；来源型属性的公有句柄 / 私有镜像双份存储归一，`btn.color = src` 那条永久无效路径消失；形态一（挂载后 setter）≡ 形态二（链式构造装入）逐属性集成测试：同一值经两条路装入后，序列化输出、快照像素、订阅计数三者全同（同一行的同一个 `assign`，等价性是构造性事实而非约定）。
4. **缺陷一闭合的实测（先红后绿）**：`Button` 那批漏标 setter 各一条回归用例（`HeadlessSurface` 逻辑快照，不引 GUI 交互测试）：控件**先挂载**、再调 setter、断言绘制档属性「取到新画面且重绘帧被请求」、几何档属性「重排并按新尺寸落位」；同批为 Inspector 热回写（`set_widget_prop` 经表逐键转 `assign` 后）补同型断言，并把 `codespec/manual-test/22-inspector.md` 相关口径从「可能标脏重绘」改述为「必然标脏重绘」。用例在改动前 HEAD 必须先失败、改动后通过，差异证据留评审记录。
5. **移动与绑定判据**：控件经 `Node` 移入子树、连续二次移动、以及移动赋值后，逐档断言 setter 仍命中标脏（对应探针 12 的真库版本）；未挂载控件上 setter 正常工作（owner 已 bind、未入树）；脱离宿主的链式 DTO 上不产生任何标脏（构造装入零落账）；`canvas.h` 对 `width` / `height` 的既有覆盖行为不回退（几何探针用例保绿）。
6. **反序列化不断绑判据**：控件绑定外部 `State<Color>` 后经 Inspector 热回写 `color` 键 —— 外部源收到变更、绑定关系保持、`collect_signals` 订阅数不变；此条在改动前 HEAD 必红（现状走换绑常量源），改动后绿。同条覆盖第 4 条的库内写通道落地证明：`ReadOnlyState` 的**公共面**仍无写入路径（其单测必须证明外部代码无法经句柄改源）。
7. **回落点与「表态位」判据（含强类型、延迟解析、同值守卫三条）**：挂 `ThemeScope` 换肤后圆角 / 间距随令牌变且**零 setter 调用**；未表态的**非必填属性一律不出现在 `serialize_props`**（口径由「仅主题属性」扩到全属性，第 1 条的往返用例覆盖），其默认值表示逐条经门禁反查到真实落点 —— 活令牌档反查到 `Theme` 成员、常量档反查到该行的默认提供者（含色角色存量显示串迁移项）；**令牌改名即编译失败**（临时改 `ShapeScale::medium` 后引用它的行必须编译红）；渲染路径 grep 无令牌名字面量。**读件档位分离**：活令牌行的生效值只有 `x(ctx)` 一个入口（签名要求 ctx，故不可能在无上下文处调用、也不可能把解析结果烘进存储）；无参 `x()` 答的是表态值 / 回落基线值，它在 `on_paint` / `on_layout` 体内对**活令牌行**的出现由 `check_no_contextless_property_read_in_render_path` 拦红。**恒等关系的正面取证**：常量档行没有第二个名字，其 `x()` 即生效值 —— 每条常量档行须有一条单测断言「明 / 暗两个 `ThemeScope` 下 `x()` 与库内 `resolve(row, ctx)` 取值全同」，这是「不发 `x(ctx)` 不丢语义」的直接证明，缺它则窄面只是断言。**表态位不再是公共方法**：`present(row)` 只在库内（序列化与 Inspector 消费），公共面不出现 `has_x()`；「用户显式 vs 系统默认」的机器可读性改由 schema 的 `overridden` / `required` 位与库内判据承担。**窄面的量化判据（本轮新增，grep 可判）**：迁移后头文件里 `x(const BuildContext &)` 形态的属性读件声明数必须 ≤ 活令牌行数（当前 47 行，接入两枚新令牌组后按实数更新）+ 已登记的合成读件数，且**常量档行的 `x(ctx)` 计数必须为 0**；`reset_` 前缀的属性写件在全库必须为 0；公共面上的 `AbstractProps` 通用件名字集合必须恰为 `{clear}`。三条一起证明「少发名字」是被守住的，而不是某次改着改着就补回去的。**同值守卫（本条的核心用例）**：把 `corner_radius` 显式设为与当时 `theme.shape.medium` **同值**，随后换肤到不同圆角的档 —— 该控件必须保持用户值不变，而同批未表态的控件必须跟随变；两半各一条断言，缺一即无法区分「系统默认」与「用户默认」。**撤表态用例**：`clear(ButtonProps::corner_radius_row())` 后换肤必须重新跟随令牌，且序列化里该键消失；同一件对必填行（`clear(ButtonProps::label_row())`）必须编译失败，这一条是**编译判据**而非运行判据（对应未测项其四）。**基线值口径**：同一未表态实例上 `corner_radius()` 与 `corner_radius(裸主题 ctx)` 必须相等（明色基线，`Theme::with_defaults()` 不回退），但 `corner_radius(深色 ctx)` 必须不等 —— 这两条断言一起证明「无参读件不是生效值读件」。新增两 Scale 逐字段有声明处默认值。
8. **手写面归零（可 grep 度量）**：控件侧不再出现 `PropDescriptor` 字面量、`.enum_values` 手写展开（基线 102 处）、`.min_value` / `.max_value` 手写（基线 117 处）、序列化体内 `.has_value()` 判据（widget 头文件基线 173 处内属属性者）与 `"theme.` 字面量（基线 50 处）；`serialize_props` / `deserialize_props` 的控件侧函数体仅存在于有非属性序列化物（容器 children 等）的控件（基线 349 / 144 处逐处清点）；`describe_static` 里基类属性条目归零（基线 30 文件）；`collect_signals` 的控件侧收敛为一行转调（基线 62 处逐属性 push）；全库手写 `mark_needs_*` 剩余调用点逐处命中非属性写入登记清单（基线 411 处），未登记新调用点门禁红。
9. **构造面迁移判据**：全库（含 tests / examples / tools golden）不再出现 `XxxProps{` 形态（编译器指认 + grep 双证，基线 481 处 / 146 文件）；codegen / LSP / `generate_ui` 发射的构造形态改为链式，`DesignatedInit` 风格名与对应 golden、`codespec/manual-test/11-widget.md` 的手工判据同步重写；`itest_to_code`、`itest_cli`、`itest_mcp` 全绿；`--changed-only` 与全量 `ctest` 逐字节可复现（无生成物）。
10. **性能与门禁不回退项**：属性写入路径为「判等 + 写 + 一次既有 `mark_needs_*` 直调」，与现状同数操作、零堆分配、零新增间接；属性读取路径的新增项如实记为「一次判空 + 一次成员指针解引用（活令牌档）或一次静态默认提供者调用（常量档）」，与现状取值点的 `value_or` / `inherit_theme(ctx)` 同量级、不进分配器；全量可空化带来的 `sizeof(XxxProps)` 增长须在实施前实测并记录（列下方待测腿），若某控件的增长超出「每帧构造 DTO 可接受」的判定，须在第 6 条的框架内解决（该件改必填或合并属性行），**不得**回退成「部分行不给回落点」——那正是本轮要消灭的分叉；既有性能门禁（`check_perf_gates` 所辖口径）不回退；`check_props_table_coverage`、`check_widget_bind_pattern`、`check_no_optional_in_public_signature`、`check_no_contextless_property_read_in_render_path`（范围＝活令牌行）绿；公共面通用件白名单（`clear` 之外一个都不许出现，并入 `check_naming_conventions`）绿；`check_api_budget` 的口径以实测为据 —— 现量 206,097 字节 ≈ 51.5k tokens 对 75k 天花板，本轮净增只有 `source` 一位（约 +2 KB），任何把公共面名字总量或 schema 体积推近 60k tokens 的追加都必须先回到第 2 条的「无逐属性替代才升公共面」判据砍面，而不是抬预算；全量 `ctest --preset ninja-test` 绿，含 wasm 与 Linux 两腿、以及附录那组探针在两腿上的复跑记录；`check_api_schema_sync`（先跑 `--target aurora_api_json`）、`check_naming_conventions`、`check_api_budget`（字节与 token 口径不升）、`check_doc_comments`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_core_layer_boundary`、`check_arch_module_map`、`check_umbrella_header`、`check_platform_macros`、`check_manual_test_format`、`check_no_hardcoded_paths`、`check_no_cjk_literals` 全绿；`lint`（仅改动 TU）与 `format-check` 绿；每条新门禁须有变异注入自证非空转。
11. **手工用例实测回写**：`codespec/manual-test/11-widget.md`（构造形态判据）与 `codespec/manual-test/22-inspector.md`（属性读写与热回写）按改后实测输出重写，编号连续不变。
12. **可空性不外泄判据（含「封装类型不采纳」的反证）**：全部 639 属性的公共写件参数与公共读件返回值中 `std::optional` 零出现，属性封装类型（`Property` 一类）同样零出现（新门禁 grep + 全量编译双证）；`ReadOnlyState<std::optional<` 在 `include/aurora/**` 归零，其现状持有者（`Button::color` 一类）迁为 `std::optional<ReadOnlyState<...>>`；**双重空态不可构造**：对迁移后的来源型属性，既不存在「句柄缺失但值存在」也不存在「句柄存在但值为空」的合法组合，表态位与两个读件的对应关系由单测逐组合枚举证明（未表态 / 常量表态 / 源表态三态各一条，每态断言 `x(ctx)` 与 `x()` 的取值关系与库内 `present(row)` 一致）；**形态族计数的判据**：全库属性字段的可空形态只由 `required` 一位决定（非必填可空、必填不可空），出现任何第三种字段形状（如裸 `T` 却带回落、或 `optional` 却标 `.required()`）门禁即红。

## 回写落点

- `codespec/CODING_STANDARDS.md`（§11.1 需求陈述、代码示例与关键约束按下列可粘贴文案整段替换；§2 「顺序容错」条的「多参数构造优先用 `XxxProps{...}` 具名聚合」改为链式；§11.5 默认值可见性改述「默认值的真源由字段声明处移到行上的回落点 `.fallback(...)`（第 6 条），hover 与描述符仍可见（活令牌档出 `default_expr`、常量档出格式化值）；`defaults()` 实例经无参 getter 读到的是回落基线值而非生效值」；§5.3 响应式成员采集改述「来源型字段由表在控件侧一行转调中采集，`StateSink` 契约与 children 收集不动，`on_signal_dirty()` 粒度本轮不变」；§6.3 元编程边界补「`AbstractProps` 是控件的公有基类、零虚函数、CRTP 只用于读派生表，不新增 Widget 层次的 CRTP 用法」；§6.5 外观变更只标绘制的纪律改述为档位由表行给定；§6.6 常量属性化的标脏义务改为「建行即完成」；§13 注释形态补行工厂与访问器的 `@brief` / `@return` 要求）
- `codespec/specification/04-widget.md`（§2.4 脏标记与缓存：手写标脏规则改为「表行档位 + 经宿主链接落既有 `mark_needs_*`」；§2.5 双模 API 与 Props 约定：三形态改两形态、`*Props` 类化与私有字段、权威表与发射转调、链式构造、公共签名零 `std::optional`；§2.5.1 响应式属性与句柄权限：句柄唯一存储、换源即重订阅、反序列化写当前源不断绑、双句柄分叉消失、`ReadOnlyState<std::optional<T>>` 的句柄内可空形态退役；§4.1 主题回退与 §4.4 尺寸字号圆角可配：回落点强制（每行 `.fallback` 或 `.required`）、延迟解析、读面按回落点种类给形（`x()` 全行同形；`x(ctx)` 只给活令牌行）、撤表态是公共件 `clear(row)` 而不是逐属性 `reset_x()`、渲染路径内**活令牌行**禁用无参读件、未表态不序列化对全属性生效与几何令牌回落）
- `codespec/specification/02-state.md`（§7.1 细粒度信号状态模型：属性级刷新档的来源改为表行；新增「库内写通道」一节的边界说明——公共面不扩写权限）
- `codespec/specification/07-environment-modifier.md`（§5.1 主题值对象与角色集：新增 `ShapeScale` / `SpacingScale` 与令牌名对照表登记纪律；§5.4 作用域与换肤：`Provider` 口径不动的说明；§5 末 stringly 边界段补「属性侧同构：渲染路径只有成员指针解引用，字符串名只存在于表行与 schema 发射层」；§7.5、§8.1 属性默认值表示口径）
- `codespec/specification/08-tooling.md`（§2.5.1 发射管线的自描述数据源补「控件权威表」一环；§6 自描述发现 API 表；§10.3 机器可读 Schema 字段口径与 `DesignatedInit` 发射风格改为链式风格并给出两种输出的对照例）
- `codespec/specification/09-cli.md`（现文「字段顺序即指定初始化器书写顺序」改述为「`rows()` 点名顺序即 schema 顺序」，并保留声明序稳定的承诺）
- `codespec/specification/03-layout-render.md`（§5.1 缓存一致性不变量：标脏入口统一后「漏标即回放旧缓存」的成因与防线改述）
- `codespec/specification/01-core.md`、`codespec/CONCEPTS.md`、`codespec/GUIDELINE.md`（其中 `XxxProps{...}` 写法示例改链式；`codespec/SPECIFICATIONS.md` §5 对应模块条目口径同步）
- `codespec/ARCHITECTURE.md`（§4.4 组件层补新增声明头 include/aurora/widget/props.h 的 `AbstractProps` / 表行 / 擦除槽形态与 `Widget` 链接位；§14.6 门禁矩阵新增「权威表 ↔ 私有字段 ↔ 访问器三方覆盖」与「控件构造尾部 `bind`」两行；§15 变更落地流程记录本次回写）
- `codespec/BUILD_OPTIONS.md`（属性表无构建期步骤的口径：不新增生成目标、不改既有 `aurora_api_json` 生成链方向）
- `codespec/manual-test/11-widget.md`、`codespec/manual-test/22-inspector.md`（按验收判据第 9 / 11 条实测重写）

**§11.1 可直接粘贴的修订文案（形态收敛为两条）**：

> **需求陈述：** 同一控件必须提供两种**语义完全等价**的构建形态，AI 可按复杂度选择——① 链式 setter（对已构造对象逐属性写，简短与复杂组件通用）、② 链式 `*Props` 构造（`XxxProps().set_a(x).set_b(y)`，嵌套结构可分块生成）。等价性由「控件公有继承其 Props 类」保证：`class Xxx : public LeafWidget, public XxxProps`，属性的存储、读写件与权威表全在 `XxxProps` 类内，字段私有，**没有任何可直写的公有字段**。
>
> ```cpp
> // 形态一：链式 setter（写件返回引用）
> auto btn = au::Button();
> btn.set_label("OK").set_corner_radius(8.0F);
> btn.set_on_click(fn);                       // 回调仍是公有成员，逐名赋值
>
> // 形态二：链式 *Props 构造（漏写者回退该行回落点，第 6 条）
> auto btn2 = au::Button(au::ButtonProps().set_label("OK").set_corner_radius(8.0F));
>
> // 容器：子节点槽同样走具名写件
> auto col = au::Column(au::ColumnProps().set_children({au::Text("A"), au::Text("B")}).set_gap(8.0F));
> ```
>
> **关键约束：** ① `*Props` 是类而非聚合，**不得**再用 `XxxProps{ .a = x }` 指定初始化器写法；② 控件类同样不是聚合类型，`au::Button{ .label = ... }` 一律编译失败；③ 属性写入只有 `set_x(...)` 一条通道，`btn.x = v` 形式对属性不存在（字段私有，派生作用域亦不可达），回调成员与 `Column` / `Row` 的 `std::initializer_list<Node>` 构造不受此条约束；④ 链式写件返回引用，作为子节点放入 `children` 时必须 `std::move` 包裹（`Widget` 拷贝构造被删除，`Node` 仅移动派生对象）；⑤ 属性名与 `*Props` 名逐一对应，AI 只需记「属性名 + `*Props`」，无需记忆位置参数与顺序；⑥ 读写面的类型一律裸：写件收 `T` 或 `ReadOnlyState<T>`，读件返回 `T`，公共面不出现 `std::optional`、也不出现属性封装类型；⑦ 每个属性都有无参读件 `x()`（取表态值；未表态时取该行回落点的基线值），**只有回落点是主题档成员指针的属性另有 `x(ctx)`**，取当前作用域下的生效值——布局 / 绘制 / 命中路径内对这些属性必须写 `x(ctx)`，写无参件就等于取到不随换肤的值（该误用由门禁拦，不靠人记）；回落点是库内默认提供者的属性**没有 `x(ctx)` 这个名字**，二者恒等，直接写 `x()` 取到的就是生效值。⑧ 撤表态没有逐属性名字：调全库一份的公共件 `clear(row)`，写法 `props.clear(ButtonProps::color_row())`（行本来就是公开的名字，可 grep、可跳转）；必填属性（`label` 一类）传入它即编译失败。「用户是否表过态」不是公共方法（由 Inspector 与 schema 的位承担）。
>
> **验收标准：** 同一界面用两种形态各写一遍，序列化输出与运行时快照一致；`itest_ai_compat` 不出现因形态歧义导致的生成失败；任何 `XxxProps{` 写法在全库编译不过（迁移完成的判据）。

**CHANGELOG.md 破坏性条目口径（该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点，此处仅给可直接粘贴的文案；落在下一版本的 Breaking / Migration 子节，版本号取当时 currentVersion）**：

> - **Breaking**：`XxxProps` 由公有字段聚合改为私有字段类；属性的公有字段读写面（含 `XxxProps{ .a = … }` 指定初始化器与 `widget.<attr> = v` 直写）退出公共 API，写入面统一为 `set_<attr>(…)`，构造面统一为 `XxxProps().set_<attr>(…)` 链式写法。控件的 `*Props` 公有继承关系不变，故所有 `set_<attr>` 调用点仍直接写在控件上。
> - **Breaking**：来源型属性的换源写件（如 `set_background_source`、`background`）并入同名过载 `set_color(ReadOnlyState<…>)`；`defaults()` 返回的属性对象改为经 getter 读取。
> - **Breaking**：每个属性都必须有**回落点**（行上的 `.fallback(...)`，指向主题档成员或库内默认提供者），非必填属性的存储随之一律可空；`std::optional` 同时退出公共签名——来源型且跟随主题的属性字段由 `ReadOnlyState<std::optional<T>>` 改为 `std::optional<ReadOnlyState<T>>`（消除「句柄缺失」与「句柄内值为空」两重空态）。
> - **Breaking**：**跟随主题**（回落点是主题档成员指针）的属性，其生效值读件由 `resolve_<attr>(ctx)` 一类的独立词根改名为 `x(ctx)`，与无参 `x()`（表态值；未表态时取回落点基线值）同名成对；其余属性的读面只有 `x()` 一件，它同时就是生效值读件（该形态的回落点不随作用域变，二者恒等）。`has_<attr>()` 退出公共 API（表态位由 Inspector 与 schema 承担）。**撤表态没有逐属性新方法**：改为全库一份的公共件 `clear(row)`，写法 `props.clear(ButtonProps::color_row())`；必填属性（`Button::label` 一类）传入它编译失败。
> - **Breaking**：序列化落盘的是**意图**而非生效值——全部未表态的非必填属性不再写入 JSON 键（原先只有主题敏感属性享受该口径，其余属性把声明处默认值一并写盘）。往返语义不变：缺失键由该行回落点求值复原。
> - **Fixed**：一批只写字段、不标脏的链式 setter（`Button` 的 `set_corner_radius` / `set_padding` / `set_hover_color` / `set_pressed_color` / `set_border` / `set_disabled_colors` / `set_min_size` 与 `text_color`）改为按属性档位必然标脏；Inspector 单属性热回写由「可能刷新」变为「必然刷新」。
> - **Fixed**：`deserialize_props` 不再把来源型属性换绑成常量源——外部 `State<T>` 绑定在热回写后保持有效。
> - **Added**：`Theme` 新增 `ShapeScale`（圆角档）与 `SpacingScale`（间距档）令牌组；控件默认值不再硬编码在字段声明处，改为属性行上的回落点，其表示在描述符与 schema 中可求值（活令牌档出 `default_expr`，常量档出格式化值）。
> - **Migration**：① 全库 codemod 规则 `XxxProps{ .a = x, .b = y }` → `XxxProps().set_a(x).set_b(y)`（顺序无关，漏写者回退默认）；② `obj.<attr> = v` → `obj.set_<attr>(v)`；③ 读取属性的既有直引用法 → 同名 getter `x()`：**跟随主题的属性**在绘制 / 布局 / 命中测试内改为 `x(ctx)`（取当前作用域生效值，这也是门禁认可的形态），事件回调、单测与 `defaults()` 比对里用无参 `x()`（读到的是表态值 / 回落基线值，不随换肤）；**不跟随主题的属性一律用 `x()`**——它们没有 `x(ctx)` 这个名字，写出来就是编译错，而不是另一种取法；原按「`has_<attr>()` + `resolve_<attr>(ctx)`」口径写的代码改为直接 `x(ctx)`，需要让属性重新跟随主题或退回默认时改调 `clear(<属性>_row())`（无逐属性 `reset_<attr>()`）；需要「用户是否表过态」的判据改走 Inspector 属性面板或序列化输出（该位不再是公共方法）；④ 覆盖 `collect_signals` 的自定义控件保留 override，函数体改为一行 `collect_prop_sources(out)`；⑤ 自研控件若曾把 `*Props` 字段当公有成员在类内直写，须改为经写件/读件访问（这正是本条要拦的静默失效面）。
