// Aurora verify — wasm fetch 真跑探针的 node 编排脚本（§16 第 37 项验收腿）。
//
// 职责三件：起本地 HTTP 服务端（相一的真实对端，只绑 127.0.0.1）、驱动探针（模块就绪 →
// `aurora_fetch_probe_run(port)` → 等探针收摊 → 读判据台账）、给退出码（0 = 全过 / 1 = 有判据未过）。
//
// 用法（构建产物与判据细节见 `wasm_fetch_live_probe.cpp` 头注释）：
//   cmake --build build-wasm --target aurora_verify_wasm_fetch
//   node build-wasm/tools/verify/wasm_fetch_node_drive.mjs

import { createServer } from 'node:http';
import auroraFetchProbe from './aurora_verify_wasm_fetch.mjs';

/// @brief 起本地服务端并等监听就绪。
/// @return 服务端实例（close 由收尾负责）。
const server = createServer((req, res) => {
    const chunks = [];
    req.on('data', (chunk) => chunks.push(chunk));
    req.on('end', () => {
        const body = Buffer.concat(chunks).toString('utf8');
        switch (req.url) {
            case '/hello':
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end('hello fetch leg');
                return;
            case '/echo':
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end(body);
                return;
            case '/marker':
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end(req.headers['x-probe-marker'] ?? 'missing');
                return;
            case '/hop1':
                // 手动重定向契约：3xx 原样交回（wasm fetch 腿以 redirect: 'manual' 发出），
                // 链由发起侧既有重定向逻辑跟随。
                res.writeHead(302, {Location: '/next'});
                res.end();
                return;
            case '/next':
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end('landed');
                return;
            case '/set-cookie':
                res.setHeader('Set-Cookie', 'probe=1; Path=/');
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end('set');
                return;
            case '/read-cookie':
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end(req.headers.cookie ?? 'none');
                return;
            default:
                res.writeHead(404, {'Content-Type': 'text/plain'});
                res.end('no route');
        }
    });
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;

const module = await auroraFetchProbe();

/// @brief 探针收摊的那枚 promise：C++ 侧 PENDING 归零时经 EM_JS resolve。
const done = new Promise((resolve) => {
    globalThis.__auroraFetchProbeResolve = resolve;
});

module._aurora_fetch_probe_run(port);

const failures = await done.then(() => module._aurora_fetch_probe_report());
console.log(failures === 0 ? 'wasm fetch probe: ALL PASS' : `wasm fetch probe: ${failures} case(s) failed`);
// 硬退，且先于任何句柄收尾：模块运行时的 uv_async 在 Windows node 的排空/收尾路径上打
// `uv_async_close` 断言——那是退出噪声，不是判据失败；判据以台账为准，退出码经 reallyExit 交出
// （服务端与模块随进程终止，无需逐句柄关闭）。
process.reallyExit(failures === 0 ? 0 : 1);
