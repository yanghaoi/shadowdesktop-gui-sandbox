// sample_gui.cpp — 沙箱桌面 GUI 测试样本程序
//
// 配合 desktopctl.exe 使用: 被 desktopctl launch 到独立桌面上运行。
// 功能:
//   - 显示窗口: 桌面名 / PID / 分辨率 / 实时时钟, 证明程序在沙箱桌面存活且持续重绘
//   - 按钮"截取本桌面": GUI 程序对自身所在桌面截图 (同桌面内 BitBlt, 无需
//     SetThreadDesktop, 天然可靠)
//   - --autoshot <秒>: 定时自动自截图, 供无人值守自动化验证
//   - 每次成功自截图后写 <out>\last_selfshot.txt (内容为 PNG 路径),
//     供外部脚本断言
//
// 用法:
//   sample_gui.exe [--autoshot <秒>] [--out <输出目录>]
//     --out 默认为本 exe 所在目录
//
// 构建 (mingw-w64 g++):
//   g++ -std=c++17 -O2 -municode -mwindows -DNOMINMAX -DUNICODE -D_UNICODE ^
//       -D_WIN32_WINNT=0x0A00 sample_gui.cpp -o sample_gui.exe ^
//       -lgdi32 -luser32 -lshell32 -lgdiplus -static
#include "gdi_capture.h"
#include "automation.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define IDC_STATIC_INFO  2001
#define IDC_STATIC_CLOCK 2002
#define IDC_BTN_SHOT_W 3001
#define IDC_STATIC_STATUS 2003
#define IDC_BTN_QUIT_W 3002
#define IDC_BTN_POPUP_W 3005
#define IDC_GROUP_W 4001
#define IDC_EDIT_W  4002
#define IDC_COMBO_W 4003
#define IDC_CHK_W   4004

#define TIMER_CLOCK 1
#define TIMER_AUTOSHOT 2

// 管道线程发来的"修改自截图间隔"命令 (在 UI 线程执行)
#define WM_APP_SETINTERVAL (WM_APP + 1)

static const wchar_t kWndClass[] = L"ShadowSandboxSampleGuiWnd";

static struct {
    HWND hwnd;
    HWND hInfo;
    HWND hClock;
    HWND hBtnShot;
    HWND hStatus;
    HWND hBtnQuit;
    wchar_t outDir[MAX_PATH];
    int autoshotSec;
    volatile LONG selfShots; // UI 线程自增, 管道线程读取
} g;

// ------------------------------------------------------------ self screenshot

static void DoSelfShot(bool beep)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t png[MAX_PATH];
    _snwprintf(png, MAX_PATH,
               L"%ls\\selfshot_%04u%02u%02u_%02u%02u%02u.png",
               g.outDir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    png[MAX_PATH - 1] = 0;

    int w = 0, h = 0;
    // GUI 线程本来就运行在目标桌面上, 优先直接 BitBlt 整屏
    bool ok = CaptureCurrentDesktopToPNG(png, &w, &h);
    if (!ok) {
        // 本桌面处于非输入状态时屏幕 DC 拒绝 BitBlt, 降级为逐窗口合成
        ok = CaptureDesktopViaPrintWindow(png, &w, &h);
    }

    wchar_t msg[1024];
    if (ok) {
        InterlockedIncrement(&g.selfShots);
        // 落一个标记文件, 供外部脚本在沙箱外断言"自截图已发生"
        wchar_t marker[MAX_PATH];
        _snwprintf(marker, MAX_PATH, L"%ls\\last_selfshot.txt", g.outDir);
        marker[MAX_PATH - 1] = 0;
        HANDLE hf = CreateFileW(marker, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char ansi[MAX_PATH * 2];
            int n = WideCharToMultiByte(CP_ACP, 0, png, -1, ansi, sizeof(ansi),
                                        NULL, NULL);
            DWORD written = 0;
            if (n > 0) WriteFile(hf, ansi, (DWORD)n - 1, &written, NULL);
            CloseHandle(hf);
        }
        _snwprintf(msg, 1024,
                   L"[ok] self shot #%d saved:\n%ls  (%d x %d)",
                   g.selfShots, png, w, h);
        if (beep) MessageBeep(MB_OK);
    } else {
        _snwprintf(msg, 1024,
                   L"[fail] self shot failed (err=%lu)", GetLastError());
    }
    msg[1023] = 0;
    SetWindowTextW(g.hStatus, msg);
}

// ------------------------------------------------------------ automation ops

// 自动化可读取的命名值 (get/wait/tree 使用)
static bool AutoGetValue(const wchar_t* name, std::wstring& val)
{
    wchar_t b[1024];
    if (!wcscmp(name, L"status"))   { AutoSafeGetText(g.hStatus, b, 1024); val = b; return true; }
    if (!wcscmp(name, L"clock"))    { AutoSafeGetText(g.hClock, b, 160);   val = b; return true; }
    if (!wcscmp(name, L"title"))    { AutoSafeGetText(g.hwnd, b, 256);     val = b; return true; }
    if (!wcscmp(name, L"selfshots")){ val = std::to_wstring(g.selfShots); return true; }
    if (!wcscmp(name, L"autoshot")) { val = std::to_wstring(g.autoshotSec); return true; }
    if (!wcscmp(name, L"pid"))      { val = std::to_wstring(GetCurrentProcessId()); return true; }
    if (!wcscmp(name, L"desktop"))  { AutoGetDesktopName(b, 64); val = b; return true; }
    if (!wcscmp(name, L"outdir"))   { val = g.outDir; return true; }
    return false;
}

static int AutoResolveTarget(const wchar_t* t)
{
    if (!wcscmp(t, L"shot")    || !wcscmp(t, L"btnShot"))  return IDC_BTN_SHOT_W;
    if (!wcscmp(t, L"quit")    || !wcscmp(t, L"btnQuit"))  return IDC_BTN_QUIT_W;
    if (!wcscmp(t, L"popup")   || !wcscmp(t, L"btnPopup")) return IDC_BTN_POPUP_W;
    int id = _wtoi(t);
    return id > 0 ? id : -1;
}

// ------------------------------------------------------ 通用命令层接入点

static HWND AutoMainWnd() { return g.hwnd; }

// tree 的 app 块附加字段 (通用层输出 desktop/pid 后追加)
static void AutoTreeExtras(std::wstring& o)
{
    o += L", ";
    JsonKN(o, L"selfshots", g.selfShots);
    o += L", ";
    JsonKN(o, L"autoshot_sec", g.autoshotSec);
}

// 命令实现 (在管道服务线程上执行)。本程序只保留业务语义:
//   get <业务名> | set title <v...> | set interval <秒> | invoke <业务名|数字> | shot
// 通用 op (tree/click/find/ctrl(s)/key/set text/gettext/combo/check/invoke/
// wait/sleep/内置 get title|pid|desktop) 统一由 AutoCommonDispatch 兜底,
// 业务命令解析经 hooks 注入 (getValue/invokeTarget/treeAppInfo)。
void AutoDispatch(const std::vector<std::wstring>& tok,
                  const std::wstring& raw, std::wstring& out)
{
    (void)raw;
    const std::wstring& op = tok[0];

    if (op == L"get" && tok.size() >= 2) {
        std::wstring v;
        if (!AutoGetValue(tok[1].c_str(), v)) {
            out = L"{\"ok\":false,\"error\":\"unknown name\"}";
            return;
        }
        out = L"{\"ok\":true";
        out += L", ";
        JsonKV(out, L"name", tok[1].c_str());
        out += L", ";
        JsonKV(out, L"value", v.c_str());
        out += L"}";
        return;
    }

    if (op == L"set" && tok.size() >= 3) {
        std::wstring val;
        for (size_t i = 2; i < tok.size(); ++i) {
            if (i > 2) val += L' ';
            val += tok[i];
        }
        if (tok[1] == L"title") {
            SendMessageTimeoutW(g.hwnd, WM_SETTEXT, 0, (LPARAM)val.c_str(),
                                SMTO_ABORTIFHUNG, 3000, NULL);
            out = L"{\"ok\":true";
            out += L", ";
            JsonKV(out, L"title", val.c_str());
            out += L"}";
            return;
        }
        if (tok[1] == L"interval" || tok[1] == L"autoshot") {
            SendMessageTimeoutW(g.hwnd, WM_APP_SETINTERVAL,
                                (WPARAM)_wtoi(val.c_str()), 0,
                                SMTO_ABORTIFHUNG, 3000, NULL);
            out = L"{\"ok\":true";
            out += L", ";
            JsonKV(out, tok[1].c_str(), val.c_str());
            out += L"}";
            return;
        }
        // 其余 (含 set text) 落到通用层
    }

    if (op == L"shot") {
        std::wstring path;
        if (tok.size() >= 2) {
            path = tok[1];
        } else {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t pb[MAX_PATH];
            _snwprintf(pb, MAX_PATH,
                       L"%ls\\pipe_%04u%02u%02u_%02u%02u%02u.png",
                       g.outDir, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
            pb[MAX_PATH - 1] = 0;
            path = pb;
        }
        int w = 0, h = 0;
        // 本进程所有线程都在目标桌面: 直接 BitBlt, 失败再走逐窗口合成
        bool ok = CaptureCurrentDesktopToPNG(path.c_str(), &w, &h);
        if (!ok) ok = CaptureDesktopViaPrintWindow(path.c_str(), &w, &h);
        out = L"{\"ok\":";
        out += ok ? L"true" : L"false";
        if (ok) {
            out += L", ";
            JsonKV(out, L"path", path.c_str());
            out += L", ";
            JsonKN(out, L"w", w);
            out += L", ";
            JsonKN(out, L"h", h);
        } else {
            out += L", \"error\": \"capture failed\"";
        }
        out += L"}";
        return;
    }

    // 通用层兜底: tree/click/find/ctrl/ctrls/key/set text/gettext/combo/check/
    // invoke(AutoResolveTarget 解析业务名)/wait/sleep/get title|pid|desktop
    AutoCommonHooks hk;
    hk.getMain      = AutoMainWnd;
    hk.getValue     = AutoGetValue;
    hk.invokeTarget = AutoResolveTarget;
    hk.treeAppInfo  = AutoTreeExtras;
    if (AutoCommonDispatch(tok, out, hk)) return;

    out = L"{\"ok\":false,\"error\":\"unknown op; see automation.h header\"}";
}

// ------------------------------------------------------------------ UI setup

static HWND AddControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                       DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE),
                             NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void BuildUI(HWND hwnd)
{
    wchar_t deskName[64];
    AutoGetDesktopName(deskName, 64);

    wchar_t info[256];
    _snwprintf(info, 256,
               L"Desktop: %ls    PID: %lu    Screen: %d x %d",
               deskName, GetCurrentProcessId(),
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    info[255] = 0;

    g.hInfo = AddControl(hwnd, L"STATIC", info, 0, 16, 14, 520, 20, IDC_STATIC_INFO);
    g.hClock = AddControl(hwnd, L"STATIC", L"clock starting ...", 0,
                          16, 40, 520, 20, IDC_STATIC_CLOCK);
    g.hBtnShot = AddControl(hwnd, L"BUTTON", L"截取本桌面 -> PNG",
                            BS_PUSHBUTTON, 16, 70, 150, 30, IDC_BTN_SHOT_W);
    AddControl(hwnd, L"BUTTON", L"弹窗测试 (模态)",
               BS_PUSHBUTTON, 176, 70, 150, 30, IDC_BTN_POPUP_W);

    // 嵌套容器: edit/combo/chk 的父窗口是 GroupBox 本身 —— 用于验证
    // tree 的层级枚举与通用控件命令 (click/find/ctrl/ctrls/text)
    HWND hGroup = AddControl(hwnd, L"BUTTON", L"嵌套容器 (层级 tree / 通用控件命令演示)",
                             BS_GROUPBOX, 16, 110, 520, 150, IDC_GROUP_W);
    AddControl(hGroup, L"EDIT", L"",
               ES_AUTOHSCROLL | WS_BORDER, 14, 28, 240, 24, IDC_EDIT_W);
    HWND hCombo = AddControl(hGroup, L"COMBOBOX", L"",
                             CBS_DROPDOWNLIST, 14, 62, 240, 120, IDC_COMBO_W);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"选项甲");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"选项乙");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"选项丙");
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    AddControl(hGroup, L"BUTTON", L"复选框 (BM_GETCHECK 测试)",
               BS_AUTOCHECKBOX, 280, 28, 220, 24, IDC_CHK_W);

    g.hStatus = AddControl(hwnd, L"STATIC",
                           L"idle - click the button, or wait for --autoshot", 0,
                           16, 272, 520, 40, IDC_STATIC_STATUS);
    g.hBtnQuit = AddControl(hwnd, L"BUTTON", L"退出",
                            BS_PUSHBUTTON, 420, 320, 116, 30, IDC_BTN_QUIT_W);
}

static void UpdateClock()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[160];
    _snwprintf(buf, 160, L"%04u-%02u-%02u %02u:%02u:%02u  (tick keeps windows "
               L"repainting on the sandbox desktop)",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[159] = 0;
    SetWindowTextW(g.hClock, buf);
}

// ---------------------------------------------------------------- window proc

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        BuildUI(hwnd);
        SetTimer(hwnd, TIMER_CLOCK, 1000, NULL);
        if (g.autoshotSec > 0)
            SetTimer(hwnd, TIMER_AUTOSHOT, g.autoshotSec * 1000, NULL);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_CLOCK) UpdateClock();
        else if (wp == TIMER_AUTOSHOT) DoSelfShot(false);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_SHOT_W) DoSelfShot(true);
        else if (LOWORD(wp) == IDC_BTN_QUIT_W) PostQuitMessage(0);
        else if (LOWORD(wp) == IDC_BTN_POPUP_W)
            MessageBoxW(hwnd, L"这是一个模态弹窗, 由自动化通过 tree+click 关闭",
                        L"弹窗测试", MB_OKCANCEL);
        return 0;

    case WM_APP_SETINTERVAL: {
        int sec = (int)wp;
        g.autoshotSec = sec;
        KillTimer(hwnd, TIMER_AUTOSHOT);
        if (sec > 0) SetTimer(hwnd, TIMER_AUTOSHOT, sec * 1000, NULL);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------------- main

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow)
{
    EnableDpiAwareness();

    // --- 解析参数: --autoshot <sec>  --out <dir>  --automation
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g.autoshotSec = 0;
    bool automation = false;
    lstrcpynW(g.outDir, L".", MAX_PATH);

    // 默认输出目录 = exe 所在目录
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *slash = 0;
        lstrcpynW(g.outDir, exePath, MAX_PATH);
    }
    for (int i = 1; argv && i < argc; ++i) {
        if (!wcscmp(argv[i], L"--autoshot") && i + 1 < argc)
            g.autoshotSec = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc)
            lstrcpynW(g.outDir, argv[++i], MAX_PATH);
        else if (!wcscmp(argv[i], L"--automation"))
            automation = true;
    }
    if (argv) LocalFree(argv);
    CreateDirectoryW(g.outDir, NULL); // 已存在则无害
    // 输出目录不可写(CWD 只读/被删)会让 automation_info.json 和截图全部
    // 静默失败, 回退到 %TEMP% 下的唯一目录
    wchar_t probe[MAX_PATH];
    _snwprintf(probe, MAX_PATH, L"%ls\\w", g.outDir);
    probe[MAX_PATH - 1] = 0;
    if (!CreateDirectoryW(probe, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        _snwprintf(g.outDir, MAX_PATH, L"%lsShadowDesktop_%lu", temp,
                   GetCurrentProcessId());
        g.outDir[MAX_PATH - 1] = 0;
        CreateDirectoryW(g.outDir, NULL);
    } else {
        RemoveDirectoryW(probe); // 探测目录, 留给真实文件用
    }

    // --- 注册窗口类 / 建窗口
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);

    RECT rc = { 0, 0, 560, 360 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    // 标题带中文, 截图里可直接确认字体渲染
    HWND hwnd = CreateWindowExW(0, kWndClass,
                                L"ShadowDesktop 沙箱 GUI 样本",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow ? nCmdShow : SW_SHOW);
    UpdateWindow(hwnd);

    // 窗口就绪后再启动管道服务 (AutoDispatch 依赖 g.hwnd)
    if (automation) {
        wchar_t dn[64];
        AutoGetDesktopName(dn, 64);
        if (StartAutomation(L"ShadowAuto", g.outDir, dn))
            MessageBeep(MB_OK);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
