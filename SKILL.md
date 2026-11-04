---
name: shadowdesktop-gui-sandbox
description: |
  把 GUI 测试程序与 PowerShell/命令行执行隔离到独立 Windows 桌面对象上运行,
  不弹窗、不进步务栏、不抢用户键盘鼠标焦点, 从根本上解决"zcode 会话做
  GUI 测试/跑脚本干扰用户"的问题。配套 PrintWindow 合成截图(用户桌面不可见
  也能截)和命名管道自动化接口(tree/click/find/wait, 无图无鼠标的语义化驱动
  与断言)。当用户要求测试/运行任何会弹窗口的 GUI 程序、做 GUI 自动化验证、
  截图验证桌面程序界面、抱怨测试窗口干扰正常操作、或要开 PowerShell 执行
  会弹控制台窗口的脚本时使用(全局约定: 此类执行一律进沙箱桌面)。
  自研 GUI 还可通过 automation.h 以 3 行代码接入自动化接口;
  WebView2/Chromium(Edge) 程序用 cdpcli.mjs 走 CDP 驱动(含沙箱内 Edge、
  WebView2 边界判决见文内)。
---

# ShadowDesktop 沙箱桌面 GUI 测试

## 工具位置（默认安装路径, 静态链接, 拷走即用）

```bash
SD="$HOME/.zcode/skills/shadowdesktop-gui-sandbox"   # 实际以安装位置为准
$SD/bin/desktopctl.exe   # 生命周期: launch/shot/kill/run/enum/switch/ps/waitwin
$SD/bin/sample_gui.exe   # 参考测试样本(自截图+自动化接口)
$SD/bin/autocli.exe      # 自动化客户端: tree/get/set/invoke/click/find/ctrl/ctrls/key/wait/gettext/combo/check
$SD/bin/cdpcli.mjs       # CDP 驱动客户端(Node>=22): Edge 等完整 Chromium 程序的沙箱驱动
$SD/bin/runhidden.exe    # 零窗口执行器: 隐藏运行 powershell/任意命令, 透传退出码; -e 提权
```

源码在 `$SD/src/`（含 automation.h 可复用自动化模块）。完整文档在
`$SD/README.md`，GitHub 仓库: https://github.com/yanghaoi/shadowdesktop-gui-sandbox 。

## 标准流程 A：一站式（最常用, 测完自动清理）

```bash
SD="$HOME/.zcode/skills/shadowdesktop-gui-sandbox"   # 实际以安装位置为准
$SD/bin/desktopctl.exe run ZCode-<会话标识> "被测程序.exe" shot.png 3
# = 启动到沙箱桌面 → 等 3 秒 → 截图 → 全家族杀掉 → 桌面自动销毁
```

## 标准流程 B：交互式（需要多步驱动/断言时）

```bash
SD="$HOME/.zcode/skills/shadowdesktop-gui-sandbox"   # 实际以安装位置为准
# 1. 启动 (打印 PID= 与 EXITFILE=, 桌面不存在会自动创建)
$SD/bin/desktopctl.exe launch ZCode-<会话标识> "被测.exe" --automation --out "$TMP/shots"
PID=<上面打印的PID>
$SD/bin/desktopctl.exe waitwin ZCode-<会话标识> $PID 15000   # 等窗口就绪(代替固定 sleep)
# 2. 语义驱动 (被测程序支持 --automation 时, 首选, 无图无鼠标):
PIPE=$(python -c "import json;print(json.load(open('$TMP/shots/automation_info.json'))['pipe'])")
$SD/bin/autocli.exe $PIPE tree                     # 全部窗口+层级控件树(含弹窗)
$SD/bin/autocli.exe $PIPE click 3001               # 按控件ID点击(=真实点击同路径)
$SD/bin/autocli.exe $PIPE find text 确定           # 全窗口子串查找
$SD/bin/autocli.exe $PIPE wait status contains ok 10000   # 状态断言
$SD/bin/autocli.exe $PIPE gettext 1002             # 读回任意控件文本(set 后断言)
$SD/bin/autocli.exe $PIPE combo 1004 list          # 下拉框: 枚举项/读选中/按索引或文本选中
$SD/bin/autocli.exe $PIPE combo 1004 select 模板名  #   (选中后向父窗口投 CBN_SELCHANGE, 联动照常)
$SD/bin/autocli.exe $PIPE check 1003 set 1         # 复选框置位(BM_SETCHECK+BN_CLICKED)/读回
$SD/bin/autocli.exe $PIPE set text 1002 ""         # 空参数要写成 "" (引号 token)
# 3. 视觉验证 (任意时刻, 窗口在沙箱里也截得到)
$SD/bin/desktopctl.exe shot ZCode-<会话标识> step1.png
$SD/bin/desktopctl.exe ps ZCode-<会话标识>          # 沙箱里还有谁(进程+窗口标题)
# 4. 收尾 (必须): 杀掉桌面全部进程(含无窗口子进程), 桌面自动销毁
$SD/bin/desktopctl.exe kill ZCode-<会话标识>
# 崩溃/退出检测: EXITFILE 在子进程退出后写入退出码(0=正常, 0xC...=异常;
# 注意 MFC 对话框程序经 WM_CLOSE/取消 关闭通常返回 2=IDCANCEL, 属正常)
```

## 隐藏窗口执行（runhidden, 2026-10-31 起的标准姿势）

**zcode 会话从 bash 跑 powershell 会分配可见控制台窗口（闪现/常驻干扰用户）**。
标准解法：一律经 `runhidden.exe` 执行 —— 它自身是 GUI 子系统（无控制台），子进程
以 CREATE_NO_WINDOW/SW_HIDE 创建，**全程零窗口零闪现**，并**透传退出码**。

```bash
RH="$HOME/.zcode/skills/shadowdesktop-gui-sandbox/bin/runhidden.exe"

# 普通隐藏执行（逻辑写进 .ps1, 输出 Out-File 落盘）:
$RH powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\path\job.ps1"
echo $?    # 透传 job 的退出码

# 提权隐藏执行（替代闪窗的 Start-Process -Verb RunAs -Wait 模式）:
$RH -e powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\path\job.ps1"
# 退出码: 0=成功; 740=需提权(改用 -e); 1602=用户取消 UAC; 3=路径未找到
```

要点：
- **输出不回传**（无控制台可看）——job.ps1 内一律 `Out-File`/重定向落盘, 结果 cat 文件。
- 内层 powershell 仍建议带 `-WindowStyle Hidden`（双保险）。
- 提权模式下 UAC 确认框无法隐藏（安全桌面机制）; 本机 ConsentPromptBehaviorAdmin=0 自动批准无感。
- 实测（2026-10-31）: 普通与提权模式均零可见窗口（job 自查 MainWindowHandle≠0 计数=0）,
  退出码 42 透传 ✓, 提权 elevated=True ✓。

## 关键事实（实测结论, 不要走弯路）

- **PowerShell / 任何会弹窗的程序也要进沙箱**（2026-10-31 起, zcode 全局约定）:
  会打开控制台/GUI 窗口、抢焦点的执行一律走 desktopctl, 避免闪窗干扰用户桌面。
  标准模式（run 不支持带参, 带参用 launch; 输出靠进程自己写文件, 沙箱里 stdout 不回传）:
  ```bash
  # 1) 把要执行的逻辑写进 .ps1（输出用 Out-File 落盘）
  # 2) launch 到沙箱（控制台窗口开在沙箱里, 用户桌面完全不可见）:
  $SD/bin/desktopctl.exe launch ZCode-<id> "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" \
      -NoProfile -ExecutionPolicy Bypass -File "C:\path\to\job.ps1"
  # 3) 需要画面就 shot（注意: 脚本退出后桌面自动销毁, shot 要赶在退出前, 或 -File 里加 Start-Sleep）
  # 4) 确认结果: cat 输出文件; 收尾 kill（桌面已自动销毁时报 err=2 属正常, 不用管）
  # 无参数的快速执行可以用 run: $SD/bin/desktopctl.exe run ZCode-<id> notepad.exe out.png 3
  ```
  纯后台命令（文件/编译/git 等不弹窗的）照常直接跑, 不必进沙箱。
  提权链的零窗口执行用 `runhidden.exe -e`（见上节）, 替代闪窗的
  `powershell -Command "Start-Process powershell -Verb RunAs ..."` 老模式。
- 沙箱 GUI **收不到** SendInput 键鼠（这是隔离的目的）；驱动它用 autocli 的
  消息注入, 或让自研程序接 automation.h。
- **PostMessage 跨桌面投递的前置条件（2026-10-31 实测）**: 驱动进程必须
  **持有沙箱桌面的打开句柄**（OpenDesktop 后保持不关）——枚举完就 CloseDesktop
  的话, 之后对沙箱内窗口的 PostMessage 一律返回 FALSE (句柄有效也拒投)。
  EnumDesktopWindows 枚举本身不受影响 (只需 READOBJECTS)。另注意: 每轮
  Start-Process -Verb RunAs 是独立的提升上下文, 跨轮复用旧 hwnd 会撞上
  已销毁窗口 (err=1400) —— **启动/驱动/截图/清理写在同一个提升脚本里闭环**。
- 提权被测程序 (requireAdministrator) 进沙箱: 非提权 desktopctl CreateProcess
  报 err=740, **整个测试会话提权运行** (提权 desktopctl 启动提权子进程, 无 UAC 弹窗,
  桌面 ACL 同上下文一致)。
- **多会话共用 bin 目录**: 别的会话可能有 keeper 常驻占用 `$SD/bin/desktopctl.exe`
  (Device or resource busy) —— **永远不要 `taskkill //im desktopctl.exe`**(会误杀
  别人会话的 keeper)。替换 exe 用 rename+copy: `mv desktopctl.exe{,.old} && cp 新
  desktopctl.exe .`。
- 协议/命令行里**空参数要写成 `""`**（引号 token）, 否则被空白切分丢弃;
  autocli 对空 argv 自动输出 `""`, 两侧版本对无引号输入解析完全一致。
- 截图链路 BitBlt→PrintWindow 合成→--flash 自动降级, 实测对 GDI/GPU(Edge) 窗口
  均可靠; `--flash` 会让用户屏幕闪现沙箱画面 0.4s, 仅作保底。
- 浏览器(Edge)在全新沙箱桌面首启要等 ~15 秒窗口才出现, 其他程序秒起。
- 桌面名带会话标识（如 `ZCode-<name>`）, 多会话并行互不干扰。
- 测试程序退出 + keeper 释放句柄后, 桌面对象**自动销毁**——之后再 ps/shot 会报
  err=2 "打不开桌面", 属正常, 不是故障。
- UIA/WinAppDriver/pyautogui **跨不了桌面**, 不要往这个方向试。
- **WebView2 宿主程序（如 musicdl）进沙箱 = 浏览器进程僵死**（黑屏、回调不触发）,
  上游 #960 不修——详见下节「WebView2 应用测试」。同版本**完整 Edge 进沙箱完全正常**。
- RDP 断连场景 PW/BitBlt/flash 全失效, 需注入式截图（shotagent/ 未随本包分发, 需要时按 README 参考资料自建）。
- 本机验证记录: 2026-10-31, win10 19045, notepad 沙箱截图(1920x1080 PrintWindow 合成)与
  PowerShell 沙箱执行(输出落盘)均通过; exe 静态链接, 直接可用。

## WebView2 应用测试（2026-11-05 判决, runtime 155; 详见 README「WebView2 × 沙箱/CDP」节）

三个硬边界（`src/wv2sandbox_test.cpp` 可复验, runtime 升级后重跑一遍）:
1. **沙箱**: WebView2 宿主进 CreateDesktop 桌面 → 浏览器僵死(#960), 注入改 lpDesktop
   也救不了（跨桌面 controller 创建 hr=0x80040205）;
2. **CDP**: runtime 150+ 回归(#5645), `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=…`
   **不生效**（env var 机制正常, 同批 --enable-logging 生效）; 149 正常;
3. **不可见形态全灭**: SW_HIDE/屏外/最小化/1x1/cloak/controller 不可见 → 浏览器进程
   握手后 ~100ms 自退（keep-alive 日志铁证）→ 宿主窗口必须正常显示。

可用配方:
- **musicdl 已接入（2026-11-05 验收通过）**: `<副本>/webview2-demo.exe --automation
  --out <dir>` 启动（窗口正常显示, 用独立路径副本避开单实例）→ autocli
  `wait wvnav eq 1` / `js "<expr>"`（DOM 断言/点击/输入/`send({type:...})` app 级动作）
  / `shot <file>`。其它自研 WebView2 程序照 automation.h 三步接入（README 有判决约束）。
- **沙箱里只驱动 Win32 外壳**（tray/对话框, autocli 消息注入）。
- **完整 Chromium 程序（Edge 等）**: 沙箱 + CDP 全通——
  ```bash
  export WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=19437 --remote-allow-origins=*"
  $SD/bin/desktopctl.exe launch ZCode-<id> "…msedge.exe" --user-data-dir=<临时目录> --no-first-run <url>
  node $SD/bin/cdpcli.mjs --wait 30 19437 eval "document.title"
  node $SD/bin/cdpcli.mjs 19437 click "#btn"      # rawclick=真实输入管线, 不用 SendInput
  node $SD/bin/cdpcli.mjs 19437 shot out.png      # CDP 截图(隐藏窗也能截)
  ```

## 自研程序接入自动化接口（AutoCommonDispatch 范式）

程序侧三步（详见 `src/automation.h` 头注释与 `src/sample_gui.cpp` 范本）:
1. 命令行解析 `--automation --out <目录>`（正常双击启动不带参、不开管道）;
2. 窗口/控件就绪后调一次 `StartAutomation(L"ShadowAuto", outDir, desktopName)`;
3. 实现 `AutoDispatch()`: **只写业务命令**（业务 get/set/invoke 名）, 末尾一行
   `AutoCommonDispatch(tok, out, hk)` 兜底通用 op（tree/click/find/ctrl(s)/key/
   set text/gettext/combo/check/invoke/wait/sleep + 内置 get title|pid|desktop）,
   业务解析经 `AutoCommonHooks`（getMain/getValue/invokeTarget/treeAppInfo）注入。
   MFC 常见控件不必再手写: 下拉枚举/选中(CB_*)、复选框置位回读(BM_*)、
   控件文本读回(WM_GETTEXT) 都是现成 op。

**MSVC/MFC 接入注意（2026-10-06 DHLoader 实测）**:
- `automation.h` 必须保持 **UTF-8 WITH BOM** —— 无 BOM 时 MSVC 按代码页 936
  误读中文注释, 会报一连串与代码无关的语法错误;
- `g_pipeName` 已用 static（非 inline 变量）, MSVC 默认 C++14 可直接编译;
- 接入无需改工程的 C++ 语言标准。

## 源码修改与重编（需要时）

```bash
export PATH="<mingw-w64 的 bin 目录>:$PATH"   # 必须前置: 多套 GCC 运行时并存时, 旧 libmpfr/libmpc 被抢先加载会导致 g++ 静默崩溃
cd <本仓库/技能源码目录>
F="-std=c++17 -O2 -municode -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"
g++ $F -mconsole desktopctl.cpp -o desktopctl.exe -lgdi32 -luser32 -lgdiplus -static
g++ $F -mwindows sample_gui.cpp -o sample_gui.exe -lgdi32 -luser32 -lshell32 -lgdiplus -static
g++ $F -mconsole autocli.cpp     -o autocli.exe     -luser32 -ladvapi32 -static
# 改动后同步回技能目录 (exe 被其他会话 keeper 占用时用 rename+copy):
#   cp desktopctl.cpp autocli.cpp automation.h sample_gui.cpp $SD/src/
#   mv $SD/bin/xxx.exe{,.old} && cp xxx.exe $SD/bin/ && rm $SD/bin/xxx.exe.old
```
