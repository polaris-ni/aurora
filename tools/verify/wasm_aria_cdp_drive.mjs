// wasm_aria_cdp_drive.mjs — 用无头 Edge + CDP 驱动 `aurora_verify_wasm_aria` 探针的验收脚本。
//
// 为什么入库：浏览器无障碍镜像链路（镜像 DOM → AX 树 → 反向动作 → 播报）是「无头 CI 无法
// 证明的接线」的唯一证据面（见 cmake/AuroraVerify.cmake 类注释），证据的可复现性要求驱动
// 脚本本身也在仓里——否则判据只在某台机器的临时目录里成立过。
// 本脚本不进 CTest：需真实浏览器 + 本地 http + 桌面 Edge，与其余浏览器探针同口径。
//
// 前置：先构建探针（WASM preset，产物含 .js/.wasm/.html 三件套，须走 HTTP 打开）：
//   cmake --preset wasm && cmake --build build-wasm --target aurora_verify_wasm_aria
// 运行（**从仓库根**，脚本按 `build-wasm/` 定位产物）：
//   node tools/verify/wasm_aria_cdp_drive.mjs
// 退出码 0 = 全部判据绿；1 = 有 FAIL（逐条 RES|fail 行）；2 = 环境不可用（探针产物缺失 /
//         浏览器或 CDP 不可达）。逐条判定与 atspi 探针同格式：`RES|pass|<id>|<detail>`。
// 可用环境变量：AURORA_WASM_BUILD（产物目录，默认 build-wasm）、AURORA_EDGE（msedge.exe 路径）、
//              AURORA_AR_PORT / AURORA_AR_DBG_PORT（本地 http / CDP 端口）。
//
// 逐项判据对应 tools/verify/wasm_aria_live_probe.cpp 头注释的 1–8。
//
// 插桩口径（判据 7）：探针与桥均未内建 full/ops 计数器，脚本侧改用可达证据——经
// Page.addScriptToEvaluateOnNewDocument 注入属性访问器陷阱，截获 `window.auroraAriaLib`
// 赋值（先于桥的 aria_js_boot，零竞态）包装 applyFull/applyOps 计数，随后整页 reload 使
// 计数自零起算。full = applyFull 调用次数；ops = applyOps 载荷内**逐条 op 数**（非调用
// 次数——一次反向动作可携带多条更新：点「确定」同发按钮名 + 输入框值两条 update，
// 点 checkbox 一条 aria-checked update，播报走直发通道不产生 op ⇒ 期望恰 3 条）。
import {spawn} from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const ROOT = path.resolve(process.env.AURORA_WASM_BUILD || 'build-wasm');
const PORT = Number(process.env.AURORA_AR_PORT || 8766);
const DBG = Number(process.env.AURORA_AR_DBG_PORT || 9338);
const PAGE = 'aurora_verify_wasm_aria.html';
const URL = `http://127.0.0.1:${PORT}/${PAGE}`;
const EDGE = process.env.AURORA_EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const MIME = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm'};
// 镜像容器 id = "aurora-a11y-" + canvas_id（wasm_surface.h set_accessibility_root）；
// 镜像元素为容器内 div[data-aurora-id]，契约见 src/aurora/window/wasm_aria.cpp 与
// detail/aria_protocol.cpp：按钮名走 aria-label、Text/TextInput 值走正文、live 区带
// data-aurora-live（aria-live=polite）。
const MIRROR = '#aurora-a11y-win-a';

let failures = 0;
/// 逐条判定行（与 atspi 探针同格式，父脚本/人读均友好）。
const ok = (id, cond, detail = '') => {
    console.log(`RES|${cond ? 'pass' : 'fail'}|${id}|${String(detail).replace(/\n/g, ' ')}`);
    if (!cond) failures++;
};

// ---- 环境不可用走退出码 2：产物缺失先拦，浏览器启动失败即时退出 ----
if (!fs.existsSync(path.join(ROOT, PAGE))) {
    console.log(`ENV|探针产物缺失：${path.join(ROOT, PAGE)}（先构建 aurora_verify_wasm_aria）`);
    process.exit(2);
}

const server = http.createServer((req, res) => {
    const f = path.join(ROOT, decodeURIComponent(req.url.split('?')[0]));
    fs.readFile(f, (err, buf) => {
        if (err) { res.writeHead(404); res.end('nf'); return; }
        res.writeHead(200, {'content-type': MIME[path.extname(f)] || 'application/octet-stream'});
        res.end(buf);
    });
});
await new Promise(r => server.listen(PORT, '127.0.0.1', r));

const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'ar-edge-'));
const browser = spawn(EDGE, [
    '--headless=new', `--remote-debugging-port=${DBG}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=800,600', URL,
], {stdio: 'ignore'});
browser.on('error', e => {
    console.log('ENV|浏览器启动失败 —', String(e).slice(0, 200));
    process.exit(2);
});

const pageErrors = [];  // 期望 8：页面 error 级日志 / 未捕获异常流水（判据在断言段末尾读）

async function json(pathname) {
    for (let i = 0; i < 120; i++) {
        try {
            const r = await fetch(`http://127.0.0.1:${DBG}${pathname}`);
            if (r.ok) return await r.json();
        } catch { /* 端口未就绪 */ }
        await new Promise(r => setTimeout(r, 250));
    }
    throw new Error('CDP 未就绪');
}

class Cdp {
    constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); }
    async connect() {
        this.ws = new WebSocket(this.url);
        this.ws.addEventListener('message', e => {
            const m = JSON.parse(e.data);
            if (m.id && this.pending.has(m.id)) { this.pending.get(m.id)(m); this.pending.delete(m.id); }
            if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') {
                pageErrors.push(m.params.entry.text.slice(0, 160));
            }
            if (m.method === 'Runtime.exceptionThrown') {
                pageErrors.push(JSON.stringify(m.params.exceptionDetails).slice(0, 200));
            }
        });
        await new Promise(r => this.ws.addEventListener('open', r));
    }
    send(method, params = {}, session) {
        const id = ++this.id;
        const msg = {id, method, params};
        if (session) msg.sessionId = session;
        return new Promise(res => { this.pending.set(id, res); this.ws.send(JSON.stringify(msg)); });
    }
    async wait(method, params, session) {
        const r = await this.send(method, params, session);
        if (r.error) throw new Error(`${method}: ${JSON.stringify(r.error)}`);
        return r.result;
    }
}

let cdp = null;
let sessionId = null;
try {
    const info = await json('/json/version');
    const pageList = await json('/json/list');
    const page = pageList.find(p => p.type === 'page' && p.url.includes(PAGE));
    if (!page) throw new Error(`未找到探针页面目标（${PAGE}）`);
    cdp = new Cdp(info.webSocketDebuggerUrl);
    await cdp.connect();
    ({sessionId} = await cdp.wait('Target.attachToTarget', {targetId: page.id, flatten: true}));
} catch (e) {
    console.log('ENV|环境不可用 —', String(e).slice(0, 300));
    browser.kill();
    server.close();
    process.exit(2);
}
const R = (method, params = {}) => cdp.wait(method, params, sessionId);
await R('Runtime.enable');
await R('Page.enable');
await R('Log.enable');
await R('Accessibility.enable');  // AX 树断言面（期望 3）

const evaluate = async expr => {
    const r = await R('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true});
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result ? r.result.value : undefined;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const tok = (s, re) => { const m = s.match(re); return m ? m[1] : ''; };

/// 插桩注入源（判据 7）：访问器陷阱截获 auroraAriaLib 赋值并包装计数，先于 aria_js_boot。
const INSTRUMENT = `(function(){
    window.__ariaCounters = {full: 0, ops: 0};
    var lib = null;
    Object.defineProperty(window, 'auroraAriaLib', {
        configurable: true,
        get: function () { return lib; },
        set: function (v) {
            lib = v;
            var of = v.applyFull;
            var oo = v.applyOps;
            v.applyFull = function (cid, payload) {
                window.__ariaCounters.full += 1;
                return of.call(this, cid, payload);
            };
            v.applyOps = function (cid, payload) {
                window.__ariaCounters.ops += payload && payload.ops ? payload.ops.length : 0;
                return oo.call(this, cid, payload);
            };
        },
    });
})()`;

const title = () => evaluate('document.title');
async function waitTitle(re, timeout = 20000) {
    const t0 = Date.now();
    for (;;) {
        let t = '';
        try { t = await title(); } catch { /* 导航/上下文重建间隙，下一拍再取 */ }
        if (re.test(t)) return t;
        if (Date.now() - t0 > timeout) throw new Error(`等待标题 ${re} 超时，最后: ${t}`);
        await sleep(120);
    }
}

/// 镜像 DOM 一次性观测（按钮名在 aria-label、正文承载 value、live 区带 data-aurora-live）。
const domInfo = () => evaluate(`(() => {
    const c = document.getElementById('${MIRROR.slice(1)}');
    if (!c) return null;
    const q = s => c.querySelector(s);
    const sl = q('[role="slider"]');
    const live = q('[data-aurora-live]');
    return {
        buttons: Array.from(c.querySelectorAll('[role="button"]'))
            .map(b => b.getAttribute('aria-label') || b.textContent || ''),
        cb: q('[role="checkbox"]') ? q('[role="checkbox"]').getAttribute('aria-checked') : null,
        entry: q('[role="textbox"]') ? q('[role="textbox"]').textContent : null,
        slider: sl ? {min: sl.getAttribute('aria-valuemin'), max: sl.getAttribute('aria-valuemax'),
                      now: sl.getAttribute('aria-valuenow')} : null,
        liveText: live ? live.textContent : null,
        livePolite: live ? live.getAttribute('aria-live') : null,
        liveTarget: live ? (live.getAttribute('data-aurora-target') || '') : null,
    };
})()`);
async function waitDom(pred, timeout = 8000) {
    const t0 = Date.now();
    for (;;) {
        let d = null;
        try { d = await domInfo(); } catch { /* 帧间隙重试 */ }
        if (d && pred(d)) return d;
        if (Date.now() - t0 > timeout) throw new Error(`等待镜像 DOM 条件超时，最后: ${JSON.stringify(d)}`);
        await sleep(120);
    }
}

/// 反向动作注入面：镜像容器视觉隐藏（1×1 + clip），坐标点击打不中，须经 JS click() 走
/// 镜像元素 click 监听 → __auroraAriaQueue → 帧尾 pump_actions（探针 ② 通道）。
const mirrorClick = sel => evaluate('(() => { const el = document.querySelector(' + JSON.stringify(sel)
    + '); if (!el) throw new Error("mirror missing"); el.click(); return 1; })()');

// ---- AX 树观测（期望 3）：CDP Accessibility 域，节点角色/名字/属性统一解包 ----
const axRole = n => (n.role && n.role.value) || '';
const axName = n => (n.name && typeof n.name === 'object' ? n.name.value : n.name);
const axProp = (n, k) => {
    const p = (n.properties || []).find(x => x.name === k);
    if (!p) return undefined;
    const v = p.value;
    return v && typeof v === 'object' ? v.value : v;
};
async function waitAx(pred, timeout = 8000) {
    const t0 = Date.now();
    for (;;) {
        let nodes = [];
        try {
            nodes = (await R('Accessibility.getFullAXTree')).nodes.filter(n => !n.ignored);
        } catch { /* 树尚在构建，下一拍再取 */ }
        if (pred(nodes)) return nodes;
        if (Date.now() - t0 > timeout) throw new Error(`等待 AX 树条件超时（节点 ${nodes.length} 个）`);
        await sleep(150);
    }
}

try {
    // —— 插桩注入 + 整页重载：计数自零起算，全量/增量全程在监控下发生（探针判定与顺序无关，重载安全）
    await R('Page.addScriptToEvaluateOnNewDocument', {source: INSTRUMENT});
    await R('Page.reload');

    // —— 期望 1：标题帧发布（帧循环活、桥随首帧根注入诞生）
    const t1 = await waitTitle(/^aria /);
    ok('1 标题以 aria 开头且 c/a/cb/ev 字段在位', /^aria c=\d+ a=\d+ cb=[01] ev=/.test(t1), t1);

    // —— 期望 2：镜像 DOM 面（角色齐备 + 关键 aria 属性）
    const d2 = await waitDom(d => d !== null);
    ok('2 镜像容器内 button×2 且名为 确定/播报',
        d2.buttons.length === 2 && d2.buttons.includes('确定') && d2.buttons.includes('播报'),
        JSON.stringify(d2.buttons));
    ok('2 checkbox 带 aria-checked=false（未勾初值）', d2.cb === 'false', `aria-checked=${d2.cb}`);
    ok('2 textbox 正文承载初值 abc', d2.entry === 'abc', `entry=${d2.entry}`);
    ok('2 slider 带 aria-valuemin=0/max=100/now=25',
        !!d2.slider && d2.slider.min === '0' && d2.slider.max === '100' && d2.slider.now === '25',
        JSON.stringify(d2.slider));

    // —— 期望 3：CDP AX 树含全部角色节点（读屏视角的终局证据，①）
    const nodes = await waitAx(ns => ns.some(n => axRole(n) === 'button' && axName(n) === '确定'));
    const axBtnOk = nodes.find(n => axRole(n) === 'button' && axName(n) === '确定');
    const axBtnAnn = nodes.find(n => axRole(n) === 'button' && axName(n) === '播报');
    const axCb = nodes.find(n => axRole(n) === 'checkbox');
    const axTb = nodes.find(n => axRole(n) === 'textbox');
    const axSl = nodes.find(n => axRole(n) === 'slider');
    ok('3 AX 树含 button「确定」', !!axBtnOk, axBtnOk ? `nodeId=${axBtnOk.nodeId}` : '缺节点');
    ok('3 AX 树含 button「播报」', !!axBtnAnn, axBtnAnn ? `nodeId=${axBtnAnn.nodeId}` : '缺节点');
    const cbChecked = axCb ? axProp(axCb, 'checked') : undefined;
    ok('3 AX 树 checkbox 节点且带 checked 状态',
        !!axCb && (cbChecked === true || cbChecked === 'true'), `checked=${JSON.stringify(cbChecked)}`);
    ok('3 AX 树 textbox 节点', !!axTb, axTb ? `value=${JSON.stringify(axProp(axTb, 'value'))}` : '缺节点');
    ok('3 AX 树 slider 节点且带 value/min/max',
        !!axSl && [axProp(axSl, 'value'), axProp(axSl, 'min'), axProp(axSl, 'max')].every(v => v !== undefined)
            && Number(axProp(axSl, 'value')) === 25 && Number(axProp(axSl, 'min')) === 0
            && Number(axProp(axSl, 'max')) === 100,
        axSl ? `now=${axProp(axSl, 'value')} min=${axProp(axSl, 'min')} max=${axProp(axSl, 'max')}` : '缺节点');

    // —— 期望 4：点击「确定」镜像按钮 → Invoke 闭环（标题/按钮名/textbox 三面同源取证）
    await mirrorClick(MIRROR + ' [role="button"][aria-label="确定"]');
    const t4 = await waitTitle(/\bc=1\b/);
    ok('4 点击「确定」→ 标题 c=1', /\bc=1\b/.test(t4), t4);
    const d4 = await waitDom(d => d.buttons.includes('确定·1') && d.entry === 'clk1');
    ok('4 镜像按钮名变「确定·1」（一次动作两处状态变更）', d4.buttons.includes('确定·1'), JSON.stringify(d4.buttons));
    ok('4 textbox 值变 clk1（标题 ev 同步）', d4.entry === 'clk1', `entry=${d4.entry} ev=${tok(t4, /ev=(\S+)/)}`);

    // —— 期望 5：点击 checkbox 镜像元素 → Toggle 闭环
    await mirrorClick(MIRROR + ' [role="checkbox"]');
    const t5 = await waitTitle(/\bcb=1\b/);
    ok('5 点击 checkbox → 标题 cb=1', /\bcb=1\b/.test(t5), t5);
    const d5 = await waitDom(d => d.cb === 'true');
    ok('5 DOM aria-checked=true（on_changed 回写镜像）', d5.cb === 'true', `aria-checked=${d5.cb}`);

    // —— 期望 6：点击「播报」镜像按钮 → live 区直发（不经 diff；data-aurora-target 归属观测）
    await mirrorClick(MIRROR + ' [role="button"][aria-label="播报"]');
    const d6 = await waitDom(d => d.liveText === 'aurora-live-1');
    ok('6 live 区文本 aurora-live-1', d6.liveText === 'aurora-live-1', `text=${d6.liveText}`);
    ok('6 live 区带 aria-live=polite', d6.livePolite === 'polite', `aria-live=${d6.livePolite} target=${d6.liveTarget || '无'}`);

    // —— 期望 7：插桩计数（注入包装口径，见文件头）：首帧一次全量、其余皆增量
    await sleep(400);  // 让尾随 sync 全部收敛（桥的 rAF 链自驱）再读计数
    const cnt = await evaluate('window.__ariaCounters || {full: -1, ops: -1}');
    ok('7 full == 1（首帧一次全量，此后不再全量）', Number(cnt.full) === 1, JSON.stringify(cnt));
    ok('7 ops >= 3（按钮名+输入框值+勾选态全走增量 op）', Number(cnt.ops) >= 3, `ops=${cnt.ops}`);

    // —— 期望 8：控制台干净（未捕获异常 / Aborted 皆以 error 级日志或异常事件浮现）
    ok('8 控制台无未捕获异常 / Aborted', pageErrors.length === 0, pageErrors.join(' | ').slice(0, 220));
} catch (e) {
    console.log('RES|fail|driver|探针驱动异常 — ' + String(e).slice(0, 300).replace(/\n/g, ' '));
    failures++;
} finally {
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} 项失败`);
    browser.kill();
    server.close();
}
process.exit(failures === 0 ? 0 : 1);
