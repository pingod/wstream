---
name: wstream
description: >
  用浏览器实时查看并操作一个与用户桌面完全隔离的无头 Wayland 虚拟桌面。
  Use when 需要「操作 GUI 但不干扰用户」——让 agent 点击/输入某个图形界面程序、
  自动化测试桌面应用、在沙箱里跑浏览器或终端、给用户演示 GUI 操作过程，
  或用户说"帮我点一下/在界面里操作/开个虚拟桌面/远程操作界面但不抢我键鼠"。
  也用于排查 wstream 本身（画面黑、鼠标点不动、打字乱码、键盘无反应）。
  不做：远程访问公网（只监听 127.0.0.1）、XWayland 程序注入、音频。
metadata:
  homepage: https://github.com/pingod/wstream
---

# wstream — 隔离的虚拟桌面

给 AI agent 一个**独立的无头桌面**：用户在浏览器里看，agent 在里面操作，
**物理上不共享输入设备**，不会抢用户的键盘鼠标和焦点。

---

## 0. 什么时候用 / 不用

**用**：
- 要操作某个 GUI 程序，但绝不能打扰用户
- 需要演示「AI 在界面上怎么操作」
- 自动化测试桌面应用
- 在隔离环境里跑浏览器（抓网页、截图、验证渲染）

**不用**：
- 只是取网页内容 → 直接用 `web_fetch`
- 只需要命令行 → 直接用 `bash`
- 要操作 XWayland 程序（firefox/Electron 默认走 X11）→ 见第 6 节

---

## 1. 快速开始

```bash
cd ~/githome/wstream          # 或任何 clone 下来的位置
./build.sh                    # 首次需要编译
./scripts/start.sh            # 默认 1600x1000，端口 8765
```

然后告诉用户：**浏览器打开 <http://127.0.0.1:8765>，点一下画面就能接管**。

停止：

```bash
./scripts/start.sh stop
```

### 首次安装依赖（Arch / CachyOS）

```bash
sudo pacman -S --needed gcc pkgconf wayland libjpeg-turbo libxkbcommon \
    wlr-protocols sway grim
```

---

## 2. 怎么在沙箱里操作

有两种方式，**优先用第一种**。

### 方式 A0：用客户端库（**最推荐**）

仓库自带 `clients/wstream_client.py`，零依赖，不用手工复制代码：

```python
import sys; sys.path.insert(0, "/path/to/wstream/clients")
from wstream_client import WStream

with WStream() as ws:
    ws.wait_ready()                       # start.sh 刚返回时可能还没初始化完
    ws.take()                             # ⚠️ 不调这个，所有输入都会被丢弃

    for w in ws.windows():                # 知道该点哪里
        print(w["app_id"], (w["cx"], w["cy"]))

    ws.click_window("AgentTop")           # 点窗口中心
    ws.type_text("echo hello")            # ASCII 逐键
    ws.press("enter")
    ws.type_any("中文内容")                # 含非 ASCII 自动走剪贴板
    ws.press("enter")
    ws.snapshot("/tmp/now.jpg")           # 取一张截图看图
```

API 速查见 `clients/README.md`。下面两种方式是它的底层实现，
需要精细控制时再看。

### 方式 A：通过 WebSocket 注入（底层，可脚本化）

沙箱启动后，`ws://127.0.0.1:8765/ws` 接受 **8 字节二进制帧**（小端）：

```
[0]=type  [1]=保留(0)  [2..3]=保留(0)  [4..5]=A  [6..7]=B
```

| type | A | B | 说明 |
|---|---|---|---|
| `1` | x（**uint16** 0..65535 归一化） | y（**uint16**） | 鼠标移动 |
| `2` | button（1=左 2=中 3=右） | state（1=按下 0=抬起） | 鼠标按键 |
| `3` | steps（**int16**，负=向下） | 0 | 滚轮 |
| `4` | evdev 扫描码 | state（1=按下 0=抬起） | 键盘 |

**Python 示例**（可直接复制用）：

```python
import socket, base64, time, struct, os

def ws_connect(port=8765):
    s = socket.create_connection(('127.0.0.1', port), timeout=8)
    # ⚠️ Host 必须是回环地址；Origin 可省略（省略 = 本机进程，放行），
    #    但填了就必须是本页来源，否则服务端回 403。
    s.sendall(b"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:8765\r\nUpgrade: websocket\r\n"
              b"Connection: Upgrade\r\nSec-WebSocket-Key: " +
              base64.b64encode(b"abcdefgh12345678") +
              b"\r\nSec-WebSocket-Version: 13\r\n\r\n")
    time.sleep(0.35); s.recv(256)
    return s

def send(s, t, a, b):
    # ⚠️ 必须用无符号 'H' 打包；坐标 > 32767 时用 'h' 会变负数
    p = bytes([t, 0, 0, 0]) + struct.pack('<HH', a & 0xffff, b & 0xffff)
    mask = os.urandom(4)
    s.sendall(bytes([0x82, 0x80 | len(p)]) + mask +
              bytes(c ^ mask[i % 4] for i, c in enumerate(p)))
    time.sleep(0.02)

# 鼠标：移到 (900, 500)，屏幕 1280x720
def move(s, x, y, W=1280, H=720):
    send(s, 1, int(x / W * 65535), int(y / H * 65535))

def click(s, button=1):
    send(s, 2, button, 1); time.sleep(0.04); send(s, 2, button, 0)

# ⚠️ 1.1.0 起必须**先取输入权**，否则所有输入会被服务端丢弃
def take(s):      # 取得操作权（同一时刻只有一个人能操作）
    send(s, 6, 1, 0); time.sleep(0.15)

def release(s):   # 用完释放，让别人也能操作
    send(s, 6, 0, 0)

# 中文 / emoji：8 字节头 + UTF-8 负载（type 5）
def type_text_raw(s, text):
    payload = bytes([5,0,0,0]) + struct.pack('<HH', 0, 0) + text.encode('utf-8')
    m = os.urandom(4); n = len(payload)
    hdr = (bytes([0x82, 0x80|n]) if n < 126
           else bytes([0x82, 0x80|126]) + struct.pack('>H', n))
    s.sendall(hdr + m + bytes(c ^ m[i % 4] for i, c in enumerate(payload)))
    time.sleep(0.05)
```

**最小可用流程**：

```python
s = ws_connect()
take(s)                    # ← 别忘
move(s, 640, 360, 1280, 720)
click(s)
type_text(s, "echo hi")    # ASCII，逐键
send(s, 4, 28, 1); send(s, 4, 28, 0)   # Enter
type_text_raw(s, "中文也能打")
release(s)
```

**键盘用 evdev 扫描码**，常用映射：

```python
KEY = {
 'a':30,'b':48,'c':46,'d':32,'e':18,'f':33,'g':34,'h':35,'i':23,'j':36,
 'k':37,'l':38,'m':50,'n':49,'o':24,'p':25,'q':16,'r':19,'s':31,'t':20,
 'u':22,'v':47,'w':17,'x':45,'y':21,'z':44,
 '1':2,'2':3,'3':4,'4':5,'5':6,'6':7,'7':8,'8':9,'9':10,'0':11,
 ' ':57,'.':52,',':51,'/':53,'-':12,'=':13,';':39,"'":40,'[':26,']':27,
 '\n':28,'\t':15,   # Enter / Tab
}
SHIFT, CTRL, ALT, SUPER = 42, 29, 56, 125
ESC, BACKSPACE = 1, 14
F1,F2,F3,F4,F5,F6 = 59,60,61,62,63,64
LEFT,RIGHT,UP,DOWN = 105,106,103,108
HOME,END,PAGEUP,PAGEDOWN = 102,107,104,109
```

打字（含大写和符号）：

```python
def tap(s, code, shift=False):
    if shift: send(s, 4, SHIFT, 1); time.sleep(0.02)
    send(s, 4, code, 1); time.sleep(0.02)
    send(s, 4, code, 0)
    if shift: time.sleep(0.02); send(s, 4, SHIFT, 0)
    time.sleep(0.03)

def type_text(s, text):
    SHIFTED = {'!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8',
               '(':'9',')':'0','_':'-','+':'=',':':';','"':"'",'{':'[','}':']',
               '|':'\\','~':'`','<':',','>':'.','?':'/'}
    for ch in text:
        if ch.isupper():
            tap(s, KEY[ch.lower()], shift=True)
        elif ch in SHIFTED:
            tap(s, KEY[SHIFTED[ch]], shift=True)
        elif ch in KEY:
            tap(s, KEY[ch])

# 组合键：Ctrl+L
def combo(s, mod, key):
    send(s, 4, mod, 1); time.sleep(0.02)
    tap(s, key)
    time.sleep(0.02); send(s, 4, mod, 0)
```

### 剪贴板（输入中文的最可靠方式）

逐键注入的中文到不了 XWayland（keycode 9 冲突）。要往 **X11 程序**里输入中文，
必须走剪贴板：

```python
def set_sandbox_clipboard(s, text):
    """type 7：把文本设成沙箱剪贴板"""
    payload = bytes([7,0,0,0]) + struct.pack('<HH', 0, 0) + text.encode('utf-8')
    m = os.urandom(4); n = len(payload)
    hdr = (bytes([0x82, 0x80|n]) if n < 126
           else bytes([0x82, 0x80|126]) + struct.pack('>H', n))
    s.sendall(hdr + m + bytes(c ^ m[i % 4] for i, c in enumerate(payload)))
    time.sleep(0.15)

# 用法：写剪贴板 → Ctrl+V
set_sandbox_clipboard(s, "中文内容")
send(s, 4, 29, 1)   # Ctrl 按下
send(s, 4, 47, 1)   # V 按下（evdev 47 = v）
send(s, 4, 47, 0)
send(s, 4, 29, 0)
```

读沙箱剪贴板（围观也能读，不要求接管）：

```python
send(s, 8)                 # 请求
# 响应是下行帧：[0]=0x81 [4..5]=版本 [8..]=UTF-8 文本
```

在沙箱里**外部**操作剪贴板（不改代码，直接命令行）：

```bash
export XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=$(cat /tmp/wstream/wl_name)
printf '内容' | wl-copy     # 写
timeout 2 wl-paste          # 读
```

### 多 agent 并行：每个 agent 一套独立桌面

多个 agent 同时要 GUI 时，各起一个实例，互不干扰：

```bash
WSTREAM_ROOT=/tmp/ws-agent1 WSTREAM_PORT=8801 ./scripts/start.sh
WSTREAM_ROOT=/tmp/ws-agent2 WSTREAM_PORT=8802 ./scripts/start.sh
```

然后各用各的端口：

```python
with WStream(port=8801) as ws:   # agent 1
    ...
```

查看有哪些实例在跑：

```bash
./scripts/list.sh        # 表格
./scripts/list.sh --url  # 只要 URL
```

**注意**：`stop` 必须带 `WSTREAM_ROOT`，否则停的是默认实例：
```bash
WSTREAM_ROOT=/tmp/ws-agent1 ./scripts/start.sh stop
```

### 方式 B：在沙箱里直接跑命令（更简单）

如果只是要"在沙箱里执行某个 GUI 程序"，不用注入，直接启动即可：

```bash
ROOT=/tmp/wstream
WL=$(cat $ROOT/wl_name)          # 实际 socket 名，不一定是 wayland-0

nohup setsid env -u DISPLAY \
  XDG_RUNTIME_DIR=$ROOT/run WAYLAND_DISPLAY=$WL \
  GDK_BACKEND=wayland \
  <你的程序> > /tmp/sandbox-app.log 2>&1 < /dev/null &
disown
```

> ⚠️ **`-u DISPLAY` 和 `GDK_BACKEND=wayland` 一个都不能少**，
> 否则程序会连到用户真实桌面的 Xwayland，窗口直接出现在用户屏幕上。

---

## 3. 看沙箱画面（不用浏览器）

```bash
ROOT=/tmp/wstream
WL=$(cat $ROOT/wl_name)

# 抓一张图（-c 会把鼠标光标也画进去）
XDG_RUNTIME_DIR=$ROOT/run WAYLAND_DISPLAY=$WL grim -c /tmp/shot.png

# 用 wstream 自己的快照模式（不需要另装 grim）
XDG_RUNTIME_DIR=$ROOT/run WAYLAND_DISPLAY=$WL \
  ./wstream -p 8899 -s /tmp/shot.ppm
ffmpeg -i /tmp/shot.ppm /tmp/shot.png
```

然后用 `read_image` 看 `/tmp/shot.png`。

---

## 4. 验证隔离性（改动启动逻辑后必做）

```bash
# 1. 沙箱进程的环境必须指向沙箱
for p in $(pgrep -x <程序名>); do
  tr '\0' '\n' < /proc/$p/environ | grep -E "^(DISPLAY|XDG_RUNTIME_DIR)="
done
# 期望：DISPLAY 为空，XDG_RUNTIME_DIR=/tmp/wstream/run

# 2. 用户真实桌面窗口数不该变
hyprctl clients | grep -c "^Window"

# 3. 注入后用户光标不该动
hyprctl cursorpos
```

---

## 5. 排查速查

| 症状 | 最可能原因 | 动作 |
|---|---|---|
| 浏览器一片黑 | 沙箱里没窗口 | `grim` 截图确认；检查 `desktop.sh` 是否执行 |
| 只有第一帧 | poll 超时没 dispatch_pending | 看 `src/wstream.c` 主循环 |
| 鼠标完全不动 | ① `cap.ptr` 为 NULL ② 合成器不支持 `motion_absolute` | `WAYLAND_DEBUG=client ./wstream \| grep motion`；换 sway |
| 只有左半边能点 | 坐标按 int16 读了 | 见 `docs/HANDOFF.md` 坑 3 |
| 打字乱码（a→l） | 键码多加了 8 | 见 `docs/HANDOFF.md` 坑 1 |
| 打字完全无反应 | WS 帧不是 8 字节 | 见 `docs/HANDOFF.md` 坑 2 |
| **所有输入都没反应** | 没取输入权（type 6 action=1） | 先 `take(s)`，见坑 7 相关 |
| **点哪里都不准** | `motion_absolute` 坐标要像素不是归一化 | 见坑 8；脚本用 `move()` 即可 |
| **点击没反应** | 按钮码要 BTN_LEFT(0x110) | 见坑 10 |
| **页面读剪贴板超时** | type 7/8 被输入权拦了 / 主循环没查 g_clip_dirty | 见坑 15 |
| **X11 程序里中文进不去** | 逐键到不了 XWayland | 改用剪贴板粘贴 |
| **大写字母变成全大写** | 修饰键释放时没发 modifiers | 见坑 18 |
| **Ctrl+V 没反应、多出字母 v** | 每键推 keymap 清零修饰位 / 终端要 Ctrl+Shift+V | 见坑 17、19 |
| 握手回 403 | `Host`/`Origin` 不是本机来源 | 用 `Host: 127.0.0.1:8765`，别带外站 `Origin`，见坑 6 |
| 服务随机崩 / 越界 | `clients[]` 跨 poll 用下标 | 见 `docs/HANDOFF.md` 坑 7 |
| 窗口跑到用户桌面上 | 没 `env -u DISPLAY` | 见 `docs/HANDOFF.md` 坑 5 |
| `sway: Operation not permitted` | 容器缺 `CAP_SYS_NICE` | `start.sh` 已自动绕过；或 `--cap-add SYS_NICE` |

详细版：`docs/TROUBLESHOOTING.md`

---

## 6. 能力边界（别承诺做不到的）

| 限制 | 说明 |
|---|---|
| **XWayland 程序注不进去** | firefox / Electron 默认走 X11，wstream 只能操作原生 Wayland 客户端。要让它们走 Wayland：加 `MOZ_ENABLE_WAYLAND=1`（firefox）或 `--ozone-platform=wayland`（Electron/Chromium） |
| 只能本机访问 | 只监听 `127.0.0.1`；已校验 `Host`/`Origin`，但无 token、无 TLS |
| 有 `/snapshot.jpg` 和 `/windows` | 前者取单帧、后者查窗口坐标，不用装 grim |
| Xwayland 里的中文 | 逐键到不了，但**走剪贴板可以**（type 7 + Ctrl+V） |
| 无音频 | 只传画面 |
| 无剪贴板同步 | 沙箱与宿主机剪贴板不通 |
| 仅 wlroots 系合成器 | sway / labwc 可以；Hyprland 的 headless 后端受限于 aquamarine |

---

## 7. 红线

1. **绝不把沙箱内容弄到用户真实桌面上** —— 窗口、输入、焦点都不行。
   这是这个项目存在的全部意义，破了就没价值。
2. **不要用 `pkill -f <关键字>`** —— bash 自己的命令行也含该字符串，会杀掉自己。
   用 `pgrep -x`（精确匹配进程名）。
3. **不要高频截图循环给用户看** —— 用户明确反感。要看画面就让他自己开浏览器。
   （验证时抓 1~2 张确认即可。）
4. 启动沙箱里的任何 GUI 程序前，先想一遍 `DISPLAY` 是否被隔离了。

---

## 8. 项目文档地图

| 想知道 | 看 |
|---|---|
| 项目介绍、用法、性能数据 | `README.md` |
| Wayland 协议细节 | `docs/PROTOCOLS.md` |
| 线程模型、主循环 | `docs/ARCHITECTURE.md` |
| 症状排查 | `docs/TROUBLESHOOTING.md` |
| **接手必读：五个坑 + 现状** | `docs/HANDOFF.md` |
| 版本历史 | `CHANGELOG.md` |
