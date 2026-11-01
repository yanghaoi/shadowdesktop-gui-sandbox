// cloak_test.cpp — 实验: 非输入桌面上, 临时去掉 DWM cloak 后 PrintWindow 能否出真内容
// 对每个可见顶层窗口依次尝试三种策略并打印结果表:
//   A. PrintWindow(PW_RENDERFULLCONTENT)            (现状, cloak 时空白)
//   B. DwmSetWindowAttribute(DWMWA_CLOAK, FALSE) + PW (实验路径)
//   C. WM_PRINT (现有兜底)
// 组合输出一张 PNG (按 B/C 最先成功者), 用于目检。
// 用法: cloak_test.exe <desktopName> <out.png>
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <stdio.h>
#include <vector>
#include <string>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#ifndef DESKTOP_ALL_ACCESS
#define DESKTOP_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | DESKTOP_CREATEMENU | \
    DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | \
    DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS | \
    DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS)
#endif

static bool IsBlank(HDC wdc, int w, int h)
{
    COLORREF key = RGB(255, 0, 255);
    for (int y = 2; y < h; y += 11)
        for (int x = 2; x < w; x += 11)
            if (GetPixel(wdc, x, y) != key) return false;
    return true;
}

struct WinInfo {
    HWND   h;
    wchar_t cls[64];
    wchar_t txt[128];
    bool aOk, bOk, cOk;   // A/B/C 三策略是否拿到内容
};

static BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lp)
{
    auto* v = (std::vector<WinInfo>*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    WinInfo wi = {};
    wi.h = hwnd;
    GetClassNameW(hwnd, wi.cls, 64);
    wchar_t t[128] = L"";
    SendMessageTimeoutW(hwnd, WM_GETTEXT, 128, (LPARAM)t, SMTO_ABORTIFHUNG, 1000, NULL);
    wcscpy_s(wi.txt, t);
    v->push_back(wi);
    return TRUE;
}

// 对单个窗口尝试一种截图手段; 成功(非空白)时立即贴到画布并返回 true
static bool TryCapture(HWND hwnd, int strategy, HDC wdc, int w, int h,
                       HDC cdc, int cx, int cy)
{
    // 洋红底
    HBRUSH mag = CreateSolidBrush(RGB(255, 0, 255));
    RECT wr = { 0, 0, w, h };
    FillRect(wdc, &wr, mag);
    DeleteObject(mag);

    BOOL cloakedByUs = FALSE;
    BOOL origCloak = FALSE;
    if (strategy == 1) {
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAK, &origCloak, sizeof(origCloak));
        BOOL off = FALSE;
        cloakedByUs = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &off, sizeof(off))
                      == S_OK;
    }

    bool got = false;
    if (strategy == 2) {
        SendMessageTimeoutW(hwnd, WM_PRINT, (WPARAM)wdc,
                            (LPARAM)(PRF_ERASEBKGND | PRF_NONCLIENT |
                                     PRF_CLIENT | PRF_CHILDREN),
                            SMTO_ABORTIFHUNG, 3000, NULL);
    } else {
        got = PrintWindow(hwnd, wdc, PW_RENDERFULLCONTENT) != FALSE
              && !IsBlank(wdc, w, h);
    }
    if (strategy != 2 && !got) {
        // PW 拿不到时也试试 WM_PRINT 一并补内容
        SendMessageTimeoutW(hwnd, WM_PRINT, (WPARAM)wdc,
                            (LPARAM)(PRF_ERASEBKGND | PRF_NONCLIENT |
                                     PRF_CLIENT | PRF_CHILDREN),
                            SMTO_ABORTIFHUNG, 3000, NULL);
    }
    got = !IsBlank(wdc, w, h);

    if (strategy == 1 && cloakedByUs)
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &origCloak, sizeof(origCloak));

    if (got)
        BitBlt(cdc, cx, cy, w, h, wdc, 0, 0, SRCCOPY);
    return got;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: cloak_test <desktop> <out.png>\n"); return 1; }
    SetProcessDPIAware();

    HDESK hd = OpenDesktopW(argv[1], 0, FALSE, DESKTOP_ALL_ACCESS);
    if (!hd) { wprintf(L"open desktop failed %lu\n", GetLastError()); return 2; }

    std::vector<WinInfo> wins;
    struct Ctx { HDESK h; std::vector<WinInfo>* v; } ctx = { hd, &wins };

    HANDLE t = CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
        Ctx* c = (Ctx*)p;
        SetThreadDesktop(c->h);
        EnumDesktopWindows(c->h, CollectProc, (LPARAM)c->v);
        return 0;
    }, &ctx, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CloseDesktop(hd);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gsi, NULL);

    int vw = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw2 = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw2;  bi.bmiHeader.biHeight = -vh;
    bi.bmiHeader.biPlanes = 1;   bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC sdc = GetDC(NULL);
    HBITMAP cv = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC cdc = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(cdc, cv);
    ReleaseDC(NULL, sdc);
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
    RECT full = { 0, 0, vw2, vh };
    FillRect(cdc, &full, bg);
    DeleteObject(bg);

    wprintf(L"%-22ls %-14ls %-8ls %-8ls %-8ls\n",
            L"class", L"title", L"A:PW", L"B:cloak", L"C:wmprint");
    FILE* logf = _wfopen(L"cloak_test_log.txt", L"wb");
    if (logf) fwprintf(logf, L"class\ttitle\tA_PW\tB_cloak\tC_wmprint\r\n");
    // z 序: 枚举是自顶向下, 绘制倒序
    for (size_t k = wins.size(); k-- > 0; ) {
        WinInfo& wi = wins[k];
        RECT r;
        if (IsIconic(wi.h)) {
            WINDOWPLACEMENT wp; wp.length = sizeof(wp);
            if (!GetWindowPlacement(wi.h, &wp)) continue;
            r = wp.rcNormalPosition;
        } else if (!GetWindowRect(wi.h, &r)) continue;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) continue;

        HDC wdc = CreateCompatibleDC(cdc);
        HBITMAP wb = CreateCompatibleBitmap(cdc, w, h);
        HGDIOBJ wo = SelectObject(wdc, wb);

        wi.aOk = TryCapture(wi.h, 0, wdc, w, h, cdc, r.left - vw, r.top - vy);
        wi.bOk = wi.aOk ? false
                : TryCapture(wi.h, 1, wdc, w, h, cdc, r.left - vw, r.top - vy);
        wi.cOk = (wi.aOk || wi.bOk) ? false
                : TryCapture(wi.h, 2, wdc, w, h, cdc, r.left - vw, r.top - vy);
        wprintf(L"%-22ls %-14ls %-8ls %-8ls %-8ls\n",
                wi.cls, wi.txt, wi.aOk ? L"ok" : L"blank",
                wi.bOk ? L"ok" : L"blank", wi.cOk ? L"ok" : L"blank");
        if (logf)
            fwprintf(logf, L"%ls\t%ls\t%ls\t%ls\t%ls\r\n",
                     wi.cls, wi.txt, wi.aOk ? L"ok" : L"blank",
                     wi.bOk ? L"ok" : L"blank", wi.cOk ? L"ok" : L"blank");

        SelectObject(wdc, wo);
        DeleteObject(wb);
        DeleteDC(wdc);
    }

    // 保存
    int wOut = vw2, hOut = vh, stride = vw2 * 4;
    std::vector<BYTE> px((size_t)stride * hOut);
    BITMAPINFO bi2 = {}; bi2.bmiHeader = bi.bmiHeader;
    GetDIBits(cdc, cv, 0, hOut, px.data(), &bi2, DIB_RGB_COLORS);
    static const CLSID png = {0x557CF406,0x1A04,0x11D3,{0x9A,0x73,0,0,0xF8,0x1E,0xF3,0x2E}};
    Gdiplus::Bitmap bmp(wOut, hOut, stride, PixelFormat32bppRGB, px.data());
    bmp.Save(argv[2], &png, NULL);
    wprintf(L"[+] saved %ls\n", argv[2]);
    return 0;
}
