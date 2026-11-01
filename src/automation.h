// automation.h — 内嵌自动化服务模块 (CDP 风格: 命名管道 + 行协议 + JSON 响应)
//
// 设计目标: 自研 GUI 程序 include 本头文件 + 调用 StartAutomation() 即获得
// "语义化测试接口"。测试会话通过命名管道直接 查询状态/调用处理函数/等待条件,
// 无需截图看图、无需模拟鼠标键盘。命名管道不受 Windows 桌面对象隔离影响,
// 因此与 ShadowDesktop 沙箱桌面方案完全正交: 程序在沙箱桌面运行, 会话在
// 用户桌面通过管道驱动它。
//
// 线程模型: 服务线程收命令 -> 直接在管道线程执行
//   - 查询类用跨线程安全的 API (GetWindowText/GetWindowRect/SendMessageTimeout)
//   - 动作类用 PostMessage(WM_COMMAND) 投给 UI 线程执行 (与真实点击同路径)
//   - 若 UI 线程挂死, 查询经由 SendMessageTimeout 限时降级, 不会吊死测试会话
//
// 使用方法 (在被测 GUI 的 cpp 中):
//   #include "automation.h"
//   void AutoDispatch(const std::vector<std::wstring>& tok,
//                     const std::wstring& raw, std::wstring& out) {
//       // ... 先处理程序业务命令, 没接住的落到通用层:
//       AutoCommonHooks hk;
//       hk.getMain      = AutoMainWnd;      // 主窗口句柄来源
//       hk.getValue     = MyGetValue;       // 业务命名值 (get/wait 用, 可 NULL)
//       hk.invokeTarget = MyInvokeName;     // 业务入口名 -> 命令 ID (可 NULL)
//       hk.treeAppInfo  = MyTreeExtras;     // tree 的 app 块附加字段 (可 NULL)
//       if (AutoCommonDispatch(tok, out, hk)) return;
//       out = L"{\"ok\":false,\"error\":\"unknown op\"}";
//   }
//   StartAutomation(L"ShadowAuto", outDir, desktopName);  // 创建管道线程
//
// 通用层内置 op (所有接入程序免费获得, 见 AutoCommonDispatch):
//   tree | get <name> | wait <name> <op> <val> <ms> | set text <id> <v...>
//   gettext <id|hwnd> | combo <id> list|sel|select <idx|文本>
//   check <id> get|set <0|1|2> | click <id|hwnd> | find <class|text|id> <值>
//   ctrl(s) <id> <msg> <wp> <lp> | key <id> <字符...> | invoke <名|数字ID> | sleep
// 内置命名值: title(主窗口)/pid/desktop; 其余名字经 hk.getValue 查询。
//
// MSVC 兼容性 (MFC 程序接入实测, 2026-10-06):
//   - 本头文件必须保存为 UTF-8 WITH BOM —— 无 BOM 时 MSVC 按代码页 936 误读
//     中文注释, 触发一连串与真实代码无关的语法错误 (C2530/C2143/C2226);
//   - g_pipeName 用 static 而非 inline 变量 —— MSVC 默认 C++14 不支持
//     inline 变量 (C7525); 服务端全部函数自包含于本头文件, 各 TU 独立副本
//     互不交互, static 语义等价;
//   - 需显式 #include <stdlib.h>/<wctype.h> (wcstoull/iswspace, MSVC 不经
//     <string> 间接声明)。
//
// 协议: 请求一行 (UTF-8, 按空白切分 token; 支持 "带引号 token" 表达空串/内嵌
// 空格)。响应一行 JSON (UTF-8)。
// 接口发现: StartAutomation 会写 <outDir>\automation_info.json
//   {"pipe":"<name>-<pid>","pid":N,"desktop":"..."}
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>   // wcstoull/wcstol(MSVC 不经 <string> 间接声明)
#include <wctype.h>   // iswspace(Tokenize 用,MSVC 需显式包含)

// ------------------------------------------------------- JSON 输出辅助

inline void JsonEsc(std::wstring& o, const wchar_t* s)
{
    o += L'"';
    for (const wchar_t* p = s; *p; ++p) {
        switch (*p) {
        case L'"':  o += L"\\\"";  break;
        case L'\\': o += L"\\\\";  break;
        case L'\n': o += L"\\n";   break;
        case L'\r': o += L"\\r";   break;
        case L'\t': o += L"\\t";   break;
        default:
            if (*p < 0x20) { wchar_t b[8]; _snwprintf(b, 8, L"\\u%04x", *p); o += b; }
            else o += *p;
        }
    }
    o += L'"';
}

// "key": "value"
inline void JsonKV(std::wstring& o, const wchar_t* key, const wchar_t* val)
{
    wchar_t b[512];
    _snwprintf(b, 512, L"\"%ls\": ", key);
    o += b;
    JsonEsc(o, val);
}

// "key": N
inline void JsonKN(std::wstring& o, const wchar_t* key, long long v)
{
    wchar_t b[80];
    _snwprintf(b, 80, L"\"%ls\": %lld", key, v);
    o += b;
}

// ------------------------------------------------------- 编码转换

inline std::string AutoWideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                NULL, 0, NULL, NULL);
    std::string s((size_t)n, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                            &s[0], n, NULL, NULL);
    return s;
}

inline std::wstring AutoUtf8ToWide(const std::string& b)
{
    if (b.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), NULL, 0);
    std::wstring w((size_t)n, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), &w[0], n);
    return w;
}

// ------------------------------------------------------- 被测程序需实现的入口

// 处理一条命令。tok = 按空白切分的请求 token (tok[0] 为 op), raw = 原始行。
// 把 JSON 响应追加到 out (不带尾换行)。响应首个字段约定为 "ok":true/false。
// 典型实现: 先处理业务命令, 再一行 AutoCommonDispatch(tok, out, hk) 兜底通用 op。
void AutoDispatch(const std::vector<std::wstring>& tok,
                  const std::wstring& raw, std::wstring& out);

// ------------------------------------------------------- 窗口查询/定位辅助

// 取窗口文本, 限时 2s —— UI 线程挂死时只超时, 不吊死调用者。
// (GetWindowTextW 同进程跨线程无超时机制, 挂死场景会卡住管道线程, 勿用)
inline bool AutoSafeGetText(HWND h, wchar_t* buf, int cch)
{
    buf[0] = 0;
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(h, WM_GETTEXT, (WPARAM)cch, (LPARAM)buf,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &res);
    buf[cch - 1] = 0;
    return ok != 0;
}

// 枚举"当前进程在当前桌面的全部顶层窗口" —— 含程序弹出的对话框/MDI 子框架,
// 沙箱桌面上没有 shell 干扰, 枚举结果即被测程序自己的窗口。
inline void AutoTopWindows(std::vector<HWND>& out)
{
    struct Ctx { DWORD pid; std::vector<HWND>* v; } c;
    c.pid = GetCurrentProcessId();
    c.v = &out;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid) c->v->push_back(h);
        return TRUE;
    }, (LPARAM)&c);
}

// 递归收集全部后代 (EnumChildWindows 深度遍历, 含孙级)
inline void AutoDescendants(HWND root, std::vector<HWND>& out)
{
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        ((std::vector<HWND>*)lp)->push_back(h);
        return TRUE;
    }, (LPARAM)&out);
}

// 统一窗口定位: "h<十进制hwnd>" 直接句柄; "<十进制id>" 先按控件 ID 搜
// 全部窗口后代, 找不到再把数字当 hwnd 兜底。outId 可空, 返回时为目标 ID。
// 两段式: 先只在"可见窗口"(IsWindowVisible 含祖先链)里找——隐藏弹窗(如带
// WS_EX_TOPMOST 的时间设置小窗)常驻 z-order 顶部, 单遍 z-order 搜索会被
// 隐藏控件遮蔽可见控件; 可见过滤无命中再退回全量(旧行为兜底,结果兼容)。
inline HWND AutoResolveSpec(const std::wstring& spec, LONG* outId)
{
    if (outId) *outId = 0;
    if (spec.size() > 1 && (spec[0] == L'h' || spec[0] == L'H')) {
        HWND h = (HWND)(uintptr_t)wcstoull(spec.c_str() + 1, NULL, 10);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && IsWindow(h)) {
            if (outId) *outId = GetWindowLongW(h, GWL_ID);
            return h;
        }
        return NULL;
    }
    LONG id = wcstol(spec.c_str(), NULL, 10);
    std::vector<HWND> tops;
    AutoTopWindows(tops);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < tops.size(); ++i) {
            if (pass == 0 && !IsWindowVisible(tops[i])) continue;
            if (GetWindowLongW(tops[i], GWL_ID) == id) {
                if (outId) *outId = id;
                return tops[i];
            }
            std::vector<HWND> desc;
            AutoDescendants(tops[i], desc);
            for (size_t k = 0; k < desc.size(); ++k) {
                if (pass == 0 && !IsWindowVisible(desc[k])) continue;
                if (GetWindowLongW(desc[k], GWL_ID) == id) {
                    if (outId) *outId = id;
                    return desc[k];
                }
            }
        }
    }
    HWND h = (HWND)(uintptr_t)wcstoull(spec.c_str(), NULL, 10);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindow(h)) {
        if (outId) *outId = GetWindowLongW(h, GWL_ID);
        return h;
    }
    return NULL;
}

// 在本进程全部窗口(含后代)中按字段查找, field = class | text | id (子串匹配)
inline void AutoFindWindows(const std::wstring& field, const std::wstring& value,
                            std::vector<HWND>& out)
{
    std::vector<HWND> tops;
    AutoTopWindows(tops);
    for (size_t i = 0; i < tops.size(); ++i) {
        std::vector<HWND> group;
        group.push_back(tops[i]);
        AutoDescendants(tops[i], group);
        for (size_t k = 0; k < group.size(); ++k) {
            HWND h = group[k];
            wchar_t b[256];
            bool m = false;
            if (field == L"class") {
                GetClassNameW(h, b, 256);
                m = wcsstr(b, value.c_str()) != NULL;
            } else if (field == L"text") {
                AutoSafeGetText(h, b, 256);
                m = wcsstr(b, value.c_str()) != NULL;
            } else if (field == L"id") {
                m = (LONG)wcstol(value.c_str(), NULL, 10) == GetWindowLongW(h, GWL_ID);
            }
            if (m && out.size() < 50) out.push_back(h);
        }
    }
}

// 当前线程所在桌面名 (进程被启动到哪个桌面, 所有线程就属于哪个桌面)
inline void AutoGetDesktopName(wchar_t* buf, int cch)
{
    lstrcpynW(buf, L"?", cch);
    HDESK hdesk = GetThreadDesktop(GetCurrentThreadId());
    if (hdesk) GetUserObjectInformationW(hdesk, UOI_NAME, buf, cch, NULL);
}

// ------------------------------------------------------- 通用控件操作辅助
// (AutoCommonDispatch 的实现件, 业务层也可直接复用)

// 编辑控件设文本后按真实输入路径补发 EN_CHANGE
// (依赖 EN_CHANGE 联动的输入框——如筛选框防抖——程序化设值后照常触发)
inline void AutoEditText(HWND h, const std::wstring& text)
{
    SendMessageTimeoutW(h, WM_SETTEXT, 0, (LPARAM)text.c_str(),
                        SMTO_ABORTIFHUNG, 3000, NULL);
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (wcsstr(cls, L"Edit")) {
        HWND p = GetParent(h);
        if (p) PostMessageW(p, WM_COMMAND,
            MAKEWPARAM((WORD)GetWindowLongW(h, GWL_ID), EN_CHANGE), (LPARAM)h);
    }
}

// 复选框置位 (BM_SETCHECK) 并按真实点击路径通知父窗口 (BN_CLICKED),
// 状态处理器读到的是新状态, 与人工点击语义一致
inline void AutoCheckSet(HWND h, int state)
{
    SendMessageTimeoutW(h, BM_SETCHECK, (WPARAM)state, 0, SMTO_ABORTIFHUNG, 3000, NULL);
    HWND p = GetParent(h);
    if (p) PostMessageW(p, WM_COMMAND,
        MAKEWPARAM((WORD)GetWindowLongW(h, GWL_ID), BN_CLICKED), (LPARAM)h);
}

// 读下拉当前选中项文本 (CB_GETCURSEL + CB_GETLBTEXT; 同进程 SendMessage,缓冲区有效)
inline bool AutoComboSelText(HWND hCombo, std::wstring& out)
{
    DWORD_PTR sel = 0;
    if (!SendMessageTimeoutW(hCombo, CB_GETCURSEL, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &sel) || (LONG)sel == CB_ERR)
        return false;
    wchar_t buf[512] = L"";
    if (!SendMessageTimeoutW(hCombo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, NULL))
        return false;
    buf[511] = 0;
    out = buf;
    return true;
}

// 枚举下拉全部项 (含选中索引), 输出 JSON
inline void AutoComboList(HWND hCombo, std::wstring& o)
{
    DWORD_PTR r = 0;
    int count = 0;
    if (SendMessageTimeoutW(hCombo, CB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r))
        count = (int)r;
    DWORD_PTR sel = CB_ERR;
    if (SendMessageTimeoutW(hCombo, CB_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r))
        sel = r;
    o = L"{\"ok\": true";
    o += L", \"count\": " + std::to_wstring(count);
    o += L", \"cursel\": " + std::to_wstring((long long)(int)sel);
    o += L", \"items\": [";
    for (int i = 0; i < count && i < 512; ++i) {
        wchar_t buf[512] = L"";
        if (i) o += L", ";
        if (SendMessageTimeoutW(hCombo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, NULL)) {
            buf[511] = 0;
            JsonEsc(o, buf);
        } else {
            o += L"\"\"";
        }
    }
    o += L"]}";
}

// 下拉选中: 数字=按索引, 否则按项文本精确匹配; 选中后按真实选择路径
// 向父窗口投 CBN_SELCHANGE (联动处理器照常触发)。失败时 err 给出原因。
inline bool AutoComboSelect(HWND hCombo, const std::wstring& want, std::wstring& err)
{
    wchar_t cls[64];
    GetClassNameW(hCombo, cls, 64);
    if (!wcsstr(cls, L"ComboBox")) { err = L"target is not a combobox"; return false; }
    DWORD_PTR r = 0;
    int count = 0;
    if (SendMessageTimeoutW(hCombo, CB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r))
        count = (int)r;
    int hit = -1;
    if (!want.empty() && want.find_first_not_of(L"0123456789") == std::wstring::npos) {
        int idx = _wtoi(want.c_str());
        if (idx >= 0 && idx < count) hit = idx;
    } else {
        for (int i = 0; i < count; ++i) {
            wchar_t buf[512] = L"";
            if (!SendMessageTimeoutW(hCombo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, NULL)) continue;
            buf[511] = 0;
            if (!wcscmp(buf, want.c_str())) { hit = i; break; }
        }
    }
    if (hit < 0) { err = L"item not found"; return false; }
    SendMessageTimeoutW(hCombo, CB_SETCURSEL, (WPARAM)hit, 0, SMTO_ABORTIFHUNG, 3000, NULL);
    HWND p = GetParent(hCombo);
    if (p) PostMessageW(p, WM_COMMAND,
        MAKEWPARAM((WORD)GetWindowLongW(hCombo, GWL_ID), CBN_SELCHANGE), (LPARAM)hCombo);
    return true;
}

// get/wait 共用的比较器
inline bool AutoCompareVal(const std::wstring& cur, const std::wstring& op,
                           const std::wstring& want)
{
    if (op == L"contains") return wcsstr(cur.c_str(), want.c_str()) != NULL;
    if (op == L"eq") return wcscmp(cur.c_str(), want.c_str()) == 0;
    if (op == L"ne") return wcscmp(cur.c_str(), want.c_str()) != 0;
    long long c = wcstoll(cur.c_str(), NULL, 10);
    long long w = wcstoll(want.c_str(), NULL, 10);
    if (op == L"ge") return c >= w;
    if (op == L"le") return c <= w;
    return false;
}

// 递归输出窗口节点 (class/text/id/rect/visible/enabled + children 嵌套)
inline void AutoDumpNode(HWND h, std::wstring& o, bool* first)
{
    if (!*first) o += L", ";
    *first = false;

    wchar_t cls[64], txt[256];
    GetClassNameW(h, cls, 64);
    AutoSafeGetText(h, txt, 256);
    RECT r;
    GetWindowRect(h, &r);

    o += L"{";
    JsonKV(o, L"class", cls);
    o += L", ";
    JsonKV(o, L"text", txt);
    o += L", ";
    JsonKN(o, L"id", GetWindowLongW(h, GWL_ID));
    o += L", \"hwnd\": " + std::to_wstring((unsigned long long)(uintptr_t)h);
    o += L", \"rect\": [" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) +
         L"," + std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom) + L"]";
    o += L", ";
    JsonKN(o, L"visible", IsWindowVisible(h) ? 1 : 0);
    o += L", ";
    JsonKN(o, L"enabled", IsWindowEnabled(h) ? 1 : 0);

    std::vector<HWND> kids;
    for (HWND c = NULL; (c = FindWindowExW(h, c, NULL, NULL)) != NULL; )
        kids.push_back(c);
    if (!kids.empty()) {
        o += L", \"children\": [";
        bool f = true;
        for (size_t i = 0; i < kids.size(); ++i) AutoDumpNode(kids[i], o, &f);
        o += L"]";
    }
    o += L"}";
}

// ------------------------------------------------------- 通用命令层
// 与具体程序无关的 op 全部实现在这里。接入程序的 AutoDispatch 先处理业务命令,
// 没接住的一行兜底: if (AutoCommonDispatch(tok, out, hk)) return;
// 协议兼容性: 新增 op 只增不改; 未知 op 由调用方输出 ok:false。

struct AutoCommonHooks {
    HWND (*getMain)();                                        // 主窗口 (invoke/tree/wait:title)
    bool (*getValue)(const wchar_t* name, std::wstring& val); // 业务命名值 (可 NULL)
    int  (*invokeTarget)(const wchar_t* name);                // 业务入口名 -> 命令 ID (可 NULL)
    void (*treeAppInfo)(std::wstring& o);                     // tree app 块附加字段 (可 NULL)
};

// get/wait 共用的取值: hook 命名值优先, 内置 title/pid/desktop 兜底
inline bool AutoCommonGetValue(const AutoCommonHooks& hk, const wchar_t* name,
                               std::wstring& val)
{
    if (hk.getValue && hk.getValue(name, val)) return true;
    wchar_t b[256];
    if (!wcscmp(name, L"title")) {
        HWND m = hk.getMain ? hk.getMain() : NULL;
        if (!m) return false;
        AutoSafeGetText(m, b, 256); val = b; return true;
    }
    if (!wcscmp(name, L"pid")) { val = std::to_wstring(GetCurrentProcessId()); return true; }
    if (!wcscmp(name, L"desktop")) { AutoGetDesktopName(b, 64); val = b; return true; }
    return false;
}

// 返回 true = 已处理 (out 已写响应); false = op 不归通用层 (调用方出 unknown op)
inline bool AutoCommonDispatch(const std::vector<std::wstring>& tok,
                               std::wstring& out, const AutoCommonHooks& hk)
{
    const std::wstring& op = tok[0];

    if (op == L"tree") {
        out = L"{\"ok\":true";
        out += L", \"app\": {";
        wchar_t b[64];
        AutoGetDesktopName(b, 64);
        JsonKV(out, L"desktop", b);
        out += L", ";
        JsonKN(out, L"pid", GetCurrentProcessId());
        if (hk.treeAppInfo) hk.treeAppInfo(out);
        out += L"}";
        out += L", \"windows\": [";
        std::vector<HWND> tops;
        AutoTopWindows(tops);
        bool first = true;
        for (size_t i = 0; i < tops.size(); ++i) AutoDumpNode(tops[i], out, &first);
        out += L"]}";
        return true;
    }

    if (op == L"get" && tok.size() >= 2) {
        std::wstring v;
        if (!AutoCommonGetValue(hk, tok[1].c_str(), v)) {
            out = L"{\"ok\":false,\"error\":\"unknown name\"}";
            return true;
        }
        out = L"{\"ok\":true";
        out += L", ";
        JsonKV(out, L"name", tok[1].c_str());
        out += L", ";
        JsonKV(out, L"value", v.c_str());
        out += L"}";
        return true;
    }

    if (op == L"wait" && tok.size() >= 5) {
        int timeout = _wtoi(tok[4].c_str());
        if (timeout <= 0) timeout = 10000;
        ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout;
        std::wstring cur;
        bool have = false, matched = false;
        for (;;) {
            have = AutoCommonGetValue(hk, tok[1].c_str(), cur);
            matched = have && AutoCompareVal(cur, tok[2], tok[3]);
            if (matched || GetTickCount64() >= deadline) break;
            Sleep(200);
        }
        out = L"{\"ok\":true";
        out += L", ";
        JsonKN(out, L"matched", matched ? 1 : 0);
        out += L", ";
        JsonKV(out, L"value", cur.c_str());
        out += L"}";
        return true;
    }

    if (op == L"set" && tok.size() >= 4 && tok[1] == L"text") {
        // set text <id|hwnd> <值...>: 任意控件 WM_SETTEXT (Edit 补发 EN_CHANGE)
        HWND h = AutoResolveSpec(tok[2], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        std::wstring text;
        for (size_t i = 3; i < tok.size(); ++i) {
            if (i > 3) text += L' ';
            text += tok[i];
        }
        AutoEditText(h, text);
        out = L"{\"ok\":true";
        out += L", ";
        JsonKV(out, L"text", text.c_str());
        out += L"}";
        return true;
    }

    // gettext <id|hwnd>: 任意控件文本读回 (set text 的反操作, 断言用)
    if (op == L"gettext" && tok.size() >= 2) {
        HWND h = AutoResolveSpec(tok[1], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        wchar_t b[1024];
        AutoSafeGetText(h, b, 1024);
        out = L"{\"ok\":true";
        out += L", ";
        JsonKV(out, L"text", b);
        out += L"}";
        return true;
    }

    if (op == L"combo" && tok.size() >= 3) {
        HWND h = AutoResolveSpec(tok[1], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        if (tok[2] == L"list") { AutoComboList(h, out); return true; }
        if (tok[2] == L"sel") {
            std::wstring v;
            bool ok = AutoComboSelText(h, v);
            out = L"{\"ok\":";
            out += ok ? L"true" : L"false";
            if (ok) { out += L", "; JsonKV(out, L"sel", v.c_str()); }
            out += L"}";
            return true;
        }
        if (tok[2] == L"select" && tok.size() >= 4) {
            std::wstring want;
            for (size_t i = 3; i < tok.size(); ++i) {
                if (i > 3) want += L' ';
                want += tok[i];
            }
            std::wstring err;
            if (!AutoComboSelect(h, want, err)) {
                out = L"{\"ok\":false";
                out += L", ";
                JsonKV(out, L"error", err.c_str());
                out += L"}";
                return true;
            }
            std::wstring v;
            AutoComboSelText(h, v);
            out = L"{\"ok\":true";
            out += L", ";
            JsonKV(out, L"sel", v.c_str());
            out += L"}";
            return true;
        }
        out = L"{\"ok\":false,\"error\":\"usage: combo <id> list|sel|select <idx|text>\"}";
        return true;
    }

    if (op == L"check" && tok.size() >= 3) {
        HWND h = AutoResolveSpec(tok[1], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        if (tok[2] == L"get") {
            DWORD_PTR r = 0;
            SendMessageTimeoutW(h, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r);
            out = L"{\"ok\":true, \"state\": " + std::to_wstring((long long)r) + L"}";
            return true;
        }
        if (tok[2] == L"set" && tok.size() >= 4) {
            int st = _wtoi(tok[3].c_str());
            if (st < 0 || st > 2) st = 0;
            AutoCheckSet(h, st);
            out = L"{\"ok\":true, \"state\": " + std::to_wstring((long long)st) + L"}";
            return true;
        }
        out = L"{\"ok\":false,\"error\":\"usage: check <id> get|set <0|1|2>\"}";
        return true;
    }

    if (op == L"click" && tok.size() >= 2) {
        LONG id = 0;
        HWND h = AutoResolveSpec(tok[1], &id);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (wcsstr(cls, L"utton")) {
            PostMessageW(h, BM_CLICK, 0, 0); // 按钮自己会向父窗口发 WM_COMMAND
        } else {
            HWND p = GetParent(h);
            if (p) PostMessageW(p, WM_COMMAND, MAKEWPARAM((WORD)id, BN_CLICKED), (LPARAM)h);
            else   PostMessageW(h, BM_CLICK, 0, 0);
        }
        out = L"{\"ok\":true";
        out += L", ";
        JsonKV(out, L"clicked", cls);
        out += L", ";
        JsonKN(out, L"id", id);
        out += L"}";
        return true;
    }

    if (op == L"find" && tok.size() >= 3) {
        std::wstring value;
        for (size_t i = 2; i < tok.size(); ++i) {
            if (i > 2) value += L' ';
            value += tok[i];
        }
        std::vector<HWND> hits;
        AutoFindWindows(tok[1], value, hits);
        out = L"{\"ok\":true";
        out += L", ";
        JsonKN(out, L"count", (long long)hits.size());
        out += L", \"hits\": [";
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i) out += L", ";
            wchar_t cls2[64], txt2[256];
            GetClassNameW(hits[i], cls2, 64);
            AutoSafeGetText(hits[i], txt2, 256);
            out += L"{";
            JsonKN(out, L"hwnd", (long long)(uintptr_t)hits[i]);
            out += L", ";
            JsonKN(out, L"id", GetWindowLongW(hits[i], GWL_ID));
            out += L", ";
            JsonKV(out, L"class", cls2);
            out += L", ";
            JsonKV(out, L"text", txt2);
            out += L"}";
        }
        out += L"]}";
        return true;
    }

    if ((op == L"ctrl" || op == L"ctrls") && tok.size() >= 5) {
        HWND h = AutoResolveSpec(tok[1], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        UINT   msg = (UINT)wcstoul(tok[2].c_str(), NULL, 0);   // 支持 0x 十六进制
        WPARAM wp  = (WPARAM)wcstoull(tok[3].c_str(), NULL, 0);
        LPARAM lp  = (LPARAM)(uintptr_t)wcstoull(tok[4].c_str(), NULL, 0);
        if (op == L"ctrl") {
            PostMessageW(h, msg, wp, lp);
            out = L"{\"ok\":true, \"posted\": true}";
        } else {
            // 查询类: 同步执行并回传返回值 (仅支持"纯数值参数"的消息;
            // 需要缓冲区指针的消息请用本文件封装好的 combo/gettext 命令)
            DWORD_PTR res = 0;
            LRESULT ok = SendMessageTimeoutW(h, msg, wp, lp,
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                             3000, &res);
            out = L"{\"ok\":";
            out += ok ? L"true" : L"false";
            out += L", ";
            JsonKN(out, L"result", (long long)res);
            out += L"}";
        }
        return true;
    }

    if (op == L"key" && tok.size() >= 3) {
        HWND h = AutoResolveSpec(tok[1], NULL);
        if (!h) { out = L"{\"ok\":false,\"error\":\"target not found\"}"; return true; }
        std::wstring text;
        for (size_t i = 2; i < tok.size(); ++i) {
            if (i > 2) text += L' ';
            text += tok[i];
        }
        for (size_t i = 0; i < text.size(); ++i)
            PostMessageW(h, WM_CHAR, (WPARAM)text[i], 0);
        out = L"{\"ok\":true";
        out += L", ";
        JsonKN(out, L"chars", (long long)text.size());
        out += L"}";
        return true;
    }

    if (op == L"invoke" && tok.size() >= 2) {
        int id = 0;
        if (hk.invokeTarget) id = hk.invokeTarget(tok[1].c_str());
        if (id == 0) id = _wtoi(tok[1].c_str());
        HWND m = hk.getMain ? hk.getMain() : NULL;
        if (id <= 0 || !m) { out = L"{\"ok\":false,\"error\":\"unknown target\"}"; return true; }
        // 与真实点击/菜单相同: 向主窗口投 WM_COMMAND, 处理函数在 UI 线程执行
        if (!PostMessageW(m, WM_COMMAND, MAKEWPARAM((WORD)id, 0), 0)) {
            out = L"{\"ok\":false,\"error\":\"PostMessage failed\"}";
            return true;
        }
        out = L"{\"ok\":true";
        out += L", ";
        JsonKN(out, L"posted_id", id);
        out += L"}";
        return true;
    }

    if (op == L"sleep" && tok.size() >= 2) {
        Sleep((DWORD)_wtoi(tok[1].c_str()));
        out = L"{\"ok\":true}";
        return true;
    }

    return false;
}

// ------------------------------------------------------- 服务实现

namespace autoimpl {

// static 而非 inline: MSVC 默认 C++14 不支持 inline 变量(C7525);服务端全部
// 函数自包含于本头文件, 各 TU 独立副本互不交互, static 语义等价且新旧编译器通吃
static wchar_t g_pipeName[160] = L"";   // \\.\pipe\<base>-<pid>

// 行切分: 空白切分; 支持 "双引号 token" —— 表达空串与内嵌空格
// (autocli 对空参数输出 "" 占位; 对无引号输入, 新旧版本解析结果完全一致)
inline void Tokenize(const std::wstring& s, std::vector<std::wstring>& t)
{
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) ++i;
        if (i >= s.size()) break;
        if (s[i] == L'"') {
            ++i;
            std::wstring v;
            while (i < s.size() && s[i] != L'"') {
                if (s[i] == L'\\' && i + 1 < s.size() && (s[i+1] == L'"' || s[i+1] == L'\\')) {
                    v += s[i+1];
                    i += 2;
                } else {
                    v += s[i];
                    ++i;
                }
            }
            if (i < s.size()) ++i;   // 跳过收尾引号
            t.push_back(v);          // 空引号 => 空 token (合法)
        } else {
            size_t j = i;
            while (j < s.size() && !iswspace(s[j])) ++j;
            if (j > i) t.push_back(s.substr(i, j - i));
            i = j;
        }
    }
}

static void HandleClient(HANDLE h)
{
    // 读一行 (UTF-8 字节流, BYTE 模式)
    std::string raw;
    char ch;
    DWORD got = 0;
    while (raw.size() < 16 * 1024 &&
           ReadFile(h, &ch, 1, &got, NULL) && got == 1) {
        if (ch == '\n') break;
        if (ch != '\r') raw += ch;
    }

    std::wstring line = AutoUtf8ToWide(raw);
    std::vector<std::wstring> tok;
    Tokenize(line, tok);

    std::wstring out;
    if (tok.empty()) {
        out = L"{\"ok\": false, \"error\": \"empty request\"}";
    } else {
        AutoDispatch(tok, line, out);
    }
    out += L'\n';

    std::string body = AutoWideToUtf8(out);
    DWORD written = 0;
    WriteFile(h, body.c_str(), (DWORD)body.size(), &written, NULL);
}

// 单客户端处理线程: 一条连接一个线程, 长 wait 不会阻塞其他命令 (多实例)
static DWORD WINAPI ClientThread(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    HandleClient(h);
    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
    return 0;
}

static DWORD WINAPI ServerThread(LPVOID)
{
    for (;;) {
        HANDLE h = CreateNamedPipeW(
            g_pipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            16 * 1024, 16 * 1024, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        BOOL conn = ConnectNamedPipe(h, NULL);
        if (!conn && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            continue;
        }
        CloseHandle(CreateThread(NULL, 0, ClientThread, (LPVOID)h, 0, NULL));
    }
}

static void WriteInfoJson(const wchar_t* outDir, const wchar_t* baseName,
                          const wchar_t* desktopName)
{
    std::wstring path = std::wstring(outDir) + L"\\automation_info.json";
    std::wstring j = L"{";
    JsonKV(j, L"pipe", baseName);
    j += L", ";
    JsonKN(j, L"pid", GetCurrentProcessId());
    j += L", ";
    JsonKV(j, L"desktop", desktopName);
    j += L" }";

    std::string b = AutoWideToUtf8(j);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}

} // namespace autoimpl

// 启动管道服务线程并写接口发现文件。basePrefix 如 L"ShadowAuto",
// 实际管道名为 "<basePrefix>-<pid>"。返回 false = 管道创建失败。
inline bool StartAutomation(const wchar_t* basePrefix, const wchar_t* outDir,
                            const wchar_t* desktopName)
{
    wchar_t base[128];
    _snwprintf(base, 128, L"%ls-%lu", basePrefix, GetCurrentProcessId());
    base[127] = 0;
    _snwprintf(autoimpl::g_pipeName, 160, L"\\\\.\\pipe\\%ls", base);
    autoimpl::g_pipeName[159] = 0;

    autoimpl::WriteInfoJson(outDir, base, desktopName);

    HANDLE t = CreateThread(NULL, 0, autoimpl::ServerThread, NULL, 0, NULL);
    return t != NULL;
}
