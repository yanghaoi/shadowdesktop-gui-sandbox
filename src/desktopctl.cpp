// desktopctl.cpp — Windows 沙箱桌面控制工具
//
// 用途: 在独立的 Windows 桌面对象 (Desktop Object, 非虚拟桌面) 上运行 GUI
//       程序并对其截图。沙箱桌面上的窗口不出现在用户桌面、不进用户任务栏、
//       抢不到用户键盘鼠标焦点, 从根本上解决 GUI 自动化测试干扰用户的问题。
//
// 用法:
//   desktopctl create  <name>                          预建桌面 (进程全部退出后会自动消失)
//   desktopctl launch  <name> <exe> [args...]          在沙箱桌面启动程序 (桌面不存在则自动创建), 打印 PID=
//   desktopctl shot    <name> <out.png> [--refresh]    截取沙箱桌面主显示器为 PNG (--refresh: 先强制全部窗口重绘)
//   desktopctl kill    <name>                          终止沙箱桌面上所有进程
//   desktopctl enum                                    列出当前窗口站的所有桌面
//   desktopctl switch  <name>                          切换物理显示到该桌面 (调试用, 切回: switch Default)
//   desktopctl run     <name> <exe> <out.png> [延迟秒] [--keep]
//                                                      一站式: launch -> 等待 -> shot -> kill (--keep 则不杀)
//
// 典型 GUI 自动化测试流程 (见 README.md):
//   desktopctl run ZCodeTest sample_gui.exe shot.png 3
//
// 构建 (mingw-w64 g++):
//   g++ -std=c++17 -O2 -municode -mconsole -DNOMINMAX -DUNICODE -D_UNICODE ^
//       -D_WIN32_WINNT=0x0A00 desktopctl.cpp -o desktopctl.exe ^
//       -lgdi32 -luser32 -lgdiplus -static
#include "gdi_capture.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <tlhelp32.h>

// ---------------------------------------------------------------- utilities

static void EnableDpi()
{
    EnableDpiAwareness();
}

// 打开指定桌面, 不存在则创建 (GENERIC_ALL, 当前用户在自家会话的 WinSta0 内,
// 无需处理 DACL)。bInherit: 句柄是否可被子进程继承 (keeper 方案需要)。
static HDESK OpenOrCreateDesktop(const wchar_t* name, ACCESS_MASK access, bool bInherit)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, bInherit ? TRUE : FALSE };
    HDESK h = OpenDesktopW(name, 0, FALSE, access);
    if (h && bInherit) {
        // OpenDesktop 拿到的句柄没带继承属性, 复制一份可继承的
        HDESK h2 = NULL;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(),
                        (HANDLE*)&h2, 0, TRUE, DUPLICATE_SAME_ACCESS);
        if (h2) { CloseDesktop(h); h = h2; }
    }
    if (!h)
        h = CreateDesktopExW(name, NULL, NULL, 0, GENERIC_ALL,
                             bInherit ? &sa : NULL, 0, NULL);
    return h;
}

// -------------------------------------------------------------- shot 子命令

struct ShotCtx {
    HDESK         hdesk;
    const wchar_t* path;
    bool          refresh;
    int           w, h;
    bool          ok;
    DWORD         err;
    int           stage;    // 1=SetThreadDesktop 2=capture
    int           capStage; // 详见 gdi_capture.h
    int           strategy; // 1=BitBlt 整屏  2=PrintWindow/WM_PRINT 合成  0=失败
    PwDiag        pwDiag;
};

static BOOL CALLBACK ForceRedrawProc(HWND hwnd, LPARAM lParam)
{
    (void)lParam;
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
    return TRUE;
}

// 关键: 截图必须发生在无窗口的专用线程上 (SetThreadDesktop 对拥有窗口/钩子
// 的线程会失败), 且要先切换线程桌面再创建屏幕 DC, 否则得到的是黑屏。
static DWORD WINAPI ShotThreadProc(LPVOID param)
{
    ShotCtx* c = (ShotCtx*)param;

    if (!SetThreadDesktop(c->hdesk)) {
        c->stage = 1;
        c->err = GetLastError();
        return 1;
    }

    if (c->refresh)
        EnumDesktopWindows(c->hdesk, ForceRedrawProc, 0);

    c->stage = 2;
    c->ok  = CaptureCurrentDesktopToPNG(c->path, &c->w, &c->h, &c->capStage);
    c->strategy = c->ok ? 1 : 0;
    if (!c->ok) {
        // 非输入桌面会拒绝 BitBlt, 降级为逐窗口 PrintWindow/WM_PRINT 合成
        c->err = GetLastError();
        c->ok  = CaptureDesktopViaPrintWindow(c->path, &c->w, &c->h, &c->pwDiag);
        if (c->ok) c->strategy = 2;
    }
    if (!c->ok) c->err = GetLastError();
    return c->ok ? 0 : 1;
}

// flash 模式: 瞬时把沙箱桌面切为输入桌面 -> BitBlt 整屏 -> 切回原桌面。
// 屏幕会闪现沙箱画面几百毫秒, 但截图 100% 可靠, 是合成方案失败时的保底。
static int CmdShotFlash(const wchar_t* name, const wchar_t* path)
{
    HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    HDESK hTarget = OpenDesktopW(name, 0, FALSE,
                                 DESKTOP_SWITCHDESKTOP | DESKTOP_ALL_ACCESS);
    if (!hTarget) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        if (hInput) CloseDesktop(hInput);
        return 2;
    }

    if (!SwitchDesktop(hTarget)) {
        fwprintf(stderr, L"[!] SwitchDesktop failed (err=%lu)\n", GetLastError());
        CloseDesktop(hTarget);
        if (hInput) CloseDesktop(hInput);
        return 3;
    }
    Sleep(400); // 等 DWM 完成组合

    ShotCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.hdesk = hTarget;
    ctx.path  = path;
    DWORD tid = 0;
    HANDLE t  = CreateThread(NULL, 0, ShotThreadProc, &ctx, 0, &tid);
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }

    // 无论成败都切回用户桌面
    if (hInput) {
        SwitchDesktop(hInput);
        CloseDesktop(hInput);
    }
    CloseDesktop(hTarget);

    if (!ctx.ok) {
        fwprintf(stderr, L"[!] flash capture failed at stage %d/%d (err=%lu)\n",
                 ctx.stage, ctx.capStage, ctx.err);
        return 4;
    }
    wprintf(L"[+] saved %ls (%d x %d) [flash bitblt]\n", path, ctx.w, ctx.h);
    return 0;
}

static int CmdShot(const wchar_t* name, const wchar_t* path,
                   bool refresh, bool flash)
{
    if (flash) return CmdShotFlash(name, path);

    HDESK h = OpenDesktopW(name, 0, FALSE, DESKTOP_ALL_ACCESS);
    if (!h) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }

    ShotCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.hdesk    = h;
    ctx.path     = path;
    ctx.refresh  = refresh;

    HANDLE t = CreateThread(NULL, 0, ShotThreadProc, &ctx, 0, NULL);
    if (!t) {
        fwprintf(stderr, L"[!] CreateThread failed (err=%lu)\n", GetLastError());
        CloseDesktop(h);
        return 3;
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CloseDesktop(h);

    if (!ctx.ok) {
        fwprintf(stderr,
            L"[!] capture failed at stage %d/%d (err=%lu)\n"
            L"    hint: the desktop may have zero paintable windows; try "
            L"'--flash' for a guaranteed (but visible ~0.4s) capture\n",
            ctx.stage, ctx.capStage, ctx.err);
        return 4;
    }
    if (ctx.strategy == 2) {
        wprintf(L"[i] bitblt unavailable on inactive desktop, used "
                L"printwindow/wm_print composite:\n");
        for (int i = 0; i < ctx.pwDiag.n; ++i) {
            const PwWinDiag& d = ctx.pwDiag.items[i];
            wprintf(L"    %-24ls rect=(%ld,%ld,%ld,%ld) printwindow=%ls %ls\n",
                    d.cls, d.r.left, d.r.top, d.r.right, d.r.bottom,
                    d.pok ? L"ok" : L"fail",
                    d.blank ? L"BLANK(skipped)" : L"content-captured");
        }
    }
    wprintf(L"[+] saved %ls (%d x %d)\n", path, ctx.w, ctx.h);
    return 0;
}

// ------------------------------------------------------------- kill 子命令

static BOOL CALLBACK CollectPidProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && pid != GetCurrentProcessId()) {
        std::vector<DWORD>* pids = (std::vector<DWORD>*)lParam;
        bool dup = false;
        for (size_t i = 0; i < pids->size(); ++i)
            if ((*pids)[i] == pid) { dup = true; break; }
        if (!dup) pids->push_back(pid);
    }
    return TRUE;
}

static int CmdKill(const wchar_t* name)
{
    HDESK h = OpenDesktopW(name, 0, FALSE,
                           DESKTOP_ENUMERATE | DESKTOP_READOBJECTS);
    if (!h) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }

    std::vector<DWORD> pids;
    EnumDesktopWindows(h, CollectPidProc, (LPARAM)&pids);
    CloseDesktop(h); // 先关句柄再杀进程, 避免自锁

    if (pids.empty()) {
        wprintf(L"[i] no live process on desktop '%ls'\n", name);
        return 0;
    }

    // 桌面窗口只属于顶层/前台进程, 但它们会派生无窗口的子进程(浏览器
    // renderer/GPU、应用自起的 helper)。只杀窗口属主会留下孤儿进程群
    // (实测 msedge 残留 7 个子进程), 因此用进程快照把后代一并收集。
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            for (size_t round = 0; round < 4; ++round) { // 进程树深度上限
                bool grown = false;
                PROCESSENTRY32W pe;
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe)) {
                    do {
                        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
                        bool parentIn = false;
                        for (size_t i = 0; i < pids.size(); ++i)
                            if (pids[i] == pe.th32ParentProcessID) { parentIn = true; break; }
                        bool selfIn = false;
                        for (size_t i = 0; i < pids.size(); ++i)
                            if (pids[i] == pe.th32ProcessID) { selfIn = true; break; }
                        if (parentIn && !selfIn) {
                            pids.push_back(pe.th32ProcessID);
                            grown = true;
                        }
                    } while (Process32NextW(snap, &pe));
                }
                if (!grown) break;
            }
            CloseHandle(snap);
        }
    }

    for (size_t i = 0; i < pids.size(); ++i) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pids[i]);
        if (!hp) {
            DWORD e = GetLastError();
            // 87 = pid 已释放 (父进程死后子进程自行退出的常见情况)
            if (e == ERROR_INVALID_PARAMETER)
                wprintf(L"[i] pid %lu already gone\n", pids[i]);
            else
                wprintf(L"[!] cannot open pid %lu (err=%lu)\n", pids[i], e);
            continue;
        }
        TerminateProcess(hp, 0);
        WaitForSingleObject(hp, 3000);
        CloseHandle(hp);
        wprintf(L"[+] killed pid %lu\n", pids[i]);
    }
    return 0;
}

// keeper: 驻留进程, 拿住(继承来的)桌面句柄直到子进程退出。
// 桌面对象在"所有句柄关闭且无进程关联"时立即销毁; 若 launch 直接退出会
// 关掉自己的 HDESK, 而子进程可能尚未完成桌面挂接 -> 桌面销毁并连带杀死
// 子进程, 表现为子进程 0xC0000142 (实测复现)。
// keeper 由 launch 以 CREATE_NO_WINDOW 拉起并经句柄继承直接拿到 HDESK,
// 无窗口、无竞态。子进程退出后把退出码写入 exitFile (崩溃检测闭环)。
static int CmdKeeper(DWORD childPid, HDESK h, const wchar_t* exitFile)
{
    HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, childPid);
    DWORD code = (DWORD)-1;
    if (hp) {
        // 无限等待: keeper 的职责就是陪跑到子进程退出。24h 上限会误杀
        // 长跑测试 (超时后本函数关闭桌面句柄 -> 桌面销毁 -> 子进程被杀)。
        WaitForSingleObject(hp, INFINITE);
        GetExitCodeProcess(hp, &code);
        CloseHandle(hp);
    }
    if (exitFile && *exitFile) {
        FILE* f = _wfopen(exitFile, L"wb");
        if (f) {
            char buf[16];
            int n = _snprintf(buf, sizeof(buf), "%lu", code);
            if (n > 0) fwrite(buf, 1, (size_t)n, f);
            fclose(f);
        }
    }
    CloseDesktop(h);
    return 0;
}

// ----------------------------------------------------------- launch 子命令

static int CmdLaunch(const wchar_t* name, int argc, wchar_t** argv)
{
    if (argc < 1) {
        fwprintf(stderr, L"[!] launch: missing <exe>\n");
        return 1;
    }

    HDESK h = OpenOrCreateDesktop(name, DESKTOP_ALL_ACCESS, true);
    if (!h) {
        fwprintf(stderr, L"[!] cannot create/open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }

    // 组装命令行: "exe" args...  (CreateProcessW 的 lpCommandLine 要求可写缓冲)。
    // 含空格且未加引号的参数补引号, 否则会被拆成多个参数
    std::wstring cmd;
    auto appendArg = [&cmd](const wchar_t* a) {
        bool needQuote = a[0] && a[0] != L'"' && wcspbrk(a, L" \t") != NULL;
        cmd += L" ";
        if (needQuote) cmd += L"\"";
        cmd += a;
        if (needQuote) cmd += L"\"";
    };
    const wchar_t* exe = argv[0];
    if (exe[0] == L'"') {
        cmd = exe; // 用户已自行加引号
    } else {
        cmd = L"\"";
        cmd += exe;
        cmd += L"\"";
    }
    for (int i = 1; i < argc; ++i)
        appendArg(argv[i]);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb        = sizeof(si);
    si.lpDesktop = (LPWSTR)name; // 核心一行: 子进程初始线程关联到沙箱桌面

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    BOOL ok = CreateProcessW(NULL, cmdbuf.data(), NULL, NULL, FALSE,
                             0, NULL, NULL, &si, &pi);

    if (!ok) {
        CloseDesktop(h);
        fwprintf(stderr, L"[!] CreateProcess '%ls' failed (err=%lu)\n",
                 cmd.c_str(), GetLastError());
        return 3;
    }
    wprintf(L"[+] launched on '%ls': %ls\n", name, cmd.c_str());
    wprintf(L"PID=%lu\n", pi.dwProcessId);

    // 生成 keeper 并把桌面句柄以继承方式交给它 (详见 CmdKeeper 注释)。
    // launch 不再自己 CloseDesktop —— keeper 持有引用直到子进程退出,
    // launch 退出时 OS 只会关闭它自己那份句柄, 不影响 keeper。
    // keeper 在子进程退出后把退出码写到 <CWD>\child_exit_<pid>.txt;
    // CWD 不可写时回退 %TEMP% (否则脚本永远等不到该文件, 静默失败)
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    wchar_t base[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, base);
    wchar_t exitFile[MAX_PATH + 64];
    _snwprintf(exitFile, MAX_PATH + 63, L"%ls\\child_exit_%lu.txt",
               base, pi.dwProcessId);
    exitFile[MAX_PATH + 63] = 0;
    FILE* probe = _wfopen(exitFile, L"wb");
    if (probe) {
        fclose(probe);
        DeleteFileW(exitFile);
    } else {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        _snwprintf(exitFile, MAX_PATH + 63, L"%lschild_exit_%lu.txt",
                   temp, pi.dwProcessId);
        exitFile[MAX_PATH + 63] = 0;
        wprintf(L"[i] cwd not writable, exit file moved to temp dir\n");
    }
    wchar_t keeperCmd[MAX_PATH * 2 + 160];
    _snwprintf(keeperCmd, MAX_PATH * 2 + 159,
               L"\"%ls\" --keeper %lu 0x%p \"%ls\"",
               selfPath, pi.dwProcessId, (void*)h, exitFile);
    keeperCmd[MAX_PATH * 2 + 159] = 0;
    // keeper 只应继承桌面句柄。先把标准句柄的继承标志摘掉:
    // 否则 keeper (驻留在子进程退出前) 会长期持有 launch 的 stdout 管道
    // 写端, 上游如 `desktopctl launch ... | tail` 永远等不到 EOF (实测复现)。
    // 注意 STD_*_HANDLE 是负数且递减 (-10/-11/-12), 不能写成无符号递增循环
    SetHandleInformation(GetStdHandle(STD_INPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW ki;
    ZeroMemory(&ki, sizeof(ki));
    ki.cb = sizeof(ki);
    PROCESS_INFORMATION kpi;
    ZeroMemory(&kpi, sizeof(kpi));
    if (!CreateProcessW(NULL, keeperCmd, NULL, NULL, TRUE /*继承句柄*/,
                        CREATE_NO_WINDOW, NULL, NULL, &ki, &kpi)) {
        CloseDesktop(h); // keeper 起不来, 只能指望子进程自己挂接够快
        fwprintf(stderr, L"[!] warning: keeper spawn failed (err=%lu); the "
                L"desktop may die and kill the child\n", GetLastError());
    } else {
        CloseHandle(kpi.hThread);
        CloseHandle(kpi.hProcess);
        wprintf(L"EXITFILE=%ls\n", exitFile);
    }

    // 诊断: 800ms 后看子进程是否还活着; 若已死, 报告退出码 (如 0xC0000142)
    Sleep(800);
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        if (code == STILL_ACTIVE)
            wprintf(L"[+] child alive after 800ms\n");
        else
            fwprintf(stderr, L"[!] child already exited, code=0x%08lX\n", code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

// ------------------------------------------------------------ enum/create/switch

static BOOL CALLBACK DesktopEnumProc(LPWSTR name, LPARAM lParam)
{
    (void)lParam;
    wprintf(L"  %ls\n", name);
    return TRUE;
}

static int CmdEnum()
{
    wprintf(L"[i] desktops in current window station:\n");
    if (!EnumDesktopsW(GetProcessWindowStation(), DesktopEnumProc, 0)) {
        fwprintf(stderr, L"[!] EnumDesktops failed (err=%lu)\n", GetLastError());
        return 1;
    }
    return 0;
}

// -------------------------------------------------------------- ps 子命令
// 列出在指定桌面上拥有窗口的进程 (EnumDesktopWindows 收集 PID + 窗口标题)。
// 用途: 确认沙箱里还有谁在跑 / kill 前预览, 替代盲猜 tasklist。
struct PsCtx { std::vector<DWORD> pids; std::vector<std::wstring> titles; };

static BOOL CALLBACK PsEnumProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return TRUE;
    PsCtx* c = (PsCtx*)lParam;
    for (size_t i = 0; i < c->pids.size(); ++i)
        if (c->pids[i] == pid) return TRUE;
    wchar_t txt[128] = L"";
    // 限时取标题: 目标若挂死不拖住枚举
    SendMessageTimeoutW(hwnd, WM_GETTEXT, 128, (LPARAM)txt,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, NULL);
    c->pids.push_back(pid);
    c->titles.push_back(txt);
    return TRUE;
}

static int CmdPs(const wchar_t* name)
{
    HDESK h = OpenDesktopW(name, 0, FALSE,
                           DESKTOP_ENUMERATE | DESKTOP_READOBJECTS);
    if (!h) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }
    PsCtx c;
    EnumDesktopWindows(h, PsEnumProc, (LPARAM)&c);
    CloseDesktop(h);

    if (c.pids.empty()) {
        wprintf(L"[i] no windows on desktop '%ls'\n", name);
        return 0;
    }
    wprintf(L"[i] processes on desktop '%ls':\n", name);
    for (size_t i = 0; i < c.pids.size(); ++i)
        wprintf(L"  PID=%lu  title=\"%ls\"\n", c.pids[i], c.titles[i].c_str());
    wprintf(L"[i] total %zu process(es)\n", c.pids.size());
    return 0;
}

// ---------------------------------------------------------- waitwin 子命令
// 等待 pid 在指定桌面出现可见顶层窗口 —— 代替 launch 后的固定 sleep。
// 退出码: 0=窗口已出现, 3=超时, 2=桌面打不开。
struct WaitWinCtx { DWORD pid; bool found; };

static BOOL CALLBACK WaitWinEnumProc(HWND hwnd, LPARAM lParam)
{
    WaitWinCtx* c = (WaitWinCtx*)lParam;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == c->pid && IsWindowVisible(hwnd)) {
        c->found = true;
        return FALSE;
    }
    return TRUE;
}

static int CmdWaitWin(const wchar_t* name, DWORD pid, int timeoutMs)
{
    HDESK h = OpenDesktopW(name, 0, FALSE,
                           DESKTOP_ENUMERATE | DESKTOP_READOBJECTS);
    if (!h) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }
    ULONGLONG start = GetTickCount64();
    ULONGLONG deadline = start + (ULONGLONG)timeoutMs;
    WaitWinCtx c = { pid, false };
    for (;;) {
        c.found = false;
        EnumDesktopWindows(h, WaitWinEnumProc, (LPARAM)&c);
        if (c.found) {
            wprintf(L"[+] pid %lu has a visible window on '%ls' (waited %lld ms)\n",
                    pid, name, (long long)(GetTickCount64() - start));
            CloseDesktop(h);
            return 0;
        }
        if (GetTickCount64() >= deadline) break;
        Sleep(120);
    }
    CloseDesktop(h);
    fwprintf(stderr, L"[!] timeout: pid %lu has no visible window on '%ls' within %d ms\n",
             pid, name, timeoutMs);
    return 3;
}

static int CmdCreate(const wchar_t* name)
{
    HDESK h = OpenOrCreateDesktop(name, DESKTOP_ALL_ACCESS, false);
    if (!h) {
        fwprintf(stderr, L"[!] cannot create/open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }
    CloseDesktop(h);
    // 注意: 桌面对象在"所有句柄关闭且无进程关联"时即被销毁, 单独 create
    // 的桌面存活期极短; 实际使用请走 launch (按需自动创建)。
    wprintf(L"[+] desktop '%ls' available (transient: it lives while a "
            L"process/thread is attached; use 'launch' to create on demand)\n",
            name);
    return 0;
}

static int CmdSwitch(const wchar_t* name)
{
    HDESK h = OpenDesktopW(name, 0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!h) {
        fwprintf(stderr, L"[!] cannot open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }
    if (!SwitchDesktop(h)) {
        fwprintf(stderr, L"[!] SwitchDesktop failed (err=%lu)\n", GetLastError());
        CloseDesktop(h);
        return 3;
    }
    CloseDesktop(h);
    wprintf(L"[+] switched to '%ls' (back to user desktop: desktopctl switch Default)\n",
            name);
    return 0;
}

// 诊断辅助: 打开桌面并保持 ms 毫秒。
// 用于验证: 桌面对象在"所有句柄关闭且无进程关联"时立即销毁。
static int CmdHoldTest(const wchar_t* name, int ms)
{
    HDESK h = OpenOrCreateDesktop(name, DESKTOP_ALL_ACCESS, false);
    if (!h) {
        fwprintf(stderr, L"[!] cannot create/open desktop '%ls' (err=%lu)\n",
                 name, GetLastError());
        return 2;
    }
    wprintf(L"[i] holding desktop '%ls' for %d ms\n", name, ms);
    Sleep(ms);
    CloseDesktop(h);
    wprintf(L"[+] desktop handle released\n");
    return 0;
}

// -------------------------------------------------------------- run 子命令
static int CmdRun(const wchar_t* name, const wchar_t* exe,
                  const wchar_t* outPng, int delaySec, bool keep)
{
    wprintf(L"[i] run: launching ...\n");
    const wchar_t* launchArgv[1] = { exe };
    int rc = CmdLaunch(name, 1, (wchar_t**)launchArgv);
    if (rc) return rc;

    wprintf(L"[i] run: waiting %d s ...\n", delaySec);
    Sleep(delaySec * 1000);

    rc = CmdShot(name, outPng, false, false);
    if (keep) {
        wprintf(L"[i] run: --keep, left processes alive\n");
        return rc;
    }
    int krc = CmdKill(name);
    return rc ? rc : krc;
}

// ------------------------------------------------------------------- main

static void PrintUsage()
{
    wprintf(
        L"ShadowDesktop desktopctl - run GUI tests on an isolated Windows desktop\n"
        L"\n"
        L"usage:\n"
        L"  desktopctl create  <name>\n"
        L"  desktopctl launch  <name> <exe> [args...]\n"
        L"  desktopctl shot    <name> <out.png> [--refresh] [--flash]\n"
        L"  desktopctl kill    <name>\n"
        L"  desktopctl enum\n"
        L"  desktopctl switch  <name>\n"
        L"  desktopctl run     <name> <exe> <out.png> [delay_sec] [--keep]\n"
        L"  desktopctl ps      <name>                          list processes (pids+titles) on desktop\n"
        L"  desktopctl waitwin <name> <pid> [timeout_ms=15000]  wait until pid shows a window\n");
}

int wmain(int argc, wchar_t** argv)
{
    EnableDpi();

    if (argc < 2) { PrintUsage(); return 1; }

    const wchar_t* cmdName = argv[1];

    if (!wcscmp(cmdName, L"enum") && argc == 2)  return CmdEnum();
    if (!wcscmp(cmdName, L"ps") && argc == 3)    return CmdPs(argv[2]);
    if (!wcscmp(cmdName, L"waitwin") && (argc == 4 || argc == 5)) {
        int timeout = (argc == 5) ? _wtoi(argv[4]) : 15000;
        if (timeout <= 0) timeout = 15000;
        return CmdWaitWin(argv[2], wcstoul(argv[3], NULL, 10), timeout);
    }

    if (!wcscmp(cmdName, L"create") && argc == 3) return CmdCreate(argv[2]);
    if (!wcscmp(cmdName, L"switch") && argc == 3) return CmdSwitch(argv[2]);
    if (!wcscmp(cmdName, L"kill")   && argc == 3) return CmdKill(argv[2]);
    if (!wcscmp(cmdName, L"holdtest") && argc == 4) return CmdHoldTest(argv[2], _wtoi(argv[3]));
    if (!wcscmp(cmdName, L"--keeper") && argc == 5)
        return CmdKeeper(wcstoul(argv[2], NULL, 10),
                         (HDESK)(uintptr_t)wcstoull(argv[3], NULL, 16),
                         argv[4]);

    if (!wcscmp(cmdName, L"shot") && argc >= 4 && argc <= 6) {
        bool refresh = false, flash = false;
        const wchar_t* out = argv[3];
        for (int i = 4; i < argc; ++i) {
            if (!wcscmp(argv[i], L"--refresh")) refresh = true;
            else if (!wcscmp(argv[i], L"--flash")) flash = true;
            else { fwprintf(stderr, L"[!] shot: unknown option '%ls'\n", argv[i]); return 1; }
        }
        return CmdShot(argv[2], out, refresh, flash);
    }

    if (!wcscmp(cmdName, L"launch") && argc >= 4)
        return CmdLaunch(argv[2], argc - 3, argv + 3);

    if (!wcscmp(cmdName, L"run") && (argc == 5 || argc == 6 || argc == 7)) {
        const wchar_t* name = argv[2];
        const wchar_t* exe  = argv[3];
        const wchar_t* png  = argv[4];
        int  delay = 3;
        bool keep  = false;
        for (int i = 5; i < argc; ++i) {
            if (!wcscmp(argv[i], L"--keep")) keep = true;
            else delay = _wtoi(argv[i]);
        }
        if (delay < 0) delay = 0;
        return CmdRun(name, exe, png, delay, keep);
    }

    PrintUsage();
    return 1;
}
