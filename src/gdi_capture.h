// gdi_capture.h — 桌面截图共享模块 (GDI BitBlt + GDI+ PNG 编码)
//
// 两种用法:
//   1) 自截图 (GUI 程序截自己所在桌面): GUI 线程直接调用 CaptureCurrentDesktopToPNG
//   2) 跨桌面截图 (desktopctl 外部截取沙箱桌面): 必须先在"无窗口的专用线程"上
//      OpenDesktop + SetThreadDesktop 切换过去, 再调用 CaptureCurrentDesktopToPNG。
//      直接在关联着 Default 桌面的线程上截另一桌面会得到黑屏。
//
// 链接: -lgdi32 -lgdiplus  (编译: -DNOMINMAX -DUNICODE -D_UNICODE, C++17)
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <cstdint>

#ifndef DESKTOP_ALL_ACCESS
#define DESKTOP_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | DESKTOP_CREATEMENU | \
    DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | \
    DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS | \
    DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS)
#endif

// 进程级 DPI 感知: 保证 GetDeviceCaps(HORZRES/VERTRES) 返回物理像素,
// 截图为显示器原始分辨率而非被 DPI 缩放过的逻辑分辨率。
inline void EnableDpiAwareness()
{
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
#else
    SetProcessDPIAware();
#endif
}

inline bool EnsureGdiplus()
{
    static ULONG_PTR s_token = 0;
    static bool      s_ok    = false;
    if (!s_ok) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&s_token, &input, NULL) != Gdiplus::Ok)
            return false;
        s_ok = true; // 有意不调用 GdiplusShutdown, 由进程退出统一回收
    }
    return s_ok;
}

// 把像素缓冲编码保存为 PNG (24bit RGB)。stride 必须是 4 的倍数。
inline bool SavePixelsToPNG(const wchar_t* pngPath, const BYTE* pixels,
                            int w, int h, int stride)
{
    if (!EnsureGdiplus() || !pixels || w <= 0 || h <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    // PNG 编码器 CLSID (系统内置, 各版本 Windows 固定)
    static const CLSID kPngEncoder =
        {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

    Gdiplus::Bitmap bmp(w, h, stride, PixelFormat32bppRGB,
                        const_cast<BYTE*>(pixels));
    return bmp.Save(pngPath, &kPngEncoder, NULL) == Gdiplus::Ok;
}

// 截取"调用线程当前所关联桌面"的主显示器, 保存为 PNG。
// 返回 true 时 *outW/*outH(可空) 为图像尺寸; 失败时 *failStage(可空) 为:
//   5=GDI+初始化失败  10=所有屏幕DC方案失败  20=建内存DC/位图失败
//   30=BitBlt失败  40=GetDIBits失败  50=保存PNG失败
//
// 说明: 非输入桌面上 BitBlt 的可用路径与输入桌面不同(实测带 CAPTUREBLT 的
// 组合会被拒绝), 因此按 屏幕DC来源(Display/GetDC) x ROP 标志 依次重试。
inline bool CaptureCurrentDesktopToPNG(const wchar_t* pngPath,
                                       int* outW = nullptr, int* outH = nullptr,
                                       int* failStage = nullptr)
{
    if (failStage) *failStage = 0;
    if (!EnsureGdiplus()) { SetLastError(ERROR_DLL_INIT_FAILED); if (failStage) *failStage = 5; return false; }

    struct DcPlan { bool useGetDc; DWORD rop; };
    const DcPlan plans[] = {
        { false, SRCCOPY | CAPTUREBLT },
        { false, SRCCOPY },
        { true,  SRCCOPY | CAPTUREBLT },
        { true,  SRCCOPY },
    };

    bool ok = false;
    int  lastStage = 10;
    for (size_t i = 0; i < sizeof(plans) / sizeof(plans[0]) && !ok; ++i) {
        HDC hdcScreen = plans[i].useGetDc ? GetDC(NULL)
                                          : CreateDCW(L"DISPLAY", NULL, NULL, NULL);
        if (!hdcScreen) { lastStage = 10; continue; }

        int w = GetDeviceCaps(hdcScreen, HORZRES);
        int h = GetDeviceCaps(hdcScreen, VERTRES);

        HDC     hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbm    = CreateCompatibleBitmap(hdcScreen, w, h);
        HBITMAP hbmOld = hbm ? (HBITMAP)SelectObject(hdcMem, hbm) : NULL;

        if (hdcMem && hbm && w > 0 && h > 0) {
            if (BitBlt(hdcMem, 0, 0, w, h, hdcScreen, 0, 0, plans[i].rop)) {
                // GetDIBits 要求位图当前不被选入任何 DC, 先摘下来
                SelectObject(hdcMem, hbmOld);
                hbmOld = nullptr;

                BITMAPINFO bi;
                ZeroMemory(&bi, sizeof(bi));
                bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth       = w;
                bi.bmiHeader.biHeight      = -h;          // 负高 = 自上而下行序
                bi.bmiHeader.biPlanes      = 1;
                bi.bmiHeader.biBitCount    = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                std::vector<BYTE> px((size_t)w * h * 4);
                if (GetDIBits(hdcMem, hbm, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h) {
                    if (SavePixelsToPNG(pngPath, px.data(), w, h, w * 4)) {
                        ok = true;
                        if (outW) *outW = w;
                        if (outH) *outH = h;
                    } else lastStage = 50;
                } else lastStage = 40;
            } else lastStage = 30;
        } else lastStage = 20;

        if (hbmOld) SelectObject(hdcMem, hbmOld);
        if (hbm)    DeleteObject(hbm);
        if (hdcMem) DeleteDC(hdcMem);
        if (plans[i].useGetDc) ReleaseDC(NULL, hdcScreen);
        else                   DeleteDC(hdcScreen);
    }

    if (!ok && failStage) *failStage = lastStage;
    return ok;
}

// ---------------------------------------------------------------------------
// 备用方案: 逐窗口 PrintWindow/WM_PRINT 合成"桌面截图"。
// 非输入桌面的屏幕 DC 拒绝 BitBlt(实测 err=5/6); 且窗口被 DWM cloak 后
// PrintWindow(PW_RENDERFULLCONTENT) 返回 TRUE 但内容为空(依赖 DWM 表面)。
// 因此对每个可见顶层窗口:
//   1) PrintWindow(PW_RENDERFULLCONTENT)
//   2) 若结果空白 -> SendMessage WM_PRINT (经典 GDI 自绘路径, 不经 DWM)
// 画布为该桌面虚拟屏大小的深灰底, 各窗口按 z 序(自底向上)贴回原坐标。
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// 单窗口诊断信息
struct PwWinDiag {
    wchar_t cls[48];
    RECT    r;
    BOOL    pok;      // PrintWindow 返回值
    bool    blank;    // 两种手段后仍全空白
};

struct PwDiag {
    int       n;                        // 实际记录条数
    int       captured;                 // 成功取到内容的窗口数
    PwWinDiag items[16];
};

// 以洋红色预填窗口位图, 采样检测 PrintWindow/WM_PRINT 是否真的画了东西
static bool PwBitmapIsBlank(HDC wdc, int ww, int wh)
{
    COLORREF key = RGB(255, 0, 255);
    int step = 11;
    for (int y = 2; y < wh; y += step)
        for (int x = 2; x < ww; x += step)
            if (GetPixel(wdc, x, y) != key)
                return false;
    return true;
}

inline bool CaptureDesktopViaPrintWindow(const wchar_t* pngPath,
                                         int* outW = nullptr, int* outH = nullptr,
                                         PwDiag* diag = nullptr)
{
    if (diag) { ZeroMemory(diag, sizeof(*diag)); }
    if (!EnsureGdiplus()) { SetLastError(ERROR_DLL_INIT_FAILED); return false; }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = GetSystemMetrics(SM_CXSCREEN); vh = GetSystemMetrics(SM_CYSCREEN); }
    if (vw <= 0 || vh <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return false; }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = vw;
    bi.bmiHeader.biHeight      = -vh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void*   bits      = NULL;
    HDC     hdcScreen = GetDC(NULL);
    HBITMAP hCanvas   = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC     hdcCanvas = hCanvas ? CreateCompatibleDC(hdcScreen) : NULL;
    HBITMAP hOld      = hdcCanvas ? (HBITMAP)SelectObject(hdcCanvas, hCanvas) : NULL;
    ReleaseDC(NULL, hdcScreen);

    bool ok = false;
    if (hdcCanvas && bits) {
        // 深灰底: 类似"无壁纸空桌面"
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        RECT full = { 0, 0, vw, vh };
        FillRect(hdcCanvas, &full, bg);
        DeleteObject(bg);

        // 先按枚举顺序(z序自顶向下)收集可见顶层窗口, 再倒序绘制(自底向上)
        struct CollectCtx { std::vector<HWND> list; };
        CollectCtx cc;
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            CollectCtx* cc = (CollectCtx*)lp;
            if (IsWindowVisible(hwnd)) {
                DWORD ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
                if (!(ex & WS_EX_TOOLWINDOW)) cc->list.push_back(hwnd);
            }
            return TRUE;
        }, (LPARAM)&cc);

        int captured = 0;
        for (size_t k = cc.list.size(); k-- > 0; ) {
            HWND hwnd = cc.list[k];
            RECT r;
            if (IsIconic(hwnd)) {
                // 最小化窗口: 屏幕坐标是 -32000, 用"还原位置"尝试截取
                // (PrintWindow 不依赖窗口在屏幕上的位置; 截不到会被空白逻辑跳过)
                WINDOWPLACEMENT wpl;
                wpl.length = sizeof(wpl);
                if (!GetWindowPlacement(hwnd, &wpl)) continue;
                r = wpl.rcNormalPosition;
            } else {
                if (!GetWindowRect(hwnd, &r)) continue;
            }
            int ww = r.right - r.left, wh = r.bottom - r.top;
            if (ww <= 0 || wh <= 0) continue;
            if (r.right <= vx || r.bottom <= vy || r.left >= vx + vw || r.top >= vy + vh)
                continue;

            PwWinDiag* d = NULL;
            if (diag && diag->n < (int)(sizeof(diag->items) / sizeof(diag->items[0]))) {
                d = &diag->items[diag->n++];
                GetClassNameW(hwnd, d->cls, 48);
                d->cls[47] = 0;
                d->r = r;
            }

            HDC     wdc  = CreateCompatibleDC(hdcCanvas);
            HBITMAP wb   = CreateCompatibleBitmap(hdcCanvas, ww, wh);
            HBITMAP wOld = (HBITMAP)SelectObject(wdc, wb);

            // 洋红底, 便于判定"什么都没画"
            HBRUSH mag = CreateSolidBrush(RGB(255, 0, 255));
            RECT wr = { 0, 0, ww, wh };
            FillRect(wdc, &wr, mag);
            DeleteObject(mag);

            BOOL pok = PrintWindow(hwnd, wdc, PW_RENDERFULLCONTENT);
            bool blank = PwBitmapIsBlank(wdc, ww, wh);
            if (blank) {
                // DWM cloak 会让 PW_RENDERFULLCONTENT 给出空表面,
                // 走窗口自身的经典 GDI 自绘 (WM_PRINT), 不经 DWM
                SendMessageTimeoutW(hwnd, WM_PRINT, (WPARAM)wdc,
                                    (LPARAM)(PRF_ERASEBKGND | PRF_NONCLIENT |
                                             PRF_CLIENT | PRF_CHILDREN),
                                    SMTO_ABORTIFHUNG, 3000, NULL);
                blank = PwBitmapIsBlank(wdc, ww, wh);
            }

            if (d) { d->pok = pok; d->blank = blank; }
            if (!blank) {
                BitBlt(hdcCanvas, r.left - vx, r.top - vy, ww, wh, wdc, 0, 0, SRCCOPY);
                ++captured;
            }
            DeleteObject(wb);
            DeleteDC(wdc);
        }

        if (diag) diag->captured = captured;
        if (captured > 0) {
            if (SavePixelsToPNG(pngPath, (const BYTE*)bits, vw, vh, vw * 4)) {
                ok = true;
                if (outW) *outW = vw;
                if (outH) *outH = vh;
            }
        } else {
            SetLastError(ERROR_NOT_FOUND);
        }
    }

    if (hOld)      SelectObject(hdcCanvas, hOld);
    if (hCanvas)   DeleteObject(hCanvas);
    if (hdcCanvas) DeleteDC(hdcCanvas);
    return ok;
}

