// wv2sandbox_test.cpp —— WebView2 × CreateDesktop 沙箱 兼容性探针（MinGW-w64）
//
// 背景结论（2026-11-04, win10 19045, WebView2 Runtime 155.0.4283.62, 详见 README
// 「WebView2 × 沙箱」一节）:
//   1) WebView2 宿主程序进 CreateDesktop 非输入桌面: 浏览器进程启动僵死
//      （UDF 能建、完成回调永不触发、DevTools 不监听、窗口黑屏），上游已知问题
//      MicrosoftEdge/WebView2Feedback#960，官方 priority-low。
//   2) 同版本完整 Edge 在沙箱桌面完全正常 → 死的只是 msedgewebview2 二进制。
//   3) "注入 hook CreateProcessW 改 lpDesktop 到 Default"的终态（dual 模式复现）:
//      浏览器进程能起来，但 Controller 创建失败 hr=0x80040205 —— 跨桌面
//      窗口父子关联被 Windows 拒绝，注入路线无效。
// runtime 升级后用本探针重验: 若 minimal 模式在沙箱 READY，说明 #960 已修。
//
// 判据（经窗口标题呈现，desktopctl shot 读取）:
//   READY              WebView2 正常创建并渲染
//   FAIL hr=... where=N  失败: where 0/1=minimal 环境/控制器;
//                      10/11/12=dual 切桌面/loader/createEnv; 20/21=dual 环境/控制器
//   STUCK              30 秒无回调（沙箱僵死特征）
//
// 用法:
//   wv2sandbox_test.exe          minimal 模式（镜像 musicdl 的 options=nullptr 路径）
//   wv2sandbox_test.exe dual     双桌面线程模式（主窗口当前桌面, WebView2 创建线程
//                                SetThreadDesktop("Default")）
//
// 构建（winlibs mingw64; 需 WebView2 SDK 头文件 + 同目录 runtime\WebView2Loader.dll）:
//   g++ -std=c++17 -O2 -municode -mwindows -DNOMINMAX -DUNICODE -D_UNICODE \
//       -D_WIN32_WINNT=0x0A00 -I<SDK>/build/native/include wv2sandbox_test.cpp \
//       -o wv2sandbox_test.exe -lgdi32 -luser32 -lole32 -loleaut32 -luuid -static
//
// MinGW 注意: 不为 __declspec(uuid()) 接口生成 __uuidof 特化，QueryInterface 只认
// IID_IUnknown（回调处理器在 WebView2 内部只被当作 IUnknown 使用，安全）。
#include <windows.h>
#include <string>
#include "WebView2.h"

static ICoreWebView2Environment* g_env = nullptr;
static ICoreWebView2Controller*  g_controller = nullptr;
static ICoreWebView2*            g_webview = nullptr;
static HWND  g_hwnd = nullptr;
static bool  g_dual = false;
static bool  g_flashhide = false;   // flashhide 模式: 正常显示 3s 后隐藏
static bool  g_cloak = false;       // cloak 模式: 正常显示 3s 后 DWM cloak（win32 层面仍"可见"）
static bool  g_novis = false;       // novis 模式: 宿主窗口正常显示 + controller IsVisible(FALSE)（musicdl --shot 同款）
#include <dwmapi.h>
static DWORD g_aThreadId = 0;

static const wchar_t kHtml[] = LR"html(<!doctype html>
<meta charset="utf-8"><title>wv2sandbox_test</title>
<body style="font-family:'Segoe UI',sans-serif;background:#14181d;color:#e8eaed;margin:24px">
<h2 id="title">wv2 sandbox probe</h2>
<pre id="log" style="background:#0b0e12;padding:10px;min-height:60px"></pre>
<script>document.getElementById('log').textContent =
  'rendered ' + new Date().toISOString();</script>
</body>)html";

using CreateEnvFn = HRESULT(STDMETHODCALLTYPE*)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, void*);

template <typename T>
struct HandlerBase : T {
  HandlerBase() = default;
  LONG m_ref{1};
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown) { *ppv = static_cast<T*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
  }
};

// 状态落盘（隐藏窗/沙箱场景下窗口标题读不到, 用文件判读）; 记录线程所在桌面名
static void WriteState(const wchar_t* state, unsigned hr, unsigned where) {
  wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\')) + L"\\probe_state.txt";
  wchar_t desk[64] = L"?"; DWORD need = 0;
  HDESK d = GetThreadDesktop(GetCurrentThreadId());
  GetUserObjectInformationW(d, UOI_NAME, desk, 64, &need);
  FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"w");
  if (f) {
    fwprintf(f, L"mode=%s\nstate=%s\nhr=0x%08X\nwhere=%u\ndesktop=%s\npid=%lu\n",
             g_dual ? L"dual" : L"minimal", state, hr, where, desk, GetCurrentProcessId());
    fclose(f);
  }
}

static void Fail(unsigned hr, unsigned where) {
  WriteState(L"FAIL", hr, where);
  PostMessageW(g_hwnd, WM_APP + 2, (WPARAM)hr, (LPARAM)where);
}
static void Ready() { WriteState(L"READY", 0, 0); PostMessageW(g_hwnd, WM_APP + 3, 0, 0); }

// ------------------------- minimal 模式（默认） -------------------------
struct MinCtrlHandler : HandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* ctrl) override {
    if (FAILED(result) || !ctrl) { Fail((unsigned)result, 1); return S_OK; }
    g_controller = ctrl;
    ctrl->get_CoreWebView2(&g_webview);
    RECT rc; GetClientRect(g_hwnd, &rc);
    ctrl->put_Bounds(rc);
    ctrl->put_IsVisible(g_novis ? FALSE : TRUE);
    if (g_webview) g_webview->NavigateToString(kHtml);
    Ready();
    return S_OK;
  }
};
static MinCtrlHandler g_minCtrl;
struct MinEnvHandler : HandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
    if (FAILED(result) || !env) { Fail((unsigned)result, 0); return S_OK; }
    g_env = env;
    return env->CreateCoreWebView2Controller(g_hwnd, &g_minCtrl);
  }
};
static MinEnvHandler  g_minEnv;

// ------------------------- dual 模式 -------------------------
static void PumpQuit() { PostThreadMessageW(g_aThreadId, WM_QUIT, 0, 0); }
struct DualCtrlHandler : HandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* ctrl) override {
    if (FAILED(result) || !ctrl) { Fail((unsigned)result, 21); PumpQuit(); return S_OK; }
    g_controller = ctrl;
    ctrl->get_CoreWebView2(&g_webview);
    RECT rc; GetClientRect(g_hwnd, &rc);
    ctrl->put_Bounds(rc);
    ctrl->put_IsVisible(TRUE);
    if (g_webview) g_webview->NavigateToString(kHtml);
    Ready();
    PumpQuit();
    return S_OK;
  }
};
static DualCtrlHandler g_dualCtrl;
struct DualEnvHandler : HandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
    if (FAILED(result) || !env) { Fail((unsigned)result, 20); PumpQuit(); return S_OK; }
    g_env = env;
    return env->CreateCoreWebView2Controller(g_hwnd, &g_dualCtrl);
  }
};
static DualEnvHandler  g_dualEnv;

static CreateEnvFn LoadLoader() {
  wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
  std::wstring base(dir); base = base.substr(0, base.find_last_of(L'\\'));
  for (auto p : { base + L"\\runtime\\WebView2Loader.dll", base + L"\\WebView2Loader.dll" }) {
    HMODULE mod = LoadLibraryW(p.c_str());
    if (!mod) continue;
    auto fn = (CreateEnvFn)GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
    if (fn) return fn;
  }
  return nullptr;
}

static std::wstring ExeDir() {
  wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
  std::wstring base(dir); return base.substr(0, base.find_last_of(L'\\'));
}

// dual: 专用线程切到 Default 桌面做 WebView2 创建（等价注入改 lpDesktop 的终态）
static DWORD WINAPI WebViewThread(LPVOID) {
  g_aThreadId = GetCurrentThreadId();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HDESK d = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE |
                         DESKTOP_HOOKCONTROL | DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD |
                         DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS);
  if (!d || !SetThreadDesktop(d)) { Fail(GetLastError(), 10); return 1; }
  CreateEnvFn createEnv = LoadLoader();
  if (!createEnv) { Fail(0xFFFFFFFF, 11); return 1; }
  const std::wstring udf = ExeDir() + (g_dual ? L"\\udf-dual" : L"\\udf");
  HRESULT hr = createEnv(nullptr, udf.c_str(), nullptr, &g_dualEnv);
  if (FAILED(hr)) { Fail((unsigned)hr, 12); return 1; }
  MSG m;                                  // 完成回调经本线程 STA 消息泵派发
  while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  return 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_APP + 2: {
      wchar_t buf[128];
      wsprintfW(buf, L"FAIL hr=0x%08X where=%u", (unsigned)wp, (unsigned)lp);
      SetWindowTextW(h, buf);
      return 0;
    }
    case WM_APP + 3:
      KillTimer(h, 1);
      SetWindowTextW(h, L"READY");
      return 0;
    case WM_TIMER:
      if (wp == 1 && g_flashhide) {           // 3 秒后隐藏（keep-alive 存续实验）
        KillTimer(h, 1);
        ShowWindow(h, SW_HIDE);
        SetTimer(h, 2, 27000, nullptr);       // 补足 30s STUCK 判定
      } else if (wp == 1 && g_cloak) {        // 3 秒后 DWM cloak（win32 仍可见, 用户看不见）
        KillTimer(h, 1);
        BOOL on = TRUE;
        HRESULT hrC = DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on));
        wchar_t bt[128];
        wsprintfW(bt, L"cloaked hr=0x%08X", (unsigned)hrC);
        SetWindowTextW(h, bt);
        SetTimer(h, 2, 27000, nullptr);
      } else if (wp == 2 || (wp == 1 && !g_flashhide && !g_cloak)) {
        WriteState(L"STUCK", 0, 0); SetWindowTextW(h, L"STUCK (30s, no callback)");
      }
      return 0;
    case WM_SIZE:
      if (g_controller) { RECT rc; GetClientRect(h, &rc); g_controller->put_Bounds(rc); }
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
  g_dual = cmdLine && wcsstr(cmdLine, L"dual") != nullptr;
  const bool offscreen = cmdLine && wcsstr(cmdLine, L"offscreen") != nullptr;
  const bool minwin   = cmdLine && wcsstr(cmdLine, L"min") != nullptr;
  const bool tinywin  = cmdLine && wcsstr(cmdLine, L"tiny") != nullptr;
  const bool tinyact  = cmdLine && wcsstr(cmdLine, L"tinyact") != nullptr;
  const bool flashhide = cmdLine && wcsstr(cmdLine, L"flashhide") != nullptr;
  const bool cloakwin  = cmdLine && wcsstr(cmdLine, L"cloak") != nullptr;
  const bool novis     = cmdLine && wcsstr(cmdLine, L"novis") != nullptr;
  g_flashhide = flashhide;
  g_cloak = cloakwin;
  g_novis = novis;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSW wc = { sizeof(wc) };
  wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName = L"Wv2SandboxTest";
  RegisterClassW(&wc);
  // offscreen: WS_EX_TOOLWINDOW(不进任务栏) + 屏幕外坐标 —— 对用户不可见,
  // 但 IsWindowVisible()==TRUE, 供"隐藏窗导致浏览器进程自退"假说对照
  // tiny: 1x1 工具窗贴屏幕右下角、z 序最底、不激活 —— 物理上真实显示在屏内
  const bool toolwin = offscreen || tinywin || tinyact;
  const bool tinyish = tinywin || tinyact;
  g_hwnd = CreateWindowExW(toolwin ? WS_EX_TOOLWINDOW : 0, L"Wv2SandboxTest",
      L"wv2sandbox_test running...",
      WS_OVERLAPPEDWINDOW & (tinyish ? ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX : ~0UL),
      tinyish ? GetSystemMetrics(SM_CXSCREEN) - 1 : (offscreen ? -32000 : CW_USEDEFAULT),
      tinyish ? GetSystemMetrics(SM_CYSCREEN) - 1 : (offscreen ? -32000 : CW_USEDEFAULT),
      tinyish ? 1 : CW_USEDEFAULT, tinyish ? 1 : CW_USEDEFAULT,
      nullptr, nullptr, inst, nullptr);
  // tinyact: 与 tiny 同形态但 SW_SHOWNORMAL 激活一次 —— 验证"激活过"是否为
  // 浏览器 keep-alive 的判据（对照: tiny 用 SW_SHOWNOACTIVATE, 从未激活）
  ShowWindow(g_hwnd, offscreen ? SW_SHOWNOACTIVATE
               : (tinyact || flashhide || cloakwin) ? SW_SHOWNORMAL
               : minwin        ? SW_SHOWMINNOACTIVE
               : tinywin       ? SW_SHOWNOACTIVATE
                               : show);
  if (offscreen || tinyish)
    SetWindowPos(g_hwnd, tinyact ? HWND_NOTOPMOST : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (offscreen) SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  UpdateWindow(g_hwnd);
  SetTimer(g_hwnd, 1, 30000, nullptr);
  WriteState(L"running", 0, 0);

  if (g_dual) {
    CreateThread(nullptr, 0, WebViewThread, nullptr, 0, nullptr);
  } else {
    CreateEnvFn createEnv = LoadLoader();
    if (!createEnv) {
      MessageBoxW(g_hwnd, L"WebView2Loader.dll 加载失败（需同目录 runtime\\WebView2Loader.dll）",
                  L"wv2sandbox_test", MB_ICONERROR);
      return 1;
    }
    // 与 musicdl(main.cpp CreateCoreWebView2EnvironmentWithOptions 调用点) 同款: options=nullptr
    HRESULT hr = createEnv(nullptr, (ExeDir() + L"\\udf").c_str(), nullptr, &g_minEnv);
    if (FAILED(hr)) Fail((unsigned)hr, 0);
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  CoUninitialize();
  return 0;
}
