// wasm_audio_cdp_drive.mjs — 用无头 Edge + CDP 驱动 `aurora_verify_wasm_audio` 探针的验收脚本。
//
// 为什么入库：浏览器音频链路是「无头 CI 无法证明的接线」的唯一证据面（见 cmake/AuroraVerify.cmake
// 类注释），证据的可复现性要求驱动脚本本身也在仓里——否则判据只在某台机器的临时目录里成立过。
// 本脚本不进 CTest：需真实浏览器 + 本地 http + 桌面 Edge，与其余浏览器探针同口径。
//
// 前置：先构建探针（WASM preset + 音频特性）：
//   cmake --preset wasm -DAURORA_ENABLE_AUDIO=ON
//   cmake --build build-wasm --target aurora_verify_wasm_audio
// 运行（**从仓库根**，脚本按 `build-wasm/` 定位产物）：
//   node tools/verify/wasm_audio_cdp_drive.mjs
// 退出码 0 = 全部判据绿；非 0 = 有 FAIL（逐条打印判据号与实测值）。
// 可用环境变量：AURORA_WASM_BUILD（产物目录）、AURORA_EDGE（msedge.exe 路径）、
//              AURORA_AU_PORT / AURORA_AU_DBG_PORT（本地 http / CDP 端口）。
//
// 逐项判据对应 tools/verify/wasm_audio_live_probe.cpp 头注释的 ①–④（另加信号旁证）。
import {spawn} from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const ROOT = path.resolve(process.env.AURORA_WASM_BUILD || 'build-wasm');
const PORT = Number(process.env.AURORA_AU_PORT || 8767);
const DBG = Number(process.env.AURORA_AU_DBG_PORT || 9339);
const PAGE = 'aurora_verify_wasm_audio.html';
const URL = `http://127.0.0.1:${PORT}/${PAGE}`;
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

const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'au-edge-'));
const browser = spawn(EDGE, [
    '--headless=new', `--remote-debugging-port=${DBG}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=800,600', URL,
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
const page = pageList.find(p => p.type === 'page' && p.url.includes(PAGE));
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
const sleep = ms => new Promise(r => setTimeout(r, ms));

/// 状态串 → 字段表（探针每 100ms 发布一次 `window.__auState`）。
const fields = s => Object.fromEntries((s || '').split(/\s+/).filter(Boolean)
    .map(kv => { const i = kv.indexOf('='); return i < 0 ? [kv, ''] : [kv.slice(0, i), kv.slice(i + 1)]; }));
const state = async () => fields(await evaluate('window.__auState || ""'));
async function waitState(pred, timeout = 10000) {
    const t0 = Date.now();
    for (;;) {
        const f = await state();
        if (pred(f)) return f;
        if (Date.now() - t0 > timeout) throw new Error(`等待条件超时，最后状态: ${JSON.stringify(f)}`);
        await sleep(100);
    }
}

/// 把探针的 ScriptProcessor 分一路接 AnalyserNode，取一次时域 RMS。
/// 无音频输出设备的 headless 环境里，这是「确有信号抵达目的地」的唯一可测口径。
const RMS_JS = `(async () => {
    const w = globalThis.__auroraWa;
    if (!w || !w.node) return -1;
    if (!w.__an) { w.__an = w.ctx.createAnalyser(); w.__an.fftSize = 2048; w.node.connect(w.__an); }
    const buf = new Float32Array(w.__an.fftSize);
    let best = 0;
    for (let i = 0; i < 12; i++) {
        w.__an.getFloatTimeDomainData(buf);
        let acc = 0;
        for (let k = 0; k < buf.length; k++) acc += buf[k] * buf[k];
        best = Math.max(best, Math.sqrt(acc / buf.length));
        await new Promise(r => setTimeout(r, 40));
    }
    return best;
})()`;

async function click(x, y) {
    for (const type of ['mousePressed', 'mouseReleased']) {
        await R('Input.dispatchMouseEvent', {type, x, y, button: 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1});
    }
    await sleep(120);
}

try {
    // —— 探针上线：状态串出现即 main() 已跑完（后端 start() 已返回）
    const up = await waitState(f => f.silent !== undefined, 20000);

    // —— 判据 ①：真后端在位（非静默降级），格式协商来自浏览器
    ok('① AudioContext 非静默模式（silent=0，设备协商成功）', up.silent === '0', JSON.stringify(up));
    ok('① 协商采样率来自 ctx.sampleRate（44.1k/48k 之一）', ['44100', '48000'].includes(up.rate), `rate=${up.rate}`);
    ok('① 图侧声道契约恒 stereo', up.ch === '2', `ch=${up.ch}`);
    ok('① 缓冲源在播（playing=1）', up.playing === '1');

    // —— 判据 ②：自动播放闸门——start() 成功 ≠ 出声，未手势时零消费
    ok('② 未手势时上下文 suspended（state=0）', up.state === '0', `state=${up.state}`);
    const g1 = await state();
    await sleep(600);
    const g2 = await state();
    ok('② 闸门期零消费（consumed 与图时钟双双冻结）',
        Number(g2.consumed) === Number(g1.consumed) && Number(g2.ctime) === Number(g1.ctime),
        `consumed=${g1.consumed}→${g2.consumed} ctime=${g1.ctime}→${g2.ctime}`);

    // —— 判据 ③：真实鼠标按下开闸 ⇒ 消费按墙钟推进
    const box = await evaluate(`(()=>{const r=document.getElementById('gesture').getBoundingClientRect();`
        + `return {x:r.left+r.width/2,y:r.top+r.height/2};})()`);
    await click(box.x, box.y);
    const run = await waitState(f => f.state === '1', 8000);
    ok('③ 真实手势后 context 进入 running（state=1）', run.state === '1', `state=${run.state}`);

    const a0 = await state();
    const w0 = Date.now();
    await sleep(2000);
    const a1 = await state();
    const w1 = Date.now();
    // 状态串里的 consumed/ctime 均以**秒**计（探针按采样率归一），故判据是「每秒墙钟消费
    // 一秒音频」⇒ 比值应 ≈ 1.00，而非拿帧数去比采样率。
    const dCons = Number(a1.consumed) - Number(a0.consumed);
    const dWall = (w1 - w0) / 1000;
    const speed = dCons / dWall;
    ok('③ 消费按墙钟实时推进（秒/秒 ∈ [0.85,1.15]）', Math.abs(speed - 1) <= 0.15,
        `${speed.toFixed(3)}× 实时（${dCons.toFixed(3)}s/${dWall.toFixed(2)}s）`);
    ok('③ 稳态零欠载（推式环水位够用）', Number(a1.underrun) === Number(a0.underrun), `underrun=${a1.underrun}`);
    ok('③ 图时钟只领先设备时钟一个水位（|ctime-consumed| ≤ 0.3s）',
        Math.abs(Number(a1.ctime) - Number(a1.consumed)) <= 0.3,
        `ctime=${a1.ctime} consumed=${a1.consumed}`);

    // —— 信号旁证：AnalyserNode 读到满幅正弦（440 Hz ⇒ RMS ≈ 0.707；阈值 0.1）
    const rms = await evaluate(RMS_JS);
    ok('旁证 信号真达目的地（Analyser RMS > 0.1）', rms > 0.1, `rms=${Number(rms).toFixed(4)}`);

    // —— 判据 ④：主线程长任务饿死 ⇒ 欠载计数上升，且能自愈续播（非一死了之）
    const before = await state();
    await evaluate(`(()=>{const t0=performance.now();while(performance.now()-t0<400);return 1;})()`);
    await sleep(500);
    const starved = await state();
    ok('④ 长任务后出现欠载（补零而非静音僵死）', Number(starved.underrun) > Number(before.underrun),
        `underrun ${before.underrun}→${starved.underrun}`);
    const c0 = Number(starved.consumed);
    await sleep(700);
    const healed = await state();
    ok('④ 欠载后自愈续播（消费继续推进）', Number(healed.consumed) > c0, `consumed ${c0}→${healed.consumed}`);
    ok('④ 自愈后仍在 running 且缓冲源未停', healed.state === '1' && healed.playing === '1',
        `state=${healed.state} playing=${healed.playing}`);
} catch (e) {
    console.log('FAIL  探针驱动异常 —', String(e).slice(0, 300));
    failures++;
} finally {
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} 项失败`);
    browser.kill();
    server.close();
}
process.exit(failures === 0 ? 0 : 1);
