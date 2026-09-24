// wasm_input_cdp_drive.mjs — 用无头 Edge + CDP `Input.dispatchKeyEvent` 驱动
// `aurora_verify_wasm_aria` 探针的键盘输入验收脚本。
//
// 为什么入库：键盘真机链路（真实浏览器键盘事件 → DOM KeyboardEvent → Emscripten document
// keydown → `WasmSurface` 折算 → `EventDispatcher` Tab/激活快捷键 → 控件状态 → ARIA 镜像投影）
// 是「无头单测无法证明的接线」——Emscripten 键盘回调依赖真实 DOM 事件环（node 驱动下注册
// 即抛 JS 异常，见 tests 对 WasmSurface 的运行时探测）。证据可复现性要求驱动脚本在仓里，
// 与 wasm_aria_cdp_drive.mjs 同口径。
//
// 为什么走键盘而非坐标鼠标：镜像容器视觉隐藏（1×1 + clip）无几何，而 aurora 场景内控件
// 几何活在 wasm 堆里、DOM 侧不可观测——Tab 序是唯一确定的目标寻址面（焦点态经「激活后
// 状态变化」回读）。桌面侧 SendInput/PostMessage/XTest 通道见 tests/e2e/etest_os_input.cpp；
// 本脚本补齐 WASM 一侧（OS 无输入系统，浏览器即宿主，CDP Input 域即「OS 级注入」）。
//
// 已知库层缺口（不设断言）：`Checkbox` 未覆写 `activate()`，Enter/Space 对焦点复选框无
// 效果——键盘切换复选框暂不支持，属后续库层增强项。
//
// 前置：先构建探针（WASM preset，产物含 .js/.wasm/.html 三件套，须走 HTTP 打开）：
//   cmake --preset wasm && cmake --build build-wasm --target aurora_verify_wasm_aria
// 运行（**从仓库根**，脚本按 `build-wasm/` 定位产物）：
//   node tools/verify/wasm_input_cdp_drive.mjs
// 退出码 0 = 全部判据绿；1 = 有 FAIL（逐条 RES|fail 行）；2 = 环境不可用（探针产物缺失 /
//         浏览器或 CDP 不可达）。逐条判定与 aria 驱动同格式：`RES|pass|<id>|<detail>`。
// 可用环境变量：AURORA_WASM_BUILD（产物目录，默认 build-wasm）、AURORA_EDGE（msedge.exe 路径）、
//              AURORA_KI_PORT / AURORA_KI_DBG_PORT（本地 http / CDP 端口）。
//
// 断言依据：
//   - 焦点序 = pre-order + tab_index 稳定排序，且 `Widget::focusable_` 默认 true——根 Column
//     容器（Generic）与纯展示 Text 也在序内：Tab1=根容器、Tab2=标题 Text（activate 均无
//     操作），Tab3 才到「确定」按钮（插桩实测坐实后写入，见判据 3 注释）；
//   - TextInput 聚焦自 Tab 序（无点击定位 ⇒ caret_ 停在 0——判据 3 的 set_value("clk1")
//     亦把 caret 重置回 0）→ 字符头插：打 "a1" ⇒ "a1clk1"（确定性断言）；
//   - Shift+Tab 反向回绕 2 步（TextInput→播报→确定）后 Enter ⇒ 二次激活（clicks=2）。
import {spawn} from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const ROOT = path.resolve(process.env.AURORA_WASM_BUILD || 'build-wasm');
const PORT = Number(process.env.AURORA_KI_PORT || 8768);
const DBG = Number(process.env.AURORA_KI_DBG_PORT || 9340);
const PAGE = 'aurora_verify_wasm_aria.html';
const URL = `http://127.0.0.1:${PORT}/${PAGE}`;
const EDGE = process.env.AURORA_EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const MIME = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm'};
// 镜像容器 id = "aurora-a11y-" + canvas_id（wasm_surface.h set_accessibility_root）。
// 按钮名在 aria-label、TextInput 值走正文——与 wasm_aria_cdp_drive.mjs 同一读取口径。
const MIRROR = '#aurora-a11y-win-a';

let failures = 0;
/// 逐条判定行（与 aria 驱动同格式，父脚本/人读均友好）。
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
    if (req.url.startsWith('/favicon.ico')) {
        // headless Edge 照常请求 favicon：204 免 404 污染 Log.entryAdded（判据 7 的控制台干净面）
        res.writeHead(204);
        res.end();
        return;
    }
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

const pageErrors = [];  // 页面 error 级日志 / 未捕获异常流水（末位判据读取）

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

const evaluate = async expr => {
    const r = await R('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true});
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result ? r.result.value : undefined;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

/// 真实键盘注入：CDP Input 域派发可信键盘事件（走浏览器完整输入管线，非 JS 合成事件）。
/// modifiers 位：Alt=1 / Ctrl=2 / Meta=4 / Shift=8（CDP 规范）。
const key = (desc) => R('Input.dispatchKeyEvent', desc);
const tabKey = (shift = false) => ({
    type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9, nativeVirtualKeyCode: 9,
    ...(shift ? {modifiers: 8} : {}),
});
const enterKey = () => ({
    type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13,
});
const charKey = (ch, code, vk) => ({
    type: 'keyDown', key: ch, code, text: ch, unmodifiedText: ch,
    windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk,
});
const upOf = d => ({type: 'keyUp', key: d.key, code: d.code, windowsVirtualKeyCode: d.windowsVirtualKeyCode});
async function pressKey(desc) {
    await key(desc);
    await key(upOf(desc));
}

/// 镜像 DOM 一次性观测（与 wasm_aria_cdp_drive.mjs 的 domInfo 同一读取口径）。
const domInfo = () => evaluate(`(() => {
    const c = document.getElementById('${MIRROR.slice(1)}');
    if (!c) return null;
    const q = s => c.querySelector(s);
    return {
        buttons: Array.from(c.querySelectorAll('[role="button"]'))
            .map(b => b.getAttribute('aria-label') || b.textContent || ''),
        cb: q('[role="checkbox"]') ? q('[role="checkbox"]').getAttribute('aria-checked') : null,
        entry: q('[role="textbox"]') ? q('[role="textbox"]').textContent : null,
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

/// canvas 像素采样指纹：渲染走「脏区决策」——空闲不重绘，同态采样恒稳；状态变化必有像素差。
/// 抽样步长 397 的线性哈希足够区分「有变化 / 无变化」，避免整帧 RGBA 过 JSON 桥。
const pxHash = () => evaluate(`(() => {
    const c = document.getElementById('win-a');
    if (!c) return -1;
    const ctx = c.getContext('2d');
    if (!ctx) return -1;
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let h = 0;
    for (let i = 0; i < d.length; i += 397) { h = ((h * 31) + d[i]) | 0; }
    return h;
})()`);

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

try {
    // —— 判据 1：帧循环活 + 镜像就绪（桥随首帧根注入诞生）
    const t1 = await waitTitle(/^aria /);
    ok('1 标题以 aria 开头（帧循环活）', /^aria c=\d+ a=\d+ cb=[01] ev=/.test(t1), t1);
    const d1 = await waitDom(d => d !== null && d.buttons.includes('确定') && d.buttons.includes('播报'));
    ok('1 镜像就绪（button 确定/播报 在位）', !!d1, JSON.stringify(d1 && d1.buttons));

    // —— 判据 2：键盘注入前初态
    const d2 = await waitDom(d => d.entry === 'abc' && d.cb === 'false');
    ok('2 初态：btn0=确定 / entry=abc / cb=false',
        d2.buttons[0] === '确定' && d2.entry === 'abc' && d2.cb === 'false',
        JSON.stringify({btn0: d2.buttons[0], entry: d2.entry, cb: d2.cb}));
    const hash0 = await pxHash();
    ok('2 canvas 像素采样可用', Number.isInteger(hash0) && hash0 !== -1, `hash=${hash0}`);

    // —— 判据 3：Tab 聚焦「确定」+ Enter 激活（键盘导航与激活快捷键真机闭环）
    // 焦点序注意：`Widget::focusable_` 默认 true——根 Column 容器（Generic）与纯展示 Text
    // 也在 Tab 序内（pre-order + tab_index 稳定排序）：Tab1=根容器、Tab2=标题 Text（activate
    // 均无操作）、Tab3 才到首个交互控件「确定」（插桩实测坐实后写入，见判据 3 注释）。
    await pressKey(tabKey());
    await pressKey(tabKey());
    await pressKey(tabKey());
    await pressKey(enterKey());
    const d3 = await waitDom(d => d.buttons.includes('确定·1') && d.entry === 'clk1');
    ok('3 Tab+Enter → 「确定·1」（on_click 真执行）', d3.buttons.includes('确定·1'), JSON.stringify(d3.buttons));
    ok('3 同一激活联动 textbox=clk1（一次动作两处状态变更）', d3.entry === 'clk1', `entry=${d3.entry}`);

    // —— 判据 4：Tab×2 至 TextInput + 字符输入（caret 头插 ⇒ a1clk1，见文件头断言依据）
    await pressKey(tabKey());
    await pressKey(tabKey());
    await pressKey(charKey('a', 'KeyA', 65));
    await pressKey(charKey('1', 'Digit1', 49));
    const d4 = await waitDom(d => d.entry === 'a1clk1');
    ok('4 键盘字符入框 a1clk1（DOM→WasmSurface→TextInput 全链）', d4.entry === 'a1clk1', `entry=${d4.entry}`);

    // —— 判据 5：Shift+Tab 反向回绕 2 步回「确定」+ 二次激活
    await pressKey(tabKey(true));
    await pressKey(tabKey(true));
    await pressKey(enterKey());
    const d5 = await waitDom(d => d.buttons.includes('确定·2') && d.entry === 'clk2');
    ok('5 Shift+Tab 回绕+Enter → 「确定·2」（反向导航真机闭环）',
        d5.buttons.includes('确定·2'), JSON.stringify(d5.buttons));
    ok('5 二次激活联动 textbox=clk2（set_value 整值替换）', d5.entry === 'clk2', `entry=${d5.entry}`);

    // —— 判据 6：canvas 像素变化（真实渲染管线跟随输入状态变化的终局证据）
    const hash1 = await pxHash();
    ok('6 输入前后 canvas 像素指纹变化', Number.isInteger(hash1) && hash1 !== hash0,
        `before=${hash0} after=${hash1}`);

    // —— 判据 7：控制台干净（未捕获异常 / Aborted 皆以 error 级日志或异常事件浮现）
    ok('7 控制台无未捕获异常 / Aborted', pageErrors.length === 0, pageErrors.join(' | ').slice(0, 220));
} catch (e) {
    console.log('RES|fail|driver|探针驱动异常 — ' + String(e).slice(0, 300).replace(/\n/g, ' '));
    failures++;
} finally {
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} 项失败`);
    browser.kill();
    server.close();
}
process.exit(failures === 0 ? 0 : 1);
