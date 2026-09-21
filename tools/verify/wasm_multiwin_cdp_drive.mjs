// wasm_multiwin_cdp_drive.mjs — 用无头 Edge + CDP 驱动 `aurora_verify_wasm_multiwin` 探针的验收脚本。
//
// 为什么入库：浏览器探针是「无头 CI 无法证明的接线」的唯一证据面（见 cmake/AuroraVerify.cmake 类注释），
// 证据的可复现性要求驱动脚本本身也在仓里——否则判据只在某台机器的临时目录里成立过。
// 本脚本不进 CTest：需真实浏览器 + 网络服务 + 桌面 Edge，属人工/CI 专项通道，与探针本体同口径。
//
// 前置：先构建探针（WASM preset，产物含 .js/.wasm/.html 三件套）：
//   cmake --preset wasm && cmake --build build-wasm --target aurora_verify_wasm_multiwin
// 运行（**从仓库根**，脚本按 `build-wasm/` 定位产物）：
//   node tools/verify/wasm_multiwin_cdp_drive.mjs
// 退出码 0 = 全部判据绿；非 0 = 有 FAIL（逐条打印判据号与实测值）。
// 可用环境变量：AURORA_WASM_BUILD（产物目录，默认 build-wasm）、AURORA_EDGE（msedge.exe 路径）。
//
// 逐项判据对应 tools/verify/wasm_multiwin_live_probe.cpp 头注释的 1–10。
import {spawn} from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const ROOT = path.resolve(process.env.AURORA_WASM_BUILD || 'build-wasm');
const PORT = Number(process.env.AURORA_MW_PORT || 8765);
const DBG = Number(process.env.AURORA_MW_DBG_PORT || 9337);
const URL = `http://127.0.0.1:${PORT}/aurora_verify_wasm_multiwin.html`;
const EDGE = process.env.AURORA_EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const MIME = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm'};

let failures = 0;
const ok = (label, cond, extra = '') => {
    console.log(`${cond ? 'ok  ' : 'FAIL'}  ${label}${extra ? ' — ' + extra : ''}`);
    if (!cond) failures++;
};

const server = http.createServer((req, res) => {
    const f = path.join(ROOT, decodeURIComponent(req.url.split('?')[0]));
    fs.readFile(f, (err, buf) => {
        if (err) { res.writeHead(404); res.end('nf'); return; }
        res.writeHead(200, {'content-type': MIME[path.extname(f)] || 'application/octet-stream'});
        res.end(buf);
    });
});
await new Promise(r => server.listen(PORT, '127.0.0.1', r));

const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'mw-edge-'));
const browser = spawn(EDGE, [
    '--headless=new', `--remote-debugging-port=${DBG}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=1280,900', URL,
], {stdio: 'ignore'});

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
            if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') console.log('  [page-error]', m.params.entry.text.slice(0, 160));
            if (m.method === 'Runtime.exceptionThrown') {
                console.log('  [exception]', JSON.stringify(m.params.exceptionDetails).slice(0, 200));
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

const info = await json('/json/version');
const pageList = await json('/json/list');
const page = pageList.find(p => p.type === 'page' && p.url.includes('aurora_verify'));
if (!page) throw new Error('未找到探针页面目标');
const cdp = new Cdp(info.webSocketDebuggerUrl);
await cdp.connect();
const {sessionId} = await cdp.wait('Target.attachToTarget', {targetId: page.id, flatten: true});
const R = (method, params = {}) => cdp.wait(method, params, sessionId);
await R('Runtime.enable');
await R('Page.enable');
await R('Log.enable');

const evaluate = async expr => {
    const r = await R('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true});
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result ? r.result.value : undefined;
};
const state = () => evaluate('window.__mwState || ""');
const title = () => evaluate('document.title');
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function waitState(re, timeout = 8000) {
    const t0 = Date.now();
    for (;;) {
        const s = await state();
        if (re.test(s)) return s;
        if (Date.now() - t0 > timeout) throw new Error(`等待 ${re} 超时，最后状态: ${s}`);
        await sleep(120);
    }
}
const rect = async id => evaluate(
    `(()=>{const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();`
    + `return {l:r.left,t:r.top,rt:r.right,b:r.bottom};})()`);
async function click(x, y) {
    for (const type of ['mousePressed', 'mouseReleased']) {
        await R('Input.dispatchMouseEvent', {type, x, y, button: 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1});
    }
    await sleep(150);
}
async function key(k, text, vk) {
    for (const type of ['keyDown', 'keyUp']) {
        await R('Input.dispatchKeyEvent', {type, key: k, text, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk});
    }
    await sleep(80);
}
async function typeChar(c) { await key(c, c, c.toUpperCase().charCodeAt(0)); }
/// 逐次试点位，直到字符真的落进该窗口的输入框（证「点击命中 canvas + TextInput 获焦」）。
async function focusInput(winId, tag, probe) {
    const r = await rect(winId);
    for (const dy of [46, 62, 78, 94, 110, 126]) {
        await click(r.l + 90, r.t + dy);
        await typeChar(probe);
        try {
            await waitState(new RegExp(`${tag}\\[[^\\]]*${probe}`), 1500);
            return dy;
        } catch { /* 这点位没打中输入框，换下一个 */ }
    }
    throw new Error(`${tag} 输入框点位未命中`);
}
const cmd = async s => evaluate(`(window.__mwCmd||(window.__mwCmd=[])).push(${JSON.stringify(s)}); 1`);
const tok = (s, re) => { const m = s.match(re); return m ? m[1] : ''; };

try {
    // —— 期望 1（判据 ③）：末建窗口接管路由
    let s = await waitState(/foc=win-b/);
    ok('③ 初始路由焦点 = 末建窗口 win-b', /foc=win-b/.test(s), s.slice(0, 40));

    // —— 期望 6（判据 ⑥）：页面标题 = 焦点窗口标题（壳初值 mw-shell 应已被替换）
    ok('⑥ 初始 document.title == 焦点窗口标题 mw-B', (await title()) === 'mw-B', await title());
    ok('⑥ 非焦点窗口标题只在缓存 ta=mw-A', /ta=mw-A/.test(s) && /tb=mw-B/.test(s), `ta=${tok(s, /ta=(\S+)/)}`);

    // —— 期望 2（判据 ①④）：点击 win-a 输入区 → 路由切换 + 字符折算 + Enter 提交
    await focusInput('win-a', 'A', 'a');
    s = await waitState(/foc=win-a/);
    ok('① 点击 win-a 后路由焦点 = win-a', /foc=win-a/.test(s));
    ok('④ 字符键折算 TextInputEvent（A 出现 a）', /A\[[^\]]*a/.test(s), tok(s, /A\[[^\]]*\]/));
    await typeChar('b');
    await waitState(/A\[ab\//);
    ok('④ 连续键入拼出 "ab" 且 B 不动', /B\[\//.test(await state()));
    await key('Enter', '\r', 13);
    s = await waitState(/A\[ab\/1\]/);
    ok('① KeyEvent 通道 on_submit 计数 = 1', /A\[ab\/1\]/.test(s), tok(s, /A\[[^\]]*\]/));
    ok('⑥ 鼠标按下这一处标题跟随 → mw-A', (await title()) === 'mw-A', await title());

    // —— 期望 3（判据 ① 反向）：win-b 键入互不串台
    await focusInput('win-b', 'B', 'c');
    await typeChar('d');
    s = await waitState(/B\[cd\//);
    ok('① 点击并键入 win-b → B[cd] 且 A 不变', /A\[ab\/1\]/.test(s) && /foc=win-b/.test(s));
    ok('⑦ focus 切到 B 后标题 = mw-B', (await title()) === 'mw-B', await title());

    // —— 期望 4（判据 ②）：窗口缩放 → resize 广播到两实例
    const before = await rect('win-a');
    const bw = await json('/json/version');
    const bcdp = new Cdp(bw.webSocketDebuggerUrl);
    await bcdp.connect();
    const {windowId} = await bcdp.wait('Browser.getWindowForTarget', {targetId: page.id});
    await bcdp.wait('Browser.setWindowBounds', {windowId, bounds: {width: 1000, height: 900}});
    await sleep(600);
    const after = await rect('win-a');
    s = await state();
    ok('② resize 广播：两画布 CSS 宽随视口变化且探针尺寸跟到',
        after.l < before.l || Math.abs(after.rt - after.l - (before.rt - before.l)) > 4,
        `dom ${Math.round(before.rt - before.l)}→${Math.round(after.rt - after.l)}，state szA=${tok(s, /szA=(\S+)/)} szB=${tok(s, /szB=(\S+?)(?:\s|$)/)}`);
    ok('② 两窗口尺寸观测均非零', /szA=[1-9]/.test(s) && /szB=[1-9]/.test(s));

    // —— 期望 8/9（判据 ⑥⑦）：焦点窗口改名即时生效；非焦点改名只缓存、易主时重播
    await cmd('titleB:待切B');
    s = await waitState(/tb=待切B/);
    ok('⑥ 焦点窗口改名即时写页面标题', (await title()) === '待切B', await title());
    await cmd('titleA:改名A');
    await waitState(/ta=改名A/);
    ok('⑥ 非焦点窗口改名只进缓存，页面标题不动', (await title()) === '待切B' && /ta=改名A/.test(await state()),
        `title=${await title()} ta=${tok(await state(), /ta=(\S+)/)}`);
    await cmd('focusA');
    s = await waitState(/foc=win-a/);
    ok('⑦ focus_window() 易主 ⇒ 缓存重播（改名A）', (await title()) === '改名A' && /ta=改名A/.test(s), await title());

    // —— 期望 10（判据 ⑤）：raise() 改层叠，且不改激活状态
    s = await state();
    ok('⑤ raise 前：DOM 末位与交叠命中者同为 win-b', /order=win-a,win-b/.test(s) && /zTop=win-b\b/.test(s),
        `order=${tok(s, /order=(\S+)/)} zTop=${tok(s, /zTop=(\S+)/)}`);
    await cmd('raiseA');
    s = await waitState(/order=win-b,win-a/);
    ok('⑤ raise(win-a) → DOM 末位为 win-a', /order=win-b,win-a/.test(s), tok(s, /order=(\S+)/));
    ok('⑤ 交叠命中者随之变 win-a（真层叠，非仅字符串）', /zTop=win-a\b/.test(s), `zTop=${tok(s, /zTop=(\S+)/)}`);
    ok('⑤ raise 不改激活：foc 与页面标题一律不动',
        /foc=win-a/.test(s) && (await title()) === '改名A', `foc=${tok(s, /foc=(\S+)/)} title=${await title()}`);

    // —— 期望 10 后半（判据 ⑦ 第四处）：焦点窗口析构 → 路由回落 + 标题重播
    await cmd('focusB');
    await waitState(/foc=win-b/);
    await cmd('closeB');
    s = await waitState(/B\[closed\]/);
    ok('⑦ 关窗后路由回落 win-a', /foc=win-a/.test(s), `foc=${tok(s, /foc=(\S+)/)}`);
    ok('⑦ 关窗后页面标题 = 新焦点窗口缓存（无幽灵标题）', (await title()) === '改名A', await title());

    // —— 期望 5：状态串始终在推进（帧环活着）
    const s1 = await state();
    await sleep(350);
    ok('帧环活着（状态串可读且含命令回执）', /cmd=closeB/.test(s1), `cmd=${tok(s1, /cmd=(\S+)/)}`);
} catch (e) {
    console.log('FAIL  探针驱动异常 —', String(e).slice(0, 300));
    failures++;
} finally {
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} 项失败`);
    browser.kill();
    server.close();
}
process.exit(failures === 0 ? 0 : 1);
