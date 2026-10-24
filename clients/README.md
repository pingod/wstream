# wstream_client — Python 客户端

给 AI agent 用的零依赖客户端。不用再手工复制 `SKILL.md` 里的代码片段。

## 安装 / 引入

不需要安装，把 `clients/` 加进 `sys.path` 即可：

```python
import sys; sys.path.insert(0, "/path/to/wstream/clients")
from wstream_client import WStream
```

## 典型用法

```python
from wstream_client import WStream

with WStream() as ws:
    ws.wait_ready()                       # start.sh 刚返回时它可能还没初始化完
    ws.take()                             # ⚠️ 不调这个，所有输入都会被服务端丢弃

    # 看有哪些窗口、各在哪
    for w in ws.windows():
        print(w["app_id"], w["title"], (w["cx"], w["cy"]))

    # 点窗口中心并操作
    ws.click_window("AgentTop")
    ws.type_text("echo hello")            # ASCII 逐键
    ws.press("enter")

    # 中文（自动走剪贴板粘贴）
    ws.type_any("中文内容")
    ws.press("enter")

    # 截图存盘（不用浏览器、不用 grim）
    ws.snapshot("/tmp/shot.jpg")

    # 剪贴板
    ws.set_clipboard("给沙箱的内容")
    print(ws.get_clipboard())
```

## API 速查

| 方法 | 说明 |
|---|---|
| `connect()` / `close()` / `with` | 连接管理 |
| `take()` / `release()` | 取得 / 释放输入权（**必须先 take**） |
| `windows()` | `[{app_id, title, x, y, width, height, cx, cy}]` |
| `window(app_id=, title_contains=)` | 找一个窗口 |
| `move(x,y)` / `click(x,y)` / `click_window()` | 鼠标（像素坐标） |
| `scroll(steps)` | 滚轮，正数向上 |
| `tap(ch)` / `type_text(s)` | 逐键输入 ASCII |
| `type_any(s)` | 任意文本：ASCII 走逐键，含中文自动走剪贴板 |
| `paste(s, keys=)` | 写剪贴板 + 粘贴快捷键 |
| `combo('ctrl','shift',key='v')` | 组合键 |
| `press('enter')` | 功能键（enter/tab/esc/backspace/方向键…） |
| `set_clipboard()` / `get_clipboard()` | 沙箱剪贴板 |
| `snapshot(path=None)` | 取实时截图（JPEG） |
| `run_in_window(cmd, app_id=)` | 点窗口 + 输入命令 + 回车 |

## 三个容易踩的坑

**① 不 `take()` 什么都干不了。**
服务端会丢弃无输入权的客户端的全部输入，只在日志里打一行警告。这是 1.1.0
加的机制，防止多个客户端互相抢鼠标。

**② 终端里粘贴是 `Ctrl+Shift+V`，不是 `Ctrl+V`。**
`Ctrl+V` 在终端里是「引号转义」。`paste()` 默认用 `ctrl+shift+v`；
往 GUI 程序（浏览器、编辑器）里贴可以传 `keys="ctrl+v"`。

**③ 中文必须走剪贴板。**
逐键注入的 Unicode 到不了 XWayland（keycode 9 冲突），所以 `type_any()`
遇到非 ASCII 会自动转成「写剪贴板 + 粘贴」。纯 ASCII 仍走逐键（更快更稳）。

## 相关端点

客户端背后用到三个 HTTP 端点 + 一个 WebSocket：

| 端点 | 用途 |
|---|---|
| `GET /` | 浏览器交互页面 |
| `GET /stream.mjpg` | MJPEG 流（浏览器 `<img>` 用） |
| `GET /snapshot.jpg` | 单帧 JPEG（agent 截图用，带 800ms 缓存） |
| `GET /windows` | 窗口列表 + 精确坐标（读 sway IPC） |
| `WS /ws` | 输入注入 + 剪贴板 |
