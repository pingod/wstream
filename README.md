# wstream

**把无头 Wayland 桌面实时推流到浏览器，并支持用鼠标键盘反向操作。**

一个约 1100 行的纯 C 程序：不依赖 VNC / RDP / PipeWire，只用 Wayland 原生协议，
在浏览器里就能看到并操作一个**与你自己桌面完全隔离**的虚拟桌面。

```
┌─────────────────────────────────────────────────────────────────┐
│  你的真实桌面（Hyprland）          ← 全程不受影响，不抢焦点      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  沙箱：sway (WLR_BACKENDS=headless)  ← 独立 socket，独立进程     │
│                                                                  │
│   zwlr_screencopy_manager_v1 ──抓屏──┐                          │
│                                      ▼                          │
│   wstream ── BGRX → JPEG (libjpeg-turbo, ~80KB/帧)              │
│      │                                                           │
│      └── HTTP multipart/x-mixed-replace ──► 浏览器 <img>        │
│                                                                  │
│   zwlr_virtual_pointer_v1 ◄── WebSocket 8 字节帧 ◄── 浏览器键鼠 │
│   zwp_virtual_keyboard_v1                                        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 目录

- [为什么做这个](#为什么做这个)
- [特性](#特性)
- [快速开始](#快速开始)
- [依赖](#依赖)
- [用法](#用法)
- [架构](#架构)
- [通信协议](#通信协议)
- [性能实测](#性能实测)
- [隔离性说明](#隔离性说明)
- [踩过的坑](#踩过的坑)
- [多实例（多个 agent 各一套桌面）](#多实例多个-agent-各一套桌面)
- [已知限制](#已知限制)
- [安全边界](#安全边界)
- [开发](#开发)
- [给 AI agent 用（skill）](#给-ai-agent-用skill)
- [文档索引](#文档索引)
- [许可证](#许可证)

---

## 为什么做这个

让 AI agent（或任何自动化工具）操作 GUI 时，传统做法是在用户自己的桌面上开窗口、
模拟点击——这会**直接抢占用户的键盘鼠标和焦点**，用户什么都干不了。

wstream 的思路是：**给 agent 一个完全独立的虚拟桌面**。

- 用户看的是浏览器里的一个网页
- agent 操作的是那个无头沙箱
- 两者通过 HTTP + WebSocket 连接，**物理上不共享输入设备**

这样 agent 可以随便折腾，用户该干嘛干嘛。

---

## 特性

| 特性 | 说明 |
|---|---|
| **零窗口** | 不往用户桌面创建任何窗口，不抢焦点 |
| **实时画面** | MJPEG 流，浏览器 `<img>` 原生支持，无需 JS 解码 |
| **双向交互** | 鼠标移动/点击/滚轮 + 完整键盘（含 Ctrl/Shift/Alt/Super 组合键） |
| **中文 / 任意 Unicode** | IME 输入、粘贴、emoji 都能进沙箱（走 keymap 重映射） |
| **多人围观不打架** | 同一时刻只有一个人能操作，其余人只读；控制权可转移 |
| **剪贴板双向同步** | 沙箱复制 → 页面能拿到；页面粘贴 → 沙箱能收到（走 data-control） |
| **给 agent 的 API** | `GET /snapshot.jpg` 取单帧、`GET /windows` 查窗口坐标、可 import 的 Python 客户端 |
| **纯 C** | 单文件 ~1100 行，无运行时依赖（除 3 个系统库） |
| **低开销** | 推流进程 CPU ~0.1%，带宽 ~0.16 MB/s @10fps |
| **零配置** | `./scripts/start.sh` 一条命令起全套 |

---

## 快速开始

```bash
git clone https://github.com/pingod/wstream.git
cd wstream
./build.sh                 # 编译
./scripts/start.sh         # 起沙箱 + 推流
```

然后浏览器打开 **<http://127.0.0.1:8765>**。

页面会显示沙箱桌面的实时画面。**点击画面即可接管**，之后你的鼠标键盘就作用在沙箱里了；
按 `Ctrl+Esc` 释放控制权。

停止：

```bash
./scripts/start.sh stop
```

---

## 依赖

### 运行时

| 依赖 | 用途 | Arch 包名 |
|---|---|---|
| `libwayland-client` | Wayland 客户端 | `wayland` |
| `libjpeg-turbo` | JPEG 编码 | `libjpeg-turbo` |
| `libxkbcommon` | keymap 生成 | `libxkbcommon` |
| `wlr-protocols` | screencopy / virtual-pointer 协议 XML | `wlr-protocols` |
| `sway` | 沙箱合成器 | `sway` |

### 编译时额外需要

```bash
gcc pkgconf wayland-scanner
```

### 一键安装（Arch / CachyOS）

```bash
sudo pacman -S --needed gcc pkgconf wayland wayland-protocols \
    libjpeg-turbo libxkbcommon wlr-protocols sway
```

> **为什么用 sway 而不是 labwc？**
> `zwlr_virtual_pointer_v1` 的 `motion_absolute` 请求**不是所有合成器都实现**。
> labwc 只接了 `wlr_seat_pointer_notify_motion`（相对位移），absolute 请求会被
> **静默丢弃**——表现为「键盘能打字，但鼠标点了没反应、也切不了窗口」。
>
> 因此 wstream 当前**不用 absolute**，改用相对位移累积模拟绝对定位（见
> `docs/PROTOCOLS.md` 第三节）；选 sway 的真实理由是它的点击聚焦和光标渲染更完整。
> 换合成器前请重新实测，别假设换个轻量的就能直接跑。

---

## 用法

### 完整沙箱（推荐）

```bash
./scripts/start.sh              # 默认 1600x1000
./scripts/start.sh 1920x1080    # 指定分辨率
./scripts/start.sh stop         # 停止
```

脚本会：
1. 起一个 `sway --unsupported-gpu`，`WLR_BACKENDS=headless`（无头）
2. 在沙箱里跑两个不停刷新的终端（方便确认画面是实时的）
3. 起 `wstream` 监听 `127.0.0.1:8765`

### 直接推流任意 wlroots 合成器

```bash
XDG_RUNTIME_DIR=/path/to/run WAYLAND_DISPLAY=wayland-1 ./wstream -p 8765 -f 10 -q 70
```

### 命令行参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `-p PORT` | HTTP 端口（**只监听 127.0.0.1**） | `8765` |
| `-f FPS` | 帧率，范围 1..30 | `10` |
| `-q QUALITY` | JPEG 质量，范围 1..100 | `70` |
| `-s FILE.ppm` | 抓一帧存 PPM 后退出（不需要浏览器，用于验证） | — |
| `-v` | 详细日志（会把 keymap 落盘到 `/tmp/wstream-keymap.txt`） | off |

### 环境变量

| 变量 | 说明 | 默认 |
|---|---|---|
| `WSTREAM_ROOT` | 沙箱根目录 | `/tmp/wstream` |
| `WSTREAM_PORT` | HTTP 端口 | `8765` |
| `WLR_PROTO_DIR` | wlr 协议 XML 目录 | `/usr/share/wlr-protocols/unstable` |

---

## 架构

### 线程模型

```
主线程                          网络线程 (http_thread)
────────                        ──────────────────────
wl_display_dispatch              poll() 所有连接
  ↓                              ↓
screencopy 抓帧                  accept / 读 HTTP / 读 WS
  ↓                              ↓
JPEG 编码                        handle_msg() → inject_*()
  ↓                              ↓
push_frame() ──写──► clients[] ◄── g_cl_mu 互斥锁
  ↑                                  │
  └──── g_input_dirty 标志 ◄─────────┘
        主线程看到后做一次 roundtrip
```

`clients[]` 用单调递增的 `id` 做句柄，不用下标也不用 fd —— 因为 `poll()` 前后数组
可能被主线程的 `drop_client()` 搬动。详见 `docs/ARCHITECTURE.md` 的「全局锁与句柄」。

**为什么输入注入不直接在网络线程里 roundtrip？**
Wayland 事件只能由**一个**线程 dispatch。两个线程同时 dispatch 同一个 `wl_display`
会让 libwayland 断言崩溃：

```
wl_display_dispatch_queue: Assertion 'ret == -1 || ret > 0' failed
```

所以网络线程只 `wl_display_flush()` + 置 `g_input_dirty`，由主线程补 `roundtrip`。

### 抓帧循环

```
capture_output() → frame.ready 回调 → 拷贝到 wl_shm 缓冲
                                            ↓
                     JPEG 编码 → multipart 分块写 → 浏览器
```

**注意**：合成器在画面不变时不会给可读事件。裸 `poll()` 会让抓帧永远等不到第二帧，
所以等待循环里用 `wl_display_dispatch` / `dispatch_pending` 配合，保证事件被处理。

---

## 通信协议

### 下行：MJPEG（浏览器 → 画面）

```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg
Content-Length: 81234

<JPEG 字节流>
--frame
...
```

浏览器 `<img src="/stream.mjpg">` 原生支持，**零 JavaScript 解码开销**。

### 端点一览

| 端点 | 用途 | 谁用 |
|---|---|---|
| `GET /` | 交互页面（内嵌单页） | 浏览器 |
| `GET /stream.mjpg` | MJPEG 流 | 浏览器 `<img>` |
| `GET /snapshot.jpg` | 单帧 JPEG，带 800ms 缓存 | agent 截图 |
| `GET /windows` | 窗口列表 + 精确坐标（读 sway IPC） | agent 定位 |
| `WS /ws` | 输入注入 + 剪贴板 | 浏览器 / 脚本 |

`/snapshot.jpg` 和 `/windows` 是为 agent 加的：前者让它「看一眼现在什么样」
不必订阅流，后者让它知道「该点哪里」。以前只能重启进程用 `-s` 截图，
或者装 `grim`，或者靠截图猜坐标。

```bash
curl -s http://127.0.0.1:8765/snapshot.jpg -o /tmp/now.jpg
curl -s http://127.0.0.1:8765/windows
# {"windows":[{"app_id":"AgentTop","x":0,"y":0,"width":640,"height":720,"cx":320,"cy":360}]}
```

> **为什么 `/windows` 不用 `zwlr_foreign_toplevel_management`**：那个协议
> 只给 title / app_id，**不给坐标**。而坐标恰恰是 agent 最需要的。

### 下行：WebSocket 剪贴板（沙箱 → 页面）

沙箱剪贴板变化时，或在页面主动请求（type 8）后，服务端发一个二进制帧：

```
[0]=0x81  [1..3]=保留  [4..5]=版本号  [6..7]=保留  [8..]=UTF-8 文本
```

`0x80` 位标记这是服务端→客户端，低 4 位 `1` 表示剪贴板，与上行的 type 1
（鼠标移动）区分开。

### 上行：WebSocket（浏览器 → 输入）

端点 `ws://127.0.0.1:8765/ws`，**二进制帧，固定 8 字节**，小端序：

```
偏移  长度  字段      说明
────────────────────────────────────────────────────
 0     1   type     1=鼠标移动 2=按键 3=滚轮 4=键盘
 1     1   reserved  保留，填 0
 2     2   reserved  保留，填 0
 4     2   A         int16/uint16（含义见下）
 6     2   B         int16/uint16（含义见下）
```

| type | A | B | 说明 |
|---|---|---|---|
| `1` 鼠标移动 | x（**uint16** 0..65535，归一化） | y（**uint16** 0..65535） | 绝对定位 |
| `2` 鼠标按键 | button（1=左 2=中 3=右） | state（1=按下 0=抬起） | |
| `3` 滚轮 | steps（**int16**，负数=向下） | 0 | |
| `4` 键盘 | evdev code | state（1=按下 0=抬起） | |
| `5` **文本** | 0 | 0 | 8 字节头 + UTF-8 负载（byte 8 起），中文/emoji 走这条 |
| `6` **控制** | action（1=取权 0=释放） | 0 | 输入所有权 |
| `7` **写剪贴板** | 0 | 0 | 8 字节头 + UTF-8 负载，设成沙箱剪贴板 |
| `8` **读剪贴板** | 0 | 0 | 请求沙箱剪贴板，响应走下行 `0x81` |

> ⚠️ **A/B 的符号性随 type 变化**：坐标是无符号 16 位（0..65535），
> 滚动步数是有符号 16 位。服务端必须按 type 分开解释，否则屏幕右半边
> （x > 32767）会被解成负数而完全点不动。这个 bug 真实存在过，见[踩过的坑](#踩过的坑)。

### 中文 / 任意 Unicode（type 5）

键盘布局里没有「中」这个键，所以非 ASCII 不能靠键码。做法是**临时换一张
只含该字符的 keymap**，按一下再换回来：

```
xkb_keycodes "(unnamed)" { minimum = 8; maximum = 10; <K1> = 9; };
xkb_symbols  "(unnamed)" { key <K1> {[U4E2D]}; };   // U4E2D = 「中」
```

`UXXXX` 是 xkb 的 Unicode keysym 写法。要点：

- 顺序必须 **keymap → key**，中间要有一次 roundtrip
- **用完必须把完整 evdev keymap 推回去**，否则后续普通按键全落到那张
  只有一个键的表上（wtype 就有这个毛病）
- 只对**原生 Wayland 客户端**有效；Xwayland 有独立 keymap，keycode 9 在
  它那里永远是 Escape

前端支持三种来源：IME 的 `compositionend`、粘贴事件、非 ASCII 的 `input` 兜底。
IME 组合期间（`compositionstart` 之后）的按键会被忽略，否则拼音字母也会被敲进去。

### 剪贴板同步（type 7 / 8）

沙箱和页面之间双向打通。技术上用 `zwlr_data_control`，**不是** `wl_data_device`：

> `wl_data_device.set_selection()` 要求 surface 有焦点，而 wstream 根本没有窗口。
> data-control 是给剪贴板管理器用的协议，不需要焦点。

| 方向 | 流程 |
|---|---|
| 沙箱 → 页面 | 合成器发 `selection` → `receive` 到管道 → 缓存 → 页面发 type 8 取走 |
| 页面 → 沙箱 | 页面发 type 7 → `create_data_source` + `set_selection` |

前端行为：
- 每 2 秒自动同步一次，状态栏显示预览
- 粘贴时**含非 ASCII 走剪贴板**（写进沙箱剪贴板再发 Ctrl+V），纯 ASCII 仍逐键打
- `Ctrl+Shift+V` 把沙箱剪贴板复制到你自己的电脑

> **为什么粘贴要绕剪贴板**：逐键注入的中文到不了 XWayland（keycode 9 冲突）。
> 走剪贴板 + Ctrl+V 则所有程序都能收到。实测 X11 客户端收到 `CLIP 中文通过剪贴板`。

剪贴板帧**不受输入所有权限制**（围观者也能复制），数据上限 256KB。

### 输入所有权（type 6）

多个浏览器同时连时，如果谁都能注入，鼠标会互相拉扯。规则：

```
第一个发 type 6 action=1 的客户端 → 成为持有者
持有者断开                        → 自动释放
显式发 action=0                   → 释放
非持有者的输入                    → 服务端直接丢弃
```

> ⚠️ **必须服务端强制。** 只在前端隐藏按钮是不够的——任何脚本都能直接连
> `/ws` 发帧，绕过界面。

### 键盘映射

前端把浏览器 `KeyboardEvent.keyCode` 映射成 **Linux evdev 扫描码**（如 `a` → 30）。
服务端直接把这个值传给 `zwp_virtual_keyboard_v1.key()`，**不要 +8**。

> ⚠️ 合成器自己会做 `evdev + 8 → xkb keycode` 的转换。如果这里再加 8，
> 等于加了 16，敲 `a` 会出来 `l`（实测：`abc` → `l]c`）。

---

## 性能实测

测试环境：1600×1000 输出，JPEG 质量 70，10 fps。

| 指标 | 值 |
|---|---|
| 单帧体积 | ~80 KB |
| 带宽 @10fps | ~0.16 MB/s |
| `wstream` CPU | ~0.1% |
| `sway` CPU | ~0.2% |
| 首帧延迟 | < 200 ms |

对比：用 PNG 传同样分辨率是 **4.8 MB/帧**（PNG 的 store 模式不压缩），
10fps 就是 **48 MB/s**——本地回环也扛不住。换 libjpeg-turbo 后降到 80 KB/帧。

---

## 隔离性说明

### 隔离了什么

| 层面 | 状态 | 说明 |
|---|---|---|
| 键盘鼠标输入 | ✅ **完全隔离** | 注入只作用于沙箱合成器，用户桌面收不到任何事件 |
| 焦点 / 窗口 | ✅ 不碰 | 不创建任何窗口，只推 MJPEG 流 |
| Wayland socket | ✅ 独立 | 沙箱用自己的 `XDG_RUNTIME_DIR`，与用户会话是不同 inode |

实测验证：通过 wstream 注入「移动光标到 (0,0) + 左键点击」，用户真实桌面的光标
位置**完全不变**。

### 没有隔离什么

| 层面 | 状态 | 说明 |
|---|---|---|
| 文件系统 | ❌ 共享 | 沙箱程序与用户共用同一个家目录，**不是容器** |
| 进程可见性 | ❌ 可见 | 沙箱程序能看到用户全部进程 |
| 网络 | ❌ 共享 | 沙箱程序走用户的网络栈 |

> **这是「输入隔离」，不是「安全沙箱」。**
> 如果需要文件级隔离，得用 bwrap / podman 再加 Wayland socket 转发。

### 关键实现细节

启动沙箱里的 GUI 程序时，**必须 `env -u DISPLAY`**：

```bash
setsid env -u DISPLAY XDG_RUNTIME_DIR=$ROOT/run WAYLAND_DISPLAY=$WL ...
```

否则 GTK/Qt 程序会继承用户的 `DISPLAY=:0`，连到**真实桌面的 Xwayland**，
窗口直接出现在用户桌面上。（真实踩过：firefox 泄漏到用户 workspace。）

---

## 踩过的坑

按发现顺序，每条都是真机实测。

### 1. `capture_output` 的第三个参数不能传 NULL

必须传一个真实的 `wl_output` 对象，否则协议报 `null value passed for arg 2`。

### 2. 监听器结构体里未实现的事件要给空函数

写 `NULL` 会 abort：

```
listener function for opcode N is NULL
```

### 3. `accept4(SOCK_NONBLOCK)` 出来的 socket 是非阻塞的

直接 `recv` 会返回 `EAGAIN`，被当成失败 → 客户端收到 `curl: (52) Empty reply from server`。
必须切回阻塞模式 + `SO_RCVTIMEO` / `SO_SNDTIMEO`。

### 4. buffer 格式要用合成器协商出来的那个

`sway`/`labwc` 给的是 `0x1`（XRGB8888），不是 `0x0`（ARGB8888）。
传错报 `invalid buffer format`。

### 5. PNG 太慢

见[性能实测](#性能实测)。PNG store 模式每帧 4.8 MB，换 libjpeg-turbo 后 80 KB。

### 6. 画面不动时抓不到第二帧

合成器在没有新输入、画面不变时不会给可读事件。裸 `poll()` 会让抓帧永远等第一帧。
解法：等待循环里配 `wl_display_dispatch` / `dispatch_pending`。

### 7. 两个线程 dispatch 同一个 `wl_display` → 断言崩溃

```
wl_display_dispatch_queue: Assertion 'ret == -1 || ret > 0' failed
```

解法：注入放到网络线程只 flush，roundtrip 交给主线程（`g_input_dirty` 标志）。

### 8. 键盘键码**不要** +8

`zwp_virtual_keyboard_v1.key()` 的 code 参数是 **evdev 扫描码**，
合成器自己 +8 转 xkb keycode。加两次等于 +16：`abc` → `l]c`，`b` → `m`。

### 9. WebSocket 帧前后端布局必须逐字节一致

前端曾发 5 字节帧（`type|flag|rsv|A|B`），服务端按 8 字节读
（`type|rsv|rsv|A16|B16`）。`5 < 8` 直接命中 `if (n < 8) return;`，
**所有按键事件被静默丢弃**——表现为「鼠标能动、打字毫无反应」。

### 10. 坐标必须按无符号读

`handle_msg` 曾统一按 `int16_t` 读 A/B。鼠标坐标是无符号 0..65535，
屏幕右半边（x > 32767）被解成负数，转 uint32 后变成天文数字，合成器直接忽略
→ **右半边完全点不动**。解法：按 type 分开解释。

### 11. 虚拟指针对象必须真的创建

`inject_move` / `inject_button` 第一行都是 `if (!cap.ptr) return;`。
如果 `input_init` 里忘了 `create_virtual_pointer`，`cap.ptr` 永远是 `NULL`，
所有鼠标事件静默丢弃——**而日志还骗人地说「虚拟指针已就绪」**。

### 12. `motion_absolute` 的 x/y 是**像素**，不是 0..65535

协议原文是 `Value of x can range from 0 to x_extent`。按归一化传，
坐标全被钳到边缘 → **光标只会在角落、点哪都不对**。

### 13. 不要依赖「光标初始位置」这种隐式状态

早期用相对位移累积，`g_cur` 初值假定 (0,0)。但合成器重启后光标在 (99,99)，
于是**每次点击都偏移 99px，越点越偏**。现在改用 `motion_absolute`，不依赖任何状态。

### 14. 按钮码要 `BTN_LEFT`，不是 1/2/3

`zwlr_virtual_pointer_v1.button` 要的是 Linux input-event-codes 的 `BTN_*`
（0x110 起）。直接发 1/2/3，客户端收到 `button=104/96/197` 这种垃圾值，点击无效。

### 15. XWayland **不是**「抓不到也注不进去」

1.0.0 的文档写错了。实测：X11 窗口能被抓进画面、能被点击、能收到 ASCII 键盘。
只有中文受限（keycode 9 冲突）。

### 16. `motion_absolute` 不是所有合成器都实现

labwc 只接了 `wlr_seat_pointer_notify_motion`（相对位移），
`motion_absolute` 请求发得完全正确（`WAYLAND_DEBUG` 可验证），但**静默丢弃**。

诊断方法：

```bash
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "motion|button"
```

如果能看到请求发出但光标不动 → 合成器不支持。**换 sway。**

### 17. 不要每键都推 keymap —— 会把修饰位清零

合成器收到新 keymap 会重建键盘状态、**修饰位归零**。每键都推的后果：
`Ctrl+V` 退化成按一个 `v`，`Shift+v` 出小写。1.1.0 为「盖掉别人的残缺
keymap」加了每键推送，把组合键全弄坏了 —— 而且 24 项测试一条都没测大写字母。

### 18. 修饰键全释放时也必须发 modifiers

`if (g_mods)` 让 `g_mods` 归零时**不发** modifiers，合成器一直以为 Shift
按着 → `Upper` 打成 `UPPER`。要比较上次发的值，变了就发（包括发 0）。

### 19. 终端里粘贴是 `Ctrl+Shift+V`

`Ctrl+V` 在终端里是「引号转义」。GUI 程序才是 `Ctrl+V`。

### 20. `/windows` 不能用 foreign-toplevel 协议

它只给 title/app_id，**不给坐标**。要读 sway IPC `GET_TREE`。
解析别用 strstr：JSON 里 `rect` 在 `app_id` 之前且节点嵌套，会错位。

### 21. `start.sh` 里的 socket 名不能写死

sway 的 socket 不一定是 `wayland-0`（多实例时会是 `wayland-1`）。
必须动态探测：

```bash
WL=$(basename "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)")
```

### 22. sway 没有 labwc 的 `-S` 启动钩子

labwc 支持 `labwc -S script.sh`，sway 只能用配置文件里的 `exec`：

```
exec /path/to/desktop.sh
```

### 23. `pkill -f xxx` 会杀掉自己的 shell

因为 bash 的命令行里也包含那个字符串。用 `pgrep -x`（精确匹配进程名）更安全。

---

## 多实例（多个 agent 各一套桌面）

每个实例 = 独立 sway + 独立 wstream + 独立端口，**完全隔离**：

```bash
WSTREAM_ROOT=/tmp/ws-a WSTREAM_PORT=8801 ./scripts/start.sh
WSTREAM_ROOT=/tmp/ws-b WSTREAM_PORT=8802 ./scripts/start.sh
```

| 资源 | 隔离方式 |
|---|---|
| 合成器 | 各自 `$ROOT/run/wayland-N` |
| 窗口 / 输入 / 剪贴板 | 独立 sway 会话，物理隔离 |
| HTTP / WS 端口 | `WSTREAM_PORT` 各自不同 |
| 运行时目录 | `WSTREAM_ROOT` 各自一套 |

### 查看多个桌面

**① 各开一个标签页**（推荐，可交互）
```
http://127.0.0.1:8801
http://127.0.0.1:8802
```

**② 列出所有在跑的实例**
```bash
./scripts/list.sh          # 表格：端口 / 状态 / 运行时目录 / URL
./scripts/list.sh --url    # 只输出 URL，方便脚本消费
```

**③ 总览页：一屏看全部画面**
```bash
./scripts/dashboard.sh     # 生成 /tmp/wstream-dashboard.html
```
所有实例的实时 MJPEG 并排成网格。点每张卡片的「打开 ↗」进入可交互页面。

> 总览页只嵌画面、不做交互：各实例的 `Origin` 校验只信任自己端口的页面，
> 跨端口建不了 WebSocket。但 `<img>` 加载 MJPEG 不带 `Origin`，
> `Host` 也是回环地址，能通过校验 —— 所以看画面没问题，操作要点进原生页面。

### 单独停止某个实例
```bash
WSTREAM_ROOT=/tmp/ws-a ./scripts/start.sh stop
```

> ⚠️ 必须带 `WSTREAM_ROOT`。不带的话会按默认 `/tmp/wstream` 处理，
> 停的是另一个实例（或什么都没停）。

### 资源成本

单实例实测（去掉演示终端）：wstream 10 MB + sway 44 MB ≈ **55 MB**。
十个 agent 约 550 MB。

---

## 已知限制

| 限制 | 说明 | 可能的解法 |
|---|---|---|
| **无音频** | 只传画面 | 加 PipeWire 抓音频 |
| **同一时刻只能一人操作** | 有输入所有权机制，其余人只读（剪贴板不受限） | 设计如此，见「输入所有权」 |
| **仅 Linux / wlroots 系** | 依赖 `wlr-screencopy` 和 `zwlr_virtual_pointer` | Hyprland 的 headless 后端受限于 aquamarine 缺软件渲染 |

---

## 安全边界

**只监听 `127.0.0.1` 并不等于安全。** 能发起连接的不只是你自己，还有你浏览器里
打开的任何网页。所以从 1.0.1 起，握手会校验来源：

| 检查 | 规则 | 挡的是什么 |
|---|---|---|
| `Host` | 必须是 `127.0.0.1` / `localhost` / `[::1]` | DNS rebinding（`evil.com` 解析到 127.0.0.1） |
| `Origin` | 带该头时必须等于 `http(s)://127.0.0.1:<port>` 等本页来源 | 恶意网页的 JS 直连 `/ws` 注入键鼠 |
| 无 `Origin` | 放行 | curl / 脚本本来就是本机进程，能直连已有等价权限 |

不合法一律回 `403`。浏览器发 WebSocket 一定带 `Origin`，所以正常使用零影响。

> 这个校验是补上的，不是一开始就有的：1.0.0 实测可被 `Origin: http://evil.example`
> 的页面连进来，在沙箱里敲命令。细节见 `CHANGELOG.md` 的 1.0.1。

**仍未做的**：无鉴权 token、无 TLS。如果哪天要开放到 `127.0.0.1` 之外，
这两项是前置条件，不是可选项。

---

## 开发

### 代码结构

```
wstream.c
├── JPEG 编码            (jpeg_encode)
├── wl_shm 缓冲          (create_shmbuf)
├── Wayland 状态         (struct capture, on_global)
├── screencopy 回调      (ev_buffer / ev_ready / ev_failed)
├── 输入注入             (inject_move / inject_button / inject_scroll / inject_key)
│   └── push_keymap_snapshot
├── PPM 快照             (save_ppm，用于 -s 模式)
├── HTTP / WS 服务       (http_thread, ws_parse_one, handle_msg)
├── 内嵌前端页面          (INDEX_HTML)
└── main                 (参数解析、抓帧主循环)
```

### 给 agent 用的 Python 客户端

`clients/wstream_client.py`（零依赖，只用标准库）：

```python
import sys; sys.path.insert(0, "clients")
from wstream_client import WStream

with WStream() as ws:
    ws.take()                            # 不调这个，所有输入都会被丢弃
    for w in ws.windows():
        print(w["app_id"], (w["cx"], w["cy"]))
    ws.click_window("AgentTop")
    ws.type_text("echo hello"); ws.press("enter")
    ws.type_any("中文内容"); ws.press("enter")   # 自动走剪贴板
    ws.snapshot("/tmp/shot.jpg")
```

详见 [clients/README.md](clients/README.md)。

### 调试技巧

```bash
# 1. 单帧快照，不需要浏览器
./wstream -s /tmp/shot.ppm && ffmpeg -i /tmp/shot.ppm /tmp/shot.png

# 2. 抓 Wayland 请求（验证注入是否真的发出去了）
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "motion|button|keymap|key"

# 3. 抓沙箱屏幕（会渲染光标）
XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=wayland-1 grim -c /tmp/s.png

# 4. 落盘 keymap 检查语法
./wstream -v -s /tmp/x.ppm && cat /tmp/wstream-keymap.txt
```

### 协议代码生成

`build.sh` 会用 `wayland-scanner` 从 XML 生成 C 代码：

| 协议 | XML 来源 |
|---|---|
| `wlr-screencopy-unstable-v1` | 系统包 `wlr-protocols` |
| `wlr-virtual-pointer-unstable-v1` | 系统包 `wlr-protocols` |
| `virtual-keyboard-unstable-v1` | **仓库内自带**（发行版都不打这个包） |

---

## 给 AI agent 用（skill）

仓库自带一份 skill，装上之后 AI agent 就知道怎么用 wstream 操作 GUI。

```bash
mkdir -p ~/.agents/skills/wstream
cp skills/wstream/SKILL.md ~/.agents/skills/wstream/
```

内容包含：
- 什么时候该用 / 不该用
- **可直接复制的 Python 代码**（WebSocket 连接、鼠标移动点击、键盘打字、组合键）
- 完整的 evdev 扫描码映射表
- 怎么在沙箱里启动 GUI 程序（含必须的环境变量）
- 隔离性验证方法
- 症状速查表
- 四条红线

源码：`skills/wstream/SKILL.md`

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [README.md](README.md) | 本文件：项目介绍、用法、性能、踩坑记录 |
| [docs/PROTOCOLS.md](docs/PROTOCOLS.md) | Wayland 协议详解：抓帧、指针注入、键盘 keymap、WS 帧格式 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 线程模型、主循环、数据结构、设计决策 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | 按症状索引的排障手册 |
| [docs/HANDOFF.md](docs/HANDOFF.md) | **接手必读**：现状、五个不能再踩的坑、下一步建议 |
| [CHANGELOG.md](CHANGELOG.md) | 版本历史 + 已修 bug 清单（1.0.0 开发期 14 个 + 1.0.1 安全修复） |
| [skills/wstream/SKILL.md](skills/wstream/SKILL.md) | 给 AI agent 的操作手册 |

---

## 许可证

MIT
