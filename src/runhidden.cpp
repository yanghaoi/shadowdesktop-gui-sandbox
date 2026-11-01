// runhidden.cpp — 零窗口执行器（ShadowDesktop 技能组件）
//
// 用法:
//   runhidden.exe <命令行...>      隐藏执行并等待退出, 透传子进程退出码
//   runhidden.exe -e <命令行...>   同上, 但以提权运行 (ShellExecuteEx "runas";
//                                  UAC 开启时确认框仍会出现, 属安全桌面机制)
//
// 原理: 自身为 GUI 子系统 (无控制台), 子进程以 CREATE_NO_WINDOW /
//       SW_HIDE 创建 —— 全程零窗口、零闪现, 用户桌面完全无感。
// 约定: 子进程 stdout 不回传, 输出用 Out-File/重定向落盘 (ShadowDesktop 约定)。
// 构建: g++ -O2 -municode -mwindows runhidden.cpp -o runhidden.exe -lshell32 -luser32 -static

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

// 把单个 argv 还原为带引号规则的命令行 token (含空格加引号, 内部引号翻倍)
static std::wstring QuoteArg(const wchar_t* a)
{
    std::wstring s = a, out;
    bool need = s.find(L' ') != std::wstring::npos || s.find(L'\t') != std::wstring::npos ||
                s.find(L'"') != std::wstring::npos || s.empty();
    if (!need) return s;
    out += L'"';
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'"') out += L"\"\"";
        else out += s[i];
    }
    out += L'"';
    return out;
}

static std::wstring JoinArgs(int argc, wchar_t** argv, int start)
{
    std::wstring cmd;
    for (int i = start; i < argc; i++) {
        if (i > start) cmd += L' ';
        cmd += QuoteArg(argv[i]);
    }
    return cmd;
}

// 取第一段作为 lpFile, 其余作为 lpParameters (ShellExecuteEx 用)
static void SplitFirst(const std::wstring& cmd, std::wstring* file, std::wstring* rest)
{
    size_t p = 0;
    std::wstring first;
    if (cmd[0] == L'"') {
        p = cmd.find(L'"', 1);
        if (p == std::wstring::npos) { *file = cmd; return; }
        first = cmd.substr(1, p - 1);
        p++;                                   // 跳过收引号
        while (p < cmd.size() && cmd[p] == L' ') p++;
    } else {
        p = cmd.find(L' ');
        if (p == std::wstring::npos) { *file = cmd; return; }
        first = cmd.substr(0, p);
        while (p < cmd.size() && cmd[p] == L' ') p++;
    }
    *file = first;
    *rest = cmd.substr(p);
}

static DWORD RunHidden(const std::wstring& exe, const std::wstring& args, BOOL elev)
{
    if (elev) {   // 提权: ShellExecuteEx runas (SW_HIDE, 等待, 透传退出码)
        SHELLEXECUTEINFOW sei;
        ZeroMemory(&sei, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_DDEWAIT;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            DWORD e = GetLastError();
            if (e == ERROR_CANCELLED) return 1602;   // 用户取消 UAC
            return 1601;                              // 提权启动失败
        }
        if (sei.hProcess == NULL) return 1603;
        DWORD ec = 1;
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &ec);
        CloseHandle(sei.hProcess);
        return ec;
    }
    // 普通模式: CreateProcess + CREATE_NO_WINDOW
    std::wstring full = exe + (args.empty() ? L"" : L" ") + args;
    std::vector<wchar_t> buf(full.begin(), full.end());
    buf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        if (e == ERROR_ELEVATION_REQUIRED) return 740;   // 需要提权: 改用 -e
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 3 : 1604;
    }
    DWORD ec = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ec;
}

int wmain(int argc, wchar_t** argv)
{
    BOOL elev = FALSE;
    int start = 1;
    if (argc > 1 && wcscmp(argv[1], L"-e") == 0) { elev = TRUE; start = 2; }
    if (argc <= start) {
        MessageBoxW(NULL,
            L"runhidden — 零窗口执行器\n\n"
            L"用法: runhidden.exe [-e] <命令行...>\n"
            L"  默认: CREATE_NO_WINDOW 隐藏执行, 等待退出, 透传退出码\n"
            L"  -e  : 提权运行 (runas; UAC 开启时确认框不可隐藏)\n\n"
            L"约定: 输出由被执行脚本 Out-File/重定向落盘。\n"
            L"退出码: 子进程透传; 740=需提权(-e), 1601/1602=提权失败/取消, 3=路径未找到",
            L"runhidden", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    std::wstring exe, rest;
    SplitFirst(JoinArgs(argc, argv, start), &exe, &rest);
    return RunHidden(exe, rest, elev);
}
