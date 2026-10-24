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

### 方式 A：通过 WebSocket 注入（推荐，可脚本化）

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
    s.sendall(b"GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
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
| 窗口跑到用户桌面上 | 没 `env -u DISPLAY` | 见 `docs/HANDOFF.md` 坑 5 |
| `sway: Operation not permitted` | 容器缺 `CAP_SYS_NICE` | `start.sh` 已自动绕过；或 `--cap-add SYS_NICE` |

详细版：`docs/TROUBLESHOOTING.md`

---

## 6. 能力边界（别承诺做不到的）

| 限制 | 说明 |
|---|---|
| **XWayland 程序注不进去** | firefox / Electron 默认走 X11，wstream 只能操作原生 Wayland 客户端。要让它们走 Wayland：加 `MOZ_ENABLE_WAYLAND=1`（firefox）或 `--ozone-platform=wayland`（Electron/Chromium） |
| 只能本机访问 | 只监听 `127.0.0.1`，无鉴权无 TLS |
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
