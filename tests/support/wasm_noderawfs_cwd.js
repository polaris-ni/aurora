// ============================================================
// wasm_noderawfs_cwd.js — Emscripten NODERAWFS 的 cwd 形态修正（Windows 宿主）
// ------------------------------------------------------------
// 背景：`-sNODERAWFS=1` 让 wasm 直通宿主文件系统，但该模式下 Emscripten 把
// `FS.cwd()` 实现为 `process.cwd()`。Windows 宿主返回 `D:\a\b`（盘符 + 反斜杠），
// 而 musl 的 `getcwd()` 只接受 POSIX 形态的绝对路径——实测该形态下 `getcwd()`
// 直接置 ENOENT 并返回 NULL，连锁导致：
//   - `std::filesystem::current_path()` 抛 "call to getcwd failed"；
//   - `std::filesystem::absolute(相对路径)` 全部失败（先解析 cwd）；
//   - 库内 `debug_backend` / `preferences` 的默认目录解析随之崩。
// 归一为 `/a/b`（去盘符、转正斜杠）后，getcwd / realpath / current_path 全部恢复，
// 且相对路径仍由 Node 依「当前盘符」展开，与宿主语义一致（实测 chdir 后相对
// 路径解析正确）。
//
// 该文件经 `--post-js` 注入（须在运行时定义之后执行，故不能用 --pre-js）。
// 非 Windows 宿主（Linux/macOS CI）的 process.cwd() 本就是 POSIX 形态，此处为恒等变换。
// ============================================================

(function () {
    if (typeof FS === "undefined" || typeof FS.cwd !== "function" || typeof process === "undefined") {
        return;
    }
    FS.cwd = function () {
        var cwd = String(process.cwd()).replace(/\\/g, "/");
        if (/^[A-Za-z]:/.test(cwd)) {
            cwd = cwd.slice(2);  // 去盘符：`D:/a/b` → `/a/b`
        }
        if (cwd.charAt(0) !== "/") {
            cwd = "/" + cwd;
        }
        return cwd;
    };
})();
