// Wasm ARIA 桥实现 —— 契约与差异申报见 `include/aurora/window/wasm_aria.h` 头注释；
// 折算与 JSON 载荷全部来自中立层 `aria_protocol`（本文件只剩 DOM 粘合与生命周期）。

#include "aurora/window/wasm_aria.h"

#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)

#include <emscripten.h>
#include <emscripten/html5.h>  // emscripten_request_animation_frame（自驱拍蹦床）

#include <algorithm>
#include <ranges>
#include <utility>

#include "aurora/core/accessibility.h"
#include "aurora/widget/widget.h"
#include "aurora/window/detail/aria_protocol.h"

namespace aurora {

namespace {

// EM_JS 形参为具名 C/C++ 参数（无 `$` 占位符），不触发 -Wdollar-in-identifier-extension。
// 页面级共享 JS 库：容器/镜像元素的建-改-删与反向动作队列。只定义一次（boot 幂等），
// 各 EM_JS 薄壳按容器 id 调库方法 —— 多窗口各持独立镜像容器，互不串树。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, aria_js_boot, (), {
    if (window.auroraAriaLib) {
        return;
    }
    window.auroraAriaLib = {
        // ---- 反向动作队列（JS 写、C++ 帧尾排空；导出函数零链接要求） ----
        pushAction: function (id, action) {
            (window.__auroraAriaQueue || (window.__auroraAriaQueue = [])).push([id, action]);
        },
        // ---- 镜像容器：视觉隐藏（clip）但保留于可访问性树 —— display:none 会整树出局 ----
        ensure: function (cid) {
            let c = document.getElementById(cid);
            if (!c) {
                c = document.createElement('div');
                c.id = cid;
                document.body.appendChild(c);
            }
            c.style.position = 'absolute';
            c.style.top = '0px';
            c.style.left = '0px';
            c.style.width = '1px';
            c.style.height = '1px';
            c.style.overflow = 'hidden';
            c.style.clipPath = 'inset(50%)';
            c.style.whiteSpace = 'nowrap';
            c.style.border = '0';
            c.setAttribute('role', 'group');
            c.tabIndex = -1;
            return c;
        },
        live: function (cid) {
            const c = this.ensure(cid);
            let l = c.querySelector('[data-aurora-live]');
            if (!l) {
                l = document.createElement('div');
                l.setAttribute('data-aurora-live', '1');
                l.setAttribute('aria-live', 'polite');
                l.setAttribute('aria-atomic', 'true');
                c.appendChild(l);
            }
            return l;
        },
        // ---- 镜像元素：spec = 折算层 JSON 载荷的一行（role/attrs/content/click） ----
        elFor: function (spec) {
            const d = document.createElement('div');
            // DOM id 必挂（`aurora-a11y-<id>`）：aria-activedescendant 是 IDREF，容器上的
            // 焦点宣告按此 id 指回元素（applyFull/applyOps 经 el.id 取用，缺挂即焦点面失效）。
            if (spec.dom) {
                d.id = spec.dom;
            }
            if (spec.role) {
                d.setAttribute('role', spec.role);
            }
            for (const a of spec.attrs) {
                d.setAttribute(a[0], a[1]);
            }
            if (spec.content) {
                d.textContent = spec.content;
            }
            if (spec.click) {
                d.addEventListener('click', () => {
                    window.auroraAriaLib.pushAction(spec.id, spec.click);
                });
            }
            d.addEventListener('focus', () => {
                window.auroraAriaLib.pushAction(spec.id, 1 /* Focus */);
            });
            return d;
        },
        byId: function (cid, id) {
            return document.querySelector('#' + cid + ' [data-aurora-id="' + id + '"]');
        },
        // index 口径 = 带 data-aurora-id 的子元素序（播报 live 区不占位）。
        insertAt: function (parent, el, index) {
            const kids = Array.from(parent.children).filter(
                (n) => n.hasAttribute && n.hasAttribute('data-aurora-id'));
            if (index >= kids.length) {
                parent.appendChild(el);
            } else {
                parent.insertBefore(el, kids[index]);
            }
        },
        setFocus: function (cid, domid) {
            const c = this.ensure(cid);
            const old = c.querySelector('[data-aurora-focused="1"]');
            if (old) {
                old.removeAttribute('data-aurora-focused');
            }
            c.removeAttribute('aria-activedescendant');
            if (domid) {
                const el = document.getElementById(domid);
                if (el) {
                    el.setAttribute('data-aurora-focused', '1');
                    c.setAttribute('aria-activedescendant', domid);
                }
            }
        },
        applyFull: function (cid, payload) {
            const c = this.ensure(cid);
            for (const n of Array.from(c.childNodes)) {
                if (!(n.getAttribute && n.getAttribute('data-aurora-live'))) {
                    c.removeChild(n);
                }
            }
            c.setAttribute('dir', payload.rtl ? 'rtl' : 'ltr');
            const map = new Map();
            for (const spec of payload.els) {
                map.set(spec.id, this.elFor(spec));
            }
            // 先序表：父先于子，逐个挂接即可保 reading order。
            for (const spec of payload.els) {
                const parent = spec.parent ? (map.get(spec.parent) || c) : c;
                parent.appendChild(map.get(spec.id));
            }
            const fel = payload.focus ? map.get(payload.focus) : null;
            this.setFocus(cid, fel ? fel.id : null);
        },
        applyOps: function (cid, payload) {
            for (const op of payload.ops) {
                if (op.op === 'remove') {
                    const n = this.byId(cid, op.id);
                    if (n) {
                        n.remove();
                    }
                } else if (op.op === 'add') {
                    const parent = op.el.parent ? this.byId(cid, op.el.parent) : null;
                    this.insertAt(parent || this.ensure(cid), this.elFor(op.el), op.index);
                } else if (op.op === 'move') {
                    const n = this.byId(cid, op.id);
                    if (n) {
                        const parent = op.parent ? this.byId(cid, op.parent) : this.ensure(cid);
                        this.insertAt(parent, n, op.index);
                    }
                } else if (op.op === 'update') {
                    const old = this.byId(cid, op.el.id);
                    if (old) {
                        const d = this.elFor(op.el);
                        for (const kid of Array.from(old.children)) {
                            if (kid.hasAttribute('data-aurora-id')) {
                                d.appendChild(kid);
                            }
                        }
                        old.replaceWith(d);
                    }
                }
            }
            const fel = payload.focus ? this.byId(cid, payload.focus) : null;
            this.setFocus(cid, fel ? fel.id : null);
        },
        // 入参已是解析后的对象（与 applyFull/applyOps 同口径：薄壳 JSON.parse 一次，此处
        // 若再 parse 即对对象二次字符串化 ⇒ SyntaxError）。
        announce: function (cid, p) {
            const l = this.live(cid);
            // 同文本重复不触发重读：先清空、下一宏任务回写（polite 队列容忍此间隙）。
            l.textContent = "";
            if (p.target) {
                l.setAttribute('data-aurora-target', String(p.target));
            } else {
                l.removeAttribute('data-aurora-target');
            }
            setTimeout(() => {
                l.textContent = p.text;
            }, 0);
        },
        clear: function (cid) {
            const c = document.getElementById(cid);
            if (c) {
                c.remove();
            }
        },
    };
});
// clang-format on

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, aria_js_apply_full, (const char *cid, const char *json), {
    window.auroraAriaLib.applyFull(UTF8ToString(cid), JSON.parse(UTF8ToString(json)));
});
// clang-format on

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, aria_js_apply_ops, (const char *cid, const char *json), {
    window.auroraAriaLib.applyOps(UTF8ToString(cid), JSON.parse(UTF8ToString(json)));
});
// clang-format on

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, aria_js_announce, (const char *cid, const char *json), {
    window.auroraAriaLib.announce(UTF8ToString(cid), JSON.parse(UTF8ToString(json)));
});
// clang-format on

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, aria_js_clear, (const char *cid), { window.auroraAriaLib.clear(UTF8ToString(cid)); });
// clang-format on

// 反向动作队列排水：id 以 double 承载（runtime_id 为进程内小计数器，< 2^53 恒精确）；
// -1 = 队列空。动作位在 pop_id 时弹出并存于 __auroraAriaCur，随后 pop_action 取回。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(double, aria_js_pop_id, (), {
    const q = window.__auroraAriaQueue;
    if (!q || !q.length) {
        return -1;
    }
    const p = q.shift();
    window.__auroraAriaCur = p;
    return p[0];
});
// clang-format on

// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, aria_js_pop_action, (), {
    return window.__auroraAriaCur ? window.__auroraAriaCur[1] : 0;
});
// clang-format on

}  // namespace

auto WasmAriaBridge::live_bridges() -> std::vector<WasmAriaBridge *> & {
    static std::vector<WasmAriaBridge *> bridges;  // NOLINT
    return bridges;
}

WasmAriaBridge::WasmAriaBridge(std::string container_id) : container_id_(std::move(container_id)) {
    live_bridges().push_back(this);
}

WasmAriaBridge::~WasmAriaBridge() {
    deactivate();
    auto &bridges = live_bridges();
    bridges.erase(std::ranges::remove(bridges, this).begin(), bridges.end());
}

auto WasmAriaBridge::activate() -> void {
    if (active_) {
        return;
    }
    active_ = true;
    has_snapshot_ = false;  // 重新激活 ⇒ 下次同步必走全量
    dirty_ = true;
    aria_js_boot();
    a11y::register_provider(*this);
    // 「读屏在线」启发式（设计 R9）：浏览器无读屏探测面，桥激活 = 镜像面对页面恒在线。
    current_accessibility_settings().screen_reader_active = true;
    // 自驱拍上线：present() 帧尾只在脏帧跑，静止页面的反向动作/播报/增量同步会饿死，
    // 故同步与排水由本桥自己的 rAF 链承担（去激活时链自然出局，见 raf_tick）。
    if (!raf_pending_) {
        emscripten_request_animation_frame(&WasmAriaBridge::raf_tick, this);
        raf_pending_ = true;
    }
}

auto WasmAriaBridge::deactivate() -> void {
    if (!active_) {
        return;
    }
    active_ = false;
    a11y::unregister_provider(*this);
    aria_js_clear(container_id_.c_str());
    root_ = nullptr;
    snapshot_ = {};
    has_snapshot_ = false;
    dirty_ = true;
}

auto WasmAriaBridge::set_root(Widget *root) -> void {
    if (root == nullptr || root == root_) {
        return;  // 幂等：同根/空根重复注入不置脏（`present_root` 每帧调用）
    }
    root_ = root;
    if (!active_) {
        activate();  // 首个根 = 本桥激活信号（无懒探测面，头注释 D14 例外申报）
    }
    dirty_ = true;
}

auto WasmAriaBridge::set_rtl(const bool rtl) -> void {
    if (rtl_ == rtl) {
        return;
    }
    rtl_ = rtl;
    has_snapshot_ = false;  // dir 写在容器上、随全量载荷生效 ⇒ 升级为全量重放
    dirty_ = true;
}

auto WasmAriaBridge::sync_if_dirty() -> void {
    if (!active_ || !dirty_) {
        return;
    }
    dirty_ = false;
    if (root_ == nullptr) {
        // 根已切断（销毁先于换根）：清空镜像容器，等待重新注入。
        if (has_snapshot_) {
            aria_js_clear(container_id_.c_str());
            snapshot_ = {};
            has_snapshot_ = false;
        }
        return;
    }
    rebuild_and_apply();
}

auto WasmAriaBridge::pump_actions() -> void {
    if (!active_) {
        return;
    }
    bool performed = false;
    for (;;) {
        const double raw = aria_js_pop_id();
        if (raw < 0.0) {
            break;
        }
        const int action = aria_js_pop_action();
        const auto id = static_cast<std::uint64_t>(raw);
        // 跨窗口寻址：runtime_id 进程内唯一，遍历存活桥找持有该 id 的活快照即可。
        for (WasmAriaBridge *bridge : live_bridges()) {
            Widget *w = bridge->widget_of_id(id);
            if (w == nullptr) {
                continue;
            }
            // 镜像面只表达离散动作（点击/聚焦）：Value 的文本/数值入参留默认，
            // 富文本注入不在本期映射面（申报见头注释）。
            const AccessibilityActionRequest req{
                .action = static_cast<AccessibilityAction>(static_cast<std::uint16_t>(action))};
            if (w->perform_accessibility_action(req)) {
                performed = true;
            }
            break;
        }
    }
    // 动作成功 ⇒ 控件态可能已变而其变更不经 a11y 广播（如 FocusManager 收焦点后仅
    // FocusChanged 置脏、被本拍开头的 sync 消费；或 perform 走无事件路径）。强制补一次
    // 脏位，由 raf_tick 的尾随 sync 在同一拍收敛镜像——否则焦点/动作位回写要等下一拍，
    // 静止页面里甚至饿到下一次外部事件（WASM 真机验收实测坐实）。diff 为空则零成本。
    if (performed) {
        dirty_ = true;
    }
}

auto WasmAriaBridge::on_announcement(const std::string &text, const Widget *target) -> void {
    if (!active_ || text.empty()) {
        return;
    }
    // target 可空/不在树内（toast 常态）：解析得 0 即纯文本播报，归属仅入镜像观测属性。
    const std::string payload = detail::aria_announce_json(text, id_of_widget(target));
    aria_js_announce(container_id_.c_str(), payload.c_str());
}

auto WasmAriaBridge::on_widget_destroying(const Widget *w) -> void {
    if (w != nullptr && w == root_) {
        // 只有一种情形需要处理：**被销毁的是当前根**（子节点生死由下次重投影自然收敛）。
        // 本帧内不得清空快照（正被遍历者可能引用）；切断根、置脏，下次同步统一清算。
        root_ = nullptr;
        dirty_ = true;
    }
}

auto WasmAriaBridge::id_of_widget(const Widget *w) const -> std::uint64_t {
    if (w == nullptr) {
        return 0;
    }
    for (const a11y::NodeSnapshot &n : snapshot_.flat) {
        if (n.widget == w) {
            return n.id;
        }
    }
    return 0;
}

auto WasmAriaBridge::widget_of_id(const std::uint64_t id) const -> Widget * {
    const a11y::NodeSnapshot *n = snapshot_.find(id);
    if (n == nullptr) {
        return nullptr;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast): 快照按 const 视图存 widget，而
    // perform_accessibility_action 是非 const 回调，桥边界取回活指针必须脱锥——AT-SPI2 /
    // UIA 两桥同款做法。安全性由时序保证：动作在帧尾排水处派发，事件环内无并发，快照里的
    // widget 就是主线程当帧还活着的同一对象。
    return const_cast<Widget *>(n->widget);
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
}

auto WasmAriaBridge::raf_tick(double /*time*/, void *user_data) -> bool {
    auto *self = static_cast<WasmAriaBridge *>(user_data);
    auto &bridges = live_bridges();
    if (std::ranges::find(bridges, self) == bridges.end()) {
        return false;  // 实例已析构（dtor 先 deactivate 再出表）：不触碰 userData，本拍出局
    }
    self->raf_pending_ = false;
    if (!self->active_) {
        return false;  // 已去激活：断链不再续订（重新 activate 时重排）
    }
    self->sync_if_dirty();
    self->pump_actions();
    self->sync_if_dirty();  // 动作回灌的态变更（pump 内补脏）同拍收敛，镜像零拍延迟
    emscripten_request_animation_frame(&WasmAriaBridge::raf_tick, self);
    self->raf_pending_ = true;
    return true;
}

auto WasmAriaBridge::rebuild_and_apply() -> void {
    a11y::TreeSnapshot fresh = a11y::build_tree_snapshot(*root_);
    if (!has_snapshot_) {
        const std::vector<detail::AriaElement> tree = detail::aria_tree_of(fresh);
        const std::string json = detail::aria_full_json(tree, detail::focus_id_of(fresh), rtl_);
        aria_js_apply_full(container_id_.c_str(), json.c_str());
    } else {
        const a11y::TreeDiff diff = a11y::diff_snapshots(snapshot_, fresh);
        if (!diff.empty()) {
            const std::string ops = detail::aria_ops_json(snapshot_, fresh, diff);
            aria_js_apply_ops(container_id_.c_str(), ops.c_str());
        }
    }
    snapshot_ = std::move(fresh);
    has_snapshot_ = true;
}

}  // namespace aurora

#endif  // AURORA_PLATFORM_WASM && AURORA_BACKEND_WASM
