#!/usr/bin/env node
// cdpcli.mjs —— WebView2/Chromium CDP 驱动客户端（零依赖, Node >= 22, 用全局 fetch/WebSocket）
//
// 原理: 给被测 WebView2 程序注入 --remote-debugging-port 后, Chromium 侧一切驱动
// 都走 CDP（DevTools 协议）: DOM 操作/断言走 Runtime.evaluate, 点击走
// Input.dispatchMouseEvent（浏览器进程真实输入管线, 不用 SendInput、不抢焦点）。
// loopback TCP 不受桌面对象隔离影响 —— 沙箱桌面里、隐藏窗里都一样能连。
//
// 被测程序注入配方（不改被测程序一行代码, 前提: 其创建 WebView2 时 options 传
// nullptr 或未显式给 AdditionalBrowserArguments; musicdl 实测为此路径）:
//   export WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=<port> --remote-allow-origins=*"
//   隐藏窗启动（用户桌面, 零闪现零抢焦点, 推荐）:
//     bin/runhidden.exe <被测.exe>            # runhidden 已带 STARTF_USESHOWWINDOW+SW_HIDE
//   沙箱启动: 见 README「WebView2 × 沙箱」——完整 Edge 可以进沙箱,
//   WebView2 宿主程序进沙箱会僵死(#960), 隐藏窗路线替代。
//
// 用法: node cdpcli.mjs [--wait <秒>] <port> <cmd> [args...]
//   list                    列出调试目标
//   eval "<js>"             页面内执行 JS（awaitPromise+returnByValue）, 打印结果 JSON
//   click "<selector>"      JS 级点击（el.click(), 走 DOM 事件处理器）
//   input "<selector>" "<text>"  写输入框值并派发 input/change 事件
//   rawclick "<selector>"   CDP Input.dispatchMouseEvent 在元素中心点击（真实输入管线）
//   send "<json>"           调页面 send(o) 桥 → app WebMessageReceived（app 级动作, 如 musicdl）
//   wait "<js>" <超时ms>    每 300ms 求值一次, 结果为真即通过（断言/等待用）
//   shot <file.png>         Page.captureScreenshot（页面内容截图, 不经 GDI, 隐藏窗也能截）
// 退出码: 0 成功, 3 失败/未命中/超时, 4 连接错误
// 示例:
//   node cdpcli.mjs --wait 30 19437 eval "document.title"
//   node cdpcli.mjs 19437 input "#searchInput" "周杰伦"
//   node cdpcli.mjs 19437 click "#searchGo"
//   node cdpcli.mjs 19437 wait "document.querySelectorAll('#hitList li').length > 0" 15000
import { writeFileSync } from 'node:fs';

const argv = process.argv.slice(2);
let waitReadySec = 0;
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--wait') {
    waitReadySec = Number(argv[i + 1] || 0);
    argv.splice(i, 2); i--;
  }
}
const [port, cmd, ...args] = argv;
const die = (code, msg) => { console.error(msg); process.exit(code); };

async function findPageTarget() {
  const res = await fetch(`http://127.0.0.1:${port}/json/list`)
    .catch(e => die(4, `连接 CDP 失败: ${e.cause?.message || e.message}`));
  const list = await res.json();
  const pages = list.filter(t => t.type === 'page' && !t.url.startsWith('devtools:'));
  if (!pages.length) die(4, `无 page 目标: ${JSON.stringify(list.map(t => ({ type: t.type, url: t.url })))}`);
  return pages[0];
}

function connect(wsUrl) {
  return new Promise((ok, bad) => {
    const ws = new WebSocket(wsUrl);
    let seq = 0; const pending = new Map();
    ws.addEventListener('open', () => ok({
      send(method, params = {}) {
        return new Promise((res, rej) => {
          const id = ++seq;
          pending.set(id, { res, rej });
          ws.send(JSON.stringify({ id, method, params }));
        });
      },
      close: () => ws.close(),
    }));
    ws.addEventListener('error', e => bad(new Error('WS 错误: ' + (e.message || e))));
    ws.addEventListener('message', ev => {
      const m = JSON.parse(ev.data);
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id); pending.delete(m.id);
        m.error ? p.rej(new Error(m.method + ' ' + JSON.stringify(m.error))) : p.res(m.result);
      }
    });
  });
}

async function evaluate(cdp, expression) {
  const r = await cdp.send('Runtime.evaluate', {
    expression, returnByValue: true, awaitPromise: true, userGesture: true,
  });
  if (r.exceptionDetails) {
    throw new Error('页面异常: ' + JSON.stringify(r.exceptionDetails.exception?.description || r.exceptionDetails, null, 2));
  }
  return r.result.value;
}

if (waitReadySec > 0) {                     // 等 CDP 端口就绪（WebView2 环境异步创建）
  const t0 = Date.now();
  for (;;) {
    const up = await fetch(`http://127.0.0.1:${port}/json/version`).then(r => r.ok).catch(() => false);
    if (up) break;
    if ((Date.now() - t0) / 1000 > waitReadySec) die(4, `CDP 端口 ${port} 在 ${waitReadySec}s 内未就绪`);
    await new Promise(r => setTimeout(r, 500));
  }
}

if (!port || !cmd) die(3, '用法: node cdpcli.mjs [--wait <秒>] <port> <cmd> [args...], 缺参数');

const page = await findPageTarget();
const cdp = await connect(page.webSocketDebuggerUrl);

try {
  switch (cmd) {
    case 'list': {
      const res = await fetch(`http://127.0.0.1:${port}/json/list`);
      console.log(JSON.stringify(await res.json(), null, 2));
      break;
    }
    case 'eval': {
      if (!args[0]) die(3, 'eval 缺表达式');
      console.log(JSON.stringify(await evaluate(cdp, args[0])));
      break;
    }
    case 'click': {
      if (!args[0]) die(3, 'click 缺选择器');
      const v = await evaluate(cdp,
        `(() => { const el = document.querySelector(${JSON.stringify(args[0])});
          if (!el) return null; el.click(); return true; })()`);
      v ? console.log('clicked', args[0]) : die(3, `选择器未命中: ${args[0]}`);
      break;
    }
    case 'input': {
      if (args.length < 2) die(3, 'input 缺参数');
      const v = await evaluate(cdp,
        `(() => { const el = document.querySelector(${JSON.stringify(args[0])});
          if (!el) return null; el.focus(); el.value = ${JSON.stringify(args[1])};
          el.dispatchEvent(new Event('input', { bubbles: true }));
          el.dispatchEvent(new Event('change', { bubbles: true }));
          return el.value; })()`);
      v === null ? die(3, `选择器未命中: ${args[0]}`) : console.log('value=', JSON.stringify(v));
      break;
    }
    case 'rawclick': {
      if (!args[0]) die(3, 'rawclick 缺选择器');
      const rect = await evaluate(cdp,
        `(() => { const el = document.querySelector(${JSON.stringify(args[0])});
          if (!el) return null; const r = el.getBoundingClientRect();
          return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
      if (!rect) die(3, `选择器未命中: ${args[0]}`);
      const { x, y } = rect;
      await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, button: 'none' });
      await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await new Promise(r => setTimeout(r, 60));
      await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
      console.log('rawclicked', args[0], 'at', Math.round(x), Math.round(y));
      break;
    }
    case 'send': {
      if (!args[0]) die(3, 'send 缺 JSON');
      const v = await evaluate(cdp, `typeof send === 'function' ? (send(${args[0]}), true) : false`);
      v ? console.log('sent', args[0]) : die(3, '页面无 send() 桥');
      break;
    }
    case 'wait': {
      if (!args[0]) die(3, 'wait 缺表达式');
      const timeout = Number(args[1] || 10000);
      const t0 = Date.now(); let last;
      for (;;) {
        last = await evaluate(cdp, args[0]);
        if (last) { console.log(JSON.stringify(last)); process.exit(0); }
        if (Date.now() - t0 > timeout) die(3, `wait 超时(${timeout}ms), 最后值: ${JSON.stringify(last)}`);
        await new Promise(r => setTimeout(r, 300));
      }
    }
    case 'shot': {
      if (!args[0]) die(3, 'shot 缺文件名');
      await cdp.send('Page.enable');
      const r = await cdp.send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(args[0], Buffer.from(r.data, 'base64'));
      console.log('saved', args[0]);
      break;
    }
    default: die(3, `未知命令: ${cmd}`);
  }
} catch (e) {
  die(3, '失败: ' + e.message);
} finally {
  cdp.close();
}
