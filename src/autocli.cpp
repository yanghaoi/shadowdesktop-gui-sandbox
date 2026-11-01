// autocli.cpp — 内嵌自动化接口的命令行客户端
//
// 配合 GUI 程序内嵌的 automation.h 管道服务使用。管道名从被测程序输出目录的
// automation_info.json 里读 ("pipe" 字段, 如 ShadowAuto-4208)。
//
// 用法:
//   autocli <pipeName> tree
//   autocli <pipeName> get <name>
//   autocli <pipeName> set <name> <value...>
//   autocli <pipeName> invoke <target>
//   autocli <pipeName> wait <name> <contains|eq|ne|ge|le> <value> <timeout_ms>
//   autocli <pipeName> shot <out.png>
//   autocli <pipeName> sleep <ms>
//   autocli <pipeName> raw <任意请求行...>
//
// 退出码: 0 = ok:true 且 (非 wait 或 matched:true);  3 = ok:false 或未匹配;  4 = 连接/协议错误
// 响应原样打印 (UTF-8 JSON 单行), 供脚本/agent 解析。
//
// 构建:
//   g++ -std=c++17 -O2 -municode -mconsole -DNOMINMAX -DUNICODE -D_UNICODE
//       autocli.cpp -o autocli.exe -luser32 -ladvapi32 -static
#include <windows.h>

#include <stdio.h>
#include <string>
#include <vector>

static std::wstring Utf8ToWide(const std::string& b)
{
    if (b.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), NULL, 0);
    std::wstring w((size_t)n, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                NULL, 0, NULL, NULL);
    std::string s((size_t)n, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        fwprintf(stderr,
            L"usage: autocli <pipeName> <op> [args...]\n"
            L"  ops: tree | get <name> | set <name> <value...> | invoke <target>\n"
            L"       wait <name> <contains|eq|ne|ge|le> <value> <timeout_ms>\n"
            L"       shot <out.png> | sleep <ms> | raw <line...>\n"
            L"pipeName: see automation_info.json \"pipe\" field, e.g. ShadowAuto-4208\n");
        return 4;
    }

    // 组请求行: op + 参数原样拼接。空参数显式写成 "" —— 服务端 Tokenize
    // 认引号 token, 否则空值会被空白切分丢弃 (set exefilter "" 之类的用例)
    std::wstring line;
    for (int i = 2; i < argc; ++i) {
        if (i > 2) line += L" ";
        if (argv[i][0] == L'\0') line += L"\"\"";
        else line += argv[i];
    }

    std::wstring full = L"\\\\.\\pipe\\";
    full += argv[1];

    // 连接 (服务可能正在处理上一条 wait 命令, 最多等 20s)
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40; ++attempt) {
        h = CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(full.c_str(), 1000);
        else Sleep(500);
    }
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"[!] cannot connect pipe '%ls' (err=%lu)\n",
                 full.c_str(), GetLastError());
        return 4;
    }

    std::string req = WideToUtf8(line) + "\n";
    DWORD written = 0;
    if (!WriteFile(h, req.c_str(), (DWORD)req.size(), &written, NULL)) {
        fwprintf(stderr, L"[!] pipe write failed (err=%lu)\n", GetLastError());
        CloseHandle(h);
        return 4;
    }

    // 读响应直到换行或对端关闭 (invoke quit 时对端进程退出, 读到 EOF 属预期)
    std::string resp;
    char ch;
    DWORD got = 0;
    while (ReadFile(h, &ch, 1, &got, NULL) && got == 1) {
        if (ch == '\n') break;
        resp += ch;
    }
    CloseHandle(h);

    DWORD e = GetLastError();
    if (resp.empty() && e != ERROR_BROKEN_PIPE) {
        fwprintf(stderr, L"[!] pipe read failed (err=%lu)\n", e);
        return 4;
    }
    if (resp.empty()) {
        // 对端已退出 (如 invoke quit), 无响应可读
        printf("{\"ok\": true, \"note\": \"peer exited before response\"}\n");
        return 0;
    }

    fwrite(resp.c_str(), 1, resp.size(), stdout);
    printf("\n");

    // 依据响应前缀决定退出码
    if (resp.compare(0, 10, "{\"ok\":true") == 0) return 0;
    return 3;
}
