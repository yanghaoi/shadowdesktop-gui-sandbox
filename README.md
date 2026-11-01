# ShadowDesktop — Windows 沙箱桌面 GUI 自动化测试工具

> Run GUI programs on an isolated Windows desktop object — invisible to the user,
> never steals focus, with screenshot capture and a named-pipe automation API.

GitHub: **https://github.com/yanghaoi/shadowdesktop-gui-sandbox**

解决的核心问题：**自动化会话（AI agent / 无人值守测试）操作 GUI 程序时，被测程序不断
在用户当前桌面弹出窗口、抢焦点、进任务栏，严重干扰正常操作。**

原理：Windows 的**桌面对象**（`CreateDesktop`，注意不是"虚拟桌面"）是同一会话内互相
隔离的窗口容器。把被测 GUI 程序启动到一个独立桌面上，它的窗口在用户桌面完全不可见、
不进用户任务栏、抢不到用户键盘鼠标焦点；测试会话通过本工具对沙箱桌面**截图**做视觉
验证，或通过**命名管道自动化接口**做无图无鼠标的语义化驱动与断言。

```
用户桌面 (Default)                          沙箱桌面 (如 ZCodeTest)
┌─────────────────────┐                    ┌─────────────────────┐
│  用户正常工作        │   desktopctl       │  被测 GUI 程序       │
│  自动化会话(终端) ───┼───launch/shot/kill─┼→  (完全不可见,       │
│                     │   ←───PNG 截图─────│    不抢任何输入)     │
└─────────────────────┘                    └─────────────────────┘
```

## 特性一览

- **真隔离**：独立 Windows 桌面对象，不弹窗、不进步务栏、收不到用户键鼠（SendInput 跨不进来）
- **看不见也能截**：PrintWindow/WM_PRINT 逐窗口合成截图，经典 GDI（notepad）与 GPU/DWM
  渲染（Edge/Chromium）窗口均可靠；`--flash` 瞬时切换保底
- **无图无鼠标的语义化驱动**：被测程序内嵌命名管道服务（CDP 风格），`autocli` 提供
  tree/click/find/get/set/wait 等命令，断言走返回值，截图只作最终视觉验收
- **自研 GUI 低成本接入**：include `automation.h` + 3 行代码即可获得全套自动化接口，
  兼容 MinGW 与 MSVC/MFC 工程
- **零窗口执行器** `runhidden.exe`：隐藏运行 PowerShell/任意命令行（含提权），退出码透传
- **零依赖、拷走即用**：全部 exe 静态链接（MinGW-w64 编译），PNG 编码用系统 GDI+

## 仓库结构

| 路径 | 说明 |
|---|---|
| `bin/desktopctl.exe` | 控制工具：建/枚举桌面、把进程启动到沙箱桌面、外部截图、清理 |
| `bin/runhidden.exe` | 零窗口执行器：CREATE_NO_WINDOW/SW_HIDE 隐藏运行并等待，透传退出码；`-e` 提权（runas） |
| `bin/sample_gui.exe` | 被测 GUI 样本：窗口+时钟、自截图、**内嵌自动化管道服务** |
| `bin/autocli.exe` | 自动化接口客户端：tree/get/set/invoke/wait/shot，纯命令行语义驱动 |
| `src/desktopctl.cpp` | desktopctl 源码 |
| `src/autocli.cpp` | autocli 源码 |
| `src/sample_gui.cpp` | sample_gui 源码（自动化接入范本） |
| `src/runhidden.cpp` | runhidden 源码 |
| `src/automation.h` | 内嵌自动化服务模块（命名管道 + 行协议 + JSON 响应），include 即用 |
| `src/gdi_capture.h` | 共享截图模块（BitBlt 整屏 + PrintWindow/WM_PRINT 逐窗口合成 → PNG） |
| `src/cloak_test.cpp` | 截图路径定向实验程序（2026-10-06 验证 PrintWindow 对 cloak/GPU 窗口可靠性） |
| `SKILL.md` | AI agent 技能（skill）版使用说明，供装入 agent 的技能目录 |

## 快速开始

预编译 exe 在 `bin/`（静态链接，无运行时依赖），两种取用方式：

**方式一：下载完整包（给 agent 装技能用，推荐）**

从 [Releases](https://github.com/yanghaoi/shadowdesktop-gui-sandbox/releases/latest) 下载
`shadowdesktop-gui-sandbox-v1.0.0.zip`，解压出的 `shadowdesktop-gui-sandbox/` 目录
直接就是一个可用的技能目录（含 `SKILL.md` + `bin/` + `src/`），丢进 agent 的技能
目录即可，无需 git 与编译环境。

**方式二：clone 仓库**

```bash
git clone https://github.com/yanghaoi/shadowdesktop-gui-sandbox.git
cd shadowdesktop-gui-sandbox

# 一站式: 启动被测程序到沙箱桌面, 等 3 秒, 截图, 自动清理 (最常用)
./bin/desktopctl.exe run ZCode-Test "C:\path\to\target.exe" out.png 3

# 或交互式: 启动 → 任意时刻截图 → 清理
./bin/desktopctl.exe launch ZCode-Test target.exe   # 打印 PID=xxx
./bin/desktopctl.exe shot   ZCode-Test step1.png
./bin/desktopctl.exe kill   ZCode-Test              # 测试完必须清理
```

桌面名建议带会话标识（如 `ZCode-<会话名>`），多个会话并行测试互不干扰。

## desktopctl 用法

```
desktopctl launch <name> <exe> [args...]        # 把程序启动到沙箱桌面, 打印 PID=xxx
desktopctl shot   <name> <out.png> [--refresh] [--flash]
                                                # 截取沙箱桌面(详见下方截图策略)
desktopctl kill   <name>                        # 终止沙箱桌上所有进程(桌面随之销毁)
desktopctl run     <name> <exe> <out.png> [延迟秒] [--keep]
                                                # 一站式: launch → 等待 → shot → kill
desktopctl enum                                 # 列出当前窗口站所有桌面
desktopctl switch <name>                        # 物理切到该桌面(调试用; 切回: switch Default)
desktopctl create <name>                        # 预建桌面(孤立桌面很快自动销毁, 一般用 launch)
desktopctl ps     <name>                        # 列出桌面上的进程(PID+窗口标题), kill 前预览
desktopctl waitwin <name> <pid> [超时ms=15000]   # 等待 pid 出现可见窗口(代替固定 sleep)
                                                # 退出码 0=已出现 3=超时; 测试程序退出+keeper
                                                # 释放句柄后桌面自动销毁, 之后 ps/shot 报 err=2 属正常
```

要点：

- 目标程序在沙箱桌面收不到鼠标键盘（这正是目的）；驱动它请用 `PostMessage/SendMessage`
  （按钮 `WM_COMMAND`、`BM_CLICK` 等）或下一节的自动化接口。
- 测试结束务必 `kill`；`run` 模式会自动做。keeper 进程（无窗口）会在子进程退出后自动
  释放桌面，不留残留。
- `--out` / PNG 路径建议放临时目录，测试后清理截图产物。

### 截图策略（自动降级）

1. **BitBlt 整屏**：仅当目标桌面恰为输入桌面时可行。Windows 对**非输入桌面**的
   屏幕 DC 直接拒绝 BitBlt（实测 err=5/6），属系统安全行为。
2. **PrintWindow/WM_PRINT 逐窗口合成**（实际主力路径）：枚举沙箱桌面全部可见顶层
   窗口，逐个 `PrintWindow(PW_RENDERFULLCONTENT)`，按 z 序贴回虚拟屏坐标画布。
   实测（2026-10-06 cloak_test 定向实验）：**PW 对经典 GDI（notepad）和 GPU/DWM
   渲染（Edge/Chromium）窗口都可靠**，启动后 1 秒内的新鲜窗口也能截到；窗口被
   DWM cloak 不影响 PW 取纹理；DwmSetWindowAttribute 去 cloak 无必要。`WM_PRINT`
   在此环境多数为空白（仅作最后兜底保留）。诊断输出会打印每个窗口的
   类名/矩形/printwindow 结果/是否空白。
3. **`--flash` 保底**：瞬时 `SwitchDesktop` 到沙箱桌面 → BitBlt → 400ms 后切回。
   屏幕会闪现沙箱画面一下，但 100% 可靠。

## PowerShell / 命令行程序的沙箱执行

凡是会弹控制台/GUI 窗口、抢焦点的命令行执行（含交互式 PowerShell），也可以放进
沙箱桌面，避免闪窗干扰用户桌面。要点：`run` 不支持带参，带参用 `launch`；沙箱里
stdout 不回传，**输出靠进程自己写文件**（`Out-File` / 重定向）。

```bash
# 1) 逻辑写进 .ps1（输出 Out-File 落盘; 末尾可加 Start-Sleep 便于 shot）
#    psjob.ps1:
#    Get-Date | Out-File "C:\...\ps_out.txt"
# 2) launch 进沙箱（powershell 的控制台窗口开在沙箱里, 用户桌面不可见）:
./bin/desktopctl.exe launch ZCode-PsTest \
    "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" \
    -NoProfile -ExecutionPolicy Bypass -File "C:\...\psjob.ps1"
# 3) 结果确认: cat 输出文件; shot 需赶在脚本退出前（退出后桌面自动销毁, shot/kill
#    报 err=2 属正常）; 收尾 kill
./bin/desktopctl.exe kill ZCode-PsTest
```

纯后台命令（文件/编译/git 等不弹窗的）照常直接执行，不必进沙箱。

### 隐藏窗口执行（runhidden.exe）

从脚本/终端直接跑 `powershell` 会分配可见控制台窗口。标准解法：

```bash
RH=./bin/runhidden.exe
$RH powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\path\job.ps1"
echo $?    # 透传 job 退出码
$RH -e powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\path\elev.ps1"   # 提权
```

- 自身 GUI 子系统无控制台，子进程 CREATE_NO_WINDOW/SW_HIDE —— 零窗口零闪现；
- 退出码透传（0=成功，740=需提权改用 `-e`，1602=UAC 取消，3=路径未找到）；
- 输出不回传，job 内 `Out-File` 落盘；
- 提权模式 UAC 确认框无法隐藏（安全桌面机制，系统级限制）。

### sample_gui 用法（测试样本）

```
sample_gui.exe [--automation] [--autoshot <秒>] [--out <目录>]
```

- 窗口显示所在桌面名 / PID / 分辨率 / 每秒时钟（持续重绘保证截图内容新鲜）
- 按钮「截取本桌面 -> PNG」手动自截图；`--autoshot` 定时自截图
- 每次成功自截图后写 `<out>\last_selfshot.txt`（内容为 PNG 路径），供脚本在沙箱
  外断言「自截图已发生」

## 内嵌自动化接口（首选测试方式，无图无鼠标）

被测 GUI 以 `--automation` 启动后，进程内嵌一个命名管道服务（CDP 风格），
测试会话通过 `autocli` 语义化驱动程序、断言状态——**不截图、不模拟输入**。
命名管道不受桌面隔离影响，与沙箱桌面方案完全正交。

```
被测程序以 --automation 启动 → 输出目录生成 automation_info.json:
{"pipe": "ShadowAuto-<pid>", "pid": N, "desktop": "ZCodeTest"}
```

autocli 命令（退出码 0=成功, 3=失败/未匹配, 4=连接错误）：

```bash
PIPE=$(python -c "import json;print(json.load(open('shots/automation_info.json'))['pipe'])")

./bin/autocli.exe $PIPE tree                        # 全部顶层窗口+层级控件树 (含弹窗/MDI/隐藏窗口)
./bin/autocli.exe $PIPE get <name>                  # 读状态: status/clock/title/selfshots/pid/...
./bin/autocli.exe $PIPE set title <文本>             # 改主窗口标题
./bin/autocli.exe $PIPE set interval <秒>           # 改自截图间隔
./bin/autocli.exe $PIPE set text <id|hwnd> <文本>    # 任意控件 WM_SETTEXT(Edit 自动补发 EN_CHANGE)
./bin/autocli.exe $PIPE gettext <id|hwnd>           # 读回任意控件文本 (set text 的反操作)
./bin/autocli.exe $PIPE combo <id> list             # 下拉框: 枚举全部项+当前选中索引
./bin/autocli.exe $PIPE combo <id> sel              # 下拉框: 读当前选中文本
./bin/autocli.exe $PIPE combo <id> select <索引|文本>  # 下拉选中(索引或精确文本),
                                                    #   选中后向父窗口投 CBN_SELCHANGE, 联动照常
./bin/autocli.exe $PIPE check <id> get              # 复选框读回 (BM_GETCHECK)
./bin/autocli.exe $PIPE check <id> set <0|1|2>      # 复选框置位 BM_SETCHECK + BN_CLICKED 通知
./bin/autocli.exe $PIPE invoke <shot|quit|popup|id> # 投 WM_COMMAND, 与真实点击同路径
./bin/autocli.exe $PIPE click <id|hwnd>             # 按钮 BM_CLICK / 非按钮投父窗口 WM_COMMAND
./bin/autocli.exe $PIPE find <class|text|id> <值>    # 全窗口子串查找, 返回 hwnd/id/类名/文本
./bin/autocli.exe $PIPE ctrl  <id|hwnd> <msg> <wp> <lp>   # 通用消息注入 (PostMessage)
./bin/autocli.exe $PIPE ctrls <id|hwnd> <msg> <wp> <lp>   # 同步消息+回传结果 (如 CB_GETCOUNT)
./bin/autocli.exe $PIPE key  <id|hwnd> <字符...>     # 逐字符 WM_CHAR, 不依赖焦点/IME
./bin/autocli.exe $PIPE wait <name> <op> <值> <ms>   # 等条件 (contains/eq/ne/ge/le)
./bin/autocli.exe $PIPE shot out.png                # 程序自截图
./bin/autocli.exe $PIPE invoke quit                 # 优雅退出
```

`<id|hwnd>` 定位: 纯数字=控件 ID (在全窗口后代中搜索); `h<十进制>`=直接 hwnd
(tree/find 的输出里有 hwnd)。消息号支持 0x 十六进制。`ctrls` 只支持纯数值参数
的消息 (需要缓冲区指针的消息已被 combo/gettext 等封装命令覆盖)。

**空参数/含空格参数**: 命令行里空值要写成 `""`（如 `set text 1002 ""` 清空输入框）,
服务端按引号 token 切分; 无引号输入的新旧解析结果完全一致 (2026-10-06 起)。

**弹窗/模态对话框流程** (实测): `invoke popup` → `find class "#32770"` 拿到
对话框 hwnd → 对其按钮 `click <id>` (MessageBox 的 OK=1/Cancel=2) → `find`
确认消失。模态期间 UI 线程照常处理管道命令, tree/get 全程可用。

**崩溃/退出检测**: launch 会打印 `EXITFILE=<cwd>\child_exit_<pid>.txt`,
keeper 在子进程退出后写入退出码 (0=正常, 0xCXXXXXXX=异常), 供脚本断言。

### 自研 GUI 的接入方法

include `automation.h`，实现 `AutoDispatch()`（**只写业务命令**，末尾一行
`AutoCommonDispatch(tok, out, hk)` 兜底通用 op，业务解析经
`AutoCommonHooks{getMain/getValue/invokeTarget/treeAppInfo}` 注入，范本见
`src/sample_gui.cpp` 的 AutoDispatch），窗口建好后调 `StartAutomation()`。
通用层自带: tree/click/find/ctrl(s)/key/set text/gettext/combo/check/invoke/
wait/sleep + 内置 get title|pid|desktop —— MFC 程序常用控件不必再手写。

程序侧三步（详见 `src/automation.h` 头注释与 `src/sample_gui.cpp` 范本）:

1. 命令行解析 `--automation --out <目录>`（正常双击启动不带参、不开管道）;
2. 窗口/控件就绪后调一次 `StartAutomation(L"ShadowAuto", outDir, desktopName)`;
3. 实现 `AutoDispatch()`: 只写业务命令, 末尾交 `AutoCommonDispatch` 兜底。

**MSVC/MFC 接入实测（2026-10-06, DHLoader）**：

- `automation.h` 必须保存为 **UTF-8 WITH BOM**——无 BOM 时 MSVC 按代码页 936
  误读中文注释, 报一连串与代码无关的语法错误（C2530/C2143/C2226）;
- `g_pipeName` 用 static 而非 inline 变量, MSVC 默认 C++14 直接可编,
  接入无需调工程语言标准; 显式 `#include <stdlib.h>/<wctype.h>` 已内置。

设计约定：

- 命令在管道线程执行；**动作**用 `PostMessage(WM_COMMAND)` 投给 UI 线程，
  与真实点击走同一代码路径；**查询**一律走 `AutoSafeGetText`/`SendMessageTimeout`
  （带 2~3s 超时，UI 线程挂死时不会吊死测试会话；勿用无超时的 GetWindowText）。
- 通用定位/查找/枚举辅助（`AutoResolveSpec`/`AutoFindWindows`/`AutoTopWindows`）
  都在 automation.h 里，新 GUI 直接复用；只需把业务量暴露成 get/wait 名字。
- 管道服务为**多实例**（每连接一个线程），一条长 wait 不阻塞其他命令。
- 截图降级为最终视觉验收；平时断言全部走 `get`/`wait` 的返回值。

## 构建源码（mingw-w64 g++，静态链接，无运行时依赖）

需要 MinGW-w64（如 [niXman/mingw-builds](https://github.com/niXman/mingw-builds-binaries)
或 MSYS2 的 mingw-w64 工具链）。**PATH 必须把要用的 mingw64/bin 放最前**——若机器上
存在多套 GCC 运行时（如 winlibs 与 MSYS2 并存），旧版 libmpfr/libmpc/libisl 被抢先
加载会导致编译器子进程静默崩溃（g++ exit 1 无输出）。

```bash
CXXFLAGS="-std=c++17 -O2 -municode -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"

g++ $CXXFLAGS -mconsole src/desktopctl.cpp -o bin/desktopctl.exe -lgdi32 -luser32 -lgdiplus -static
g++ $CXXFLAGS -mwindows src/sample_gui.cpp -o bin/sample_gui.exe -lgdi32 -luser32 -lshell32 -lgdiplus -static
g++ $CXXFLAGS -mconsole src/autocli.cpp     -o bin/autocli.exe     -luser32 -ladvapi32 -static
g++ -O2 -municode -mwindows src/runhidden.cpp -o bin/runhidden.exe -lshell32 -luser32 -static
```

## 实现要点（踩坑记录）

1. **桌面生命周期竞态**：桌面对象在"所有句柄关闭且无进程关联"时立即销毁。
   `launch` 进程如果启动完子进程就退出，桌面对象会赶在子进程完成挂接前被销毁，
   连带杀死子进程——表现为子进程 `0xC0000142`（DLL 初始化失败）。解法：launch 以
   `CREATE_NO_WINDOW` 拉起自身作为 **keeper** 驻留进程，桌面句柄经**句柄继承**传递，
   keeper 等子进程退出后才释放。
2. **非输入桌面截图**：`SetThreadDesktop`（必须在无窗口的专用线程上）后 BitBlt 依然
   被拒；`PrintWindow(PW_RENDERFULLCONTENT)` 对被 cloak 的窗口返回空白；最终可靠
   路径是 `WM_PRINT`。三者按序降级实现。**（2026-10-31 本机实测更新：PrintWindow 合成
   路径直接可靠，notepad 截图 1920x1080 一次成功，无需 WM_PRINT 兜底。）**
3. **跨桌面 PostMessage 前置条件**：驱动进程必须**持有沙箱桌面的打开句柄**
   （`OpenDesktop` 后保持不关）——枚举完就 `CloseDesktop` 的话，之后对沙箱内窗口的
   PostMessage 一律返回 FALSE（句柄有效也拒投）。`EnumDesktopWindows` 枚举本身不受
   影响（只需 READOBJECTS）。
4. **提权被测程序**（requireAdministrator）进沙箱：非提权 desktopctl CreateProcess
   报 err=740，需**整个测试会话提权运行**（提权 desktopctl 启动提权子进程，无 UAC
   弹窗，桌面 ACL 同上下文一致）。
5. **DPI**：两个 exe 均声明 Per-Monitor V2 感知，截图为物理分辨率。
6. **PNG 编码**：GDI+（系统组件），无第三方依赖；exe 静态链接，拷走即用。

## 已知限制

- 沙箱 GUI **收不到** SendInput 键鼠（这是隔离的目的）；驱动用 autocli 消息注入或
  automation.h 接口。
- UIA / WinAppDriver / pyautogui **跨不了桌面**，不要往这个方向试。
- RDP 断连场景下显示管线失效，PW/BitBlt/flash 全部不可用——需注入式截图后端
  （DLL 注入目标进程做进程内 BitBlt；**未随本仓库分发**，需要时可按参考资料自建）。
- UAC 提升确认框出现在安全桌面，无法被沙箱隔离（`runhidden -e` 只是隐藏提权链上
  的控制台窗口）。
- 浏览器（Edge/Chromium）在全新沙箱桌面首启要等 ~15 秒窗口才出现，其他程序秒起。
- 同一 bin 目录被多个并行会话共用时，替换 exe 需 rename+copy（`mv xxx.exe{,.old} &&
  cp 新 xxx.exe .`），不要对运行中的 exe 强删。

## 验证记录

- 2026-10-06（win10 19045）：cloak_test 定向实验，确认 PrintWindow 对 GDI/GPU/cloak
  窗口均可靠；MSVC/MFC 工程（DHLoader）接入 automation.h 成功。
- 2026-10-31（win10 19045）：notepad 沙箱截图（1920x1080 PrintWindow 合成）✓、
  PowerShell 沙箱执行（launch + 输出落盘）✓、脚本退出后桌面自动销毁 ✓、
  runhidden 普通与提权模式零可见窗口 ✓、退出码透传 ✓、跨桌面 PostMessage
  持句柄前置条件实测 ✓。

## 参考资料

- [CreateProcess in other desktop with lpDesktop (SO)](https://stackoverflow.com/questions/6974823/createprocess-in-other-desktop-with-lpdesktop)
- [Start process on a specific desktop (SO)](https://stackoverflow.com/questions/35942573/start-process-on-a-specific-desktop)
- [Desktop Security and Access Rights (MS Learn)](https://learn.microsoft.com/en-us/windows/win32/winstation/desktop-security-and-access-rights)
- [EnumDesktopWindows (MS Learn)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdesktopwindows)
- [SetThreadDesktop 截 UAC 桌面实战 (LookingGlass #263)](https://github.com/gnif/LookingGlass/issues/263)
- [Capturing an Image (MS Learn, BitBlt 截屏经典流程)](https://learn.microsoft.com/en-us/windows/win32/gdi/capturing-an-image)

## License

[MIT](LICENSE)
