# 架构说明

## 一、整体结构

```
┌──────────────────────────────────────────────────────────────────────┐
│ 宿主：用户的真实桌面（Hyprland）                                      │
│                                                                       │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │ 浏览器（用户自己开的窗口，用户完全控制）                      │   │
│   │   <img src="http://127.0.0.1:8765/stream.mjpg">             │   │
│   │   WebSocket → ws://127.0.0.1:8765/ws                        │   │
│   └─────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
              │ HTTP (MJPEG 下行)      ▲ WebSocket (输入上行)
              ▼                        │
┌──────────────────────────────────────────────────────────────────────┐
│ wstream 进程                                                          │
│                                                                       │
│  ┌────────────────────┐          ┌──────────────────────────────┐   │
│  │ 主线程              │          │ http_thread (pthread)         │   │
│  │                    │          │                               │   │
│  │  wl_display_dispatch│          │  poll() 所有连接              │   │
│  │        ↓           │          │        ↓                      │   │
│  │  screencopy 抓帧   │          │  accept / HTTP / WS 解析      │   │
│  │        ↓           │          │        ↓                      │   │
│  │  JPEG 编码         │          │  handle_msg()                 │   │
│  │        ↓           │          │        ↓                      │   │
│  │  push_frame() ─────┼──写──►   │  inject_move/button/key()     │   │
│  │        ↑           │ clients[]│        ↓                      │   │
│  │  g_input_dirty ◄───┼──────────┤  wl_display_flush()           │   │
│  │        ↓           │ g_cl_mu  │                               │   │
│  │  wl_display_roundtrip         │                               │   │
│  └────────────────────┘          └──────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
              │ wl_display (独立 socket)      ▲
              ▼                               │
┌──────────────────────────────────────────────────────────────────────┐
│ 沙箱：sway --unsupported-gpu                                          │
│   WLR_BACKENDS=headless  WLR_RENDERER=pixman                         │
│   XDG_RUNTIME_DIR=/tmp/wstream/run  →  socket: wayland-1             │
│                                                                       │
│   ┌──────────┐  ┌──────────┐                                        │
│   │ 窗口 1    │  │ 窗口 2    │   ← 沙箱里的程序，用户桌面看不到      │
│   └──────────┘  └──────────┘                                        │
└──────────────────────────────────────────────────────────────────────┘
```

## 二、关键设计决策

### 决策 1：为什么用无头合成器而不是「截真实桌面」

| 方案 | 问题 |
|---|---|
| 截用户真实桌面 | agent 的操作会**直接影响**用户正在做的事 |
| 开一个窗口在用户桌面 | 会抢焦点（真实踩过：用户键盘鼠标完全失控） |
| **无头合成器（本方案）** | 完全独立，用户无感 |

### 决策 2：为什么用 MJPEG 而不是 H.264 / VP9

| 方案 | 优点 | 缺点 |
|---|---|---|
| **MJPEG** | 浏览器 `<img>` 原生支持，零 JS；单帧独立，无花屏 | 带宽比 H.264 高 3~5 倍 |
| H.264 (WebRTC) | 带宽低 | 需要信令服务器 + ICE + JS 解码栈 |
| H.264 (MSE) | 带宽低 | 需要 fMP4 封装 + JS，延迟高 |

在**本地回环**场景下，0.16 MB/s 的带宽完全不是问题，而 MJPEG 的实现复杂度
低了一个数量级。这是有意的权衡。

### 决策 3：为什么用 WebSocket 而不是 HTTP POST

输入事件频率高（鼠标移动可达 100+ Hz）。HTTP 短连接每次都要 TCP 握手 + 头解析，
开销不可接受。WebSocket 长连接 + 8 字节定长帧，开销接近裸 TCP。

### 决策 4：为什么不用现成库（libwebsockets 等）

- 只需要 2 个端点（`/` 和 `/ws`），自己实现约 200 行
- 避免一个额外依赖（编译门槛越低越好）
- 完全控制超时和并发行为（调试过很多奇怪的连接问题）

### 决策 5：为什么沙箱用 sway 而不是 labwc

> ⚠️ 历史说明：早期把「`motion_absolute` 支持」当作选 sway 的决定性理由。
> **现在代码已经不用 `motion_absolute` 了**——`inject_move()` 改成相对位移累积
> （见 `docs/PROTOCOLS.md` 第三节），因为 labwc 会静默丢弃 absolute 请求。
> 所以下表里 absolute 那一行只解释历史，不再代表当前依赖。

| | labwc（实测） | sway（实测） |
|---|---|---|
| `motion_absolute` | ❌ 静默丢弃 | ✅ |
| 相对位移 `motion` | ✅ | ✅（当前实现走这条） |
| 点击切焦点 | ⚠️ 当时测下来不行 | ✅ |
| 光标渲染 | ❌ 截图不可见 | ✅ |
| 窗口布局 | 需手动 | ✅ 自动平铺 |

**结论**：现在真正的依赖是 `motion`（相对位移）+ 点击聚焦 + 光标渲染，不是 absolute。
如果有人想换更轻量的合成器，值得重新实测 labwc——但别假设能直接用。

## 三、主循环详解

```c
for (;;) {
    /* ---------- 1. 发起抓帧 ---------- */
    struct zwlr_screencopy_frame_v1 *fr =
        zwlr_screencopy_manager_v1_capture_output(cap.mgr, 1, cap.output);
    zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, &cap);

    cap.frame_ready = 0;
    cap.frame_failed = 0;

    /* ---------- 2. 等这一帧 ---------- */
    wl_display_flush(cap.display);
    while (g_run && !cap.frame_ready) {
        struct pollfd p = { .fd = wl_display_get_fd(cap.display), .events = POLLIN };
        int pr = poll(&p, 1, 100);

        /* 2a. 网络线程刚注入了输入 → 主线程统一 roundtrip */
        if (g_input_dirty) {
            g_input_dirty = 0;
            wl_display_roundtrip(cap.display);
            continue;
        }

        /* 2b. 有事件 → dispatch */
        if (p.revents & POLLIN) {
            if (wl_display_dispatch(cap.display) < 0) { g_run = 0; break; }
        } else {
            /* 2c. 超时也要 dispatch_pending，否则事件永远不被处理 */
            wl_display_dispatch_pending(cap.display);
            wl_display_flush(cap.display);
        }
    }

    zwlr_screencopy_frame_v1_destroy(fr);
    if (!g_run) break;

    /* ---------- 3. 编码 + 推送 ---------- */
    if (!cap.frame_failed && nclients > 0) {
        unsigned long jlen = jpeg_encode(&enc, fb[cur].addr, W, H);
        if (jlen > 0) push_frame(enc.buf, jlen);
    }

    /* ---------- 4. 帧率控制 ---------- */
    throttle_to_target_fps();
}
```

### 为什么 `2c` 分支不能省

`wl_display_dispatch_pending()` 处理**已经到达本地缓冲区**但还没被处理的事件。
如果 poll 超时后什么都不做，那些事件会一直堆着，抓帧就永远停在第一帧。

### 为什么 `2a` 要放在 poll 之后

因为 `g_input_dirty` 由网络线程设置。放在 poll 之后能保证：
1. 网络线程的 flush 已经执行
2. 我们做 roundtrip 时不会和网络线程冲突

## 四、数据结构

### `struct capture`（Wayland 状态）

```c
struct capture {
    struct wl_display *display;   /* 与合成器的连接 */
    struct wl_registry *registry;
    struct wl_shm     *shm;       /* 共享内存 */
    struct wl_output  *output;    /* 抓哪个输出 */
    struct wl_seat    *seat;      /* 虚拟设备挂载点 */

    struct zwlr_screencopy_manager_v1      *mgr;
    struct zwlr_virtual_pointer_manager_v1 *ptr_mgr;
    struct zwp_virtual_keyboard_manager_v1 *kbd_mgr;

    struct zwlr_virtual_pointer_v1 *ptr;   /* 虚拟鼠标（NULL = 不可用） */
    struct zwp_virtual_keyboard_v1 *kbd;   /* 虚拟键盘（NULL = 不可用） */

    int frame_ready, frame_failed;
    int probed;        /* 是否做过 shm 格式 roundtrip */
    int fb_w, fb_h;    /* 输出尺寸 */
    uint32_t fb_format;/* 合成器要的像素格式 */
};
```

### `struct client`（连接状态）

```c
struct client {
    int fd;               /* -1 = 空槽 / 已死 */
    int is_stream;        /* 是否 MJPEG 订阅者 */
    int is_ws;            /* 是否已升级为 WebSocket */
    unsigned long id;     /* 槽位会复用，id 单调递增永不重用 */
    uint8_t in[16384];    /* 读缓冲 */
    size_t in_len;
};
```

### 全局锁与句柄

```c
static pthread_mutex_t g_cl_mu = PTHREAD_MUTEX_INITIALIZER;
```

保护 `clients[]` / `nclients`。主线程写（`push_frame`、`has_viewers`），
网络线程读写（accept / close / 反查）。

**为什么用 `id` 而不是下标或 fd**：`http_thread` 建好 `pollfd[]` 到 `poll()` 返回之间，
主线程的 `drop_client()` 可能搬移数组元素。下标会指错人甚至越界；fd 会被内核复用。
id 单调递增，配合 `client_by_id()` 反查即可判定「还活着没有」。

| 函数 | 职责 | 锁 |
|---|---|---|
| `client_add` | 分配槽位并返回 id | 必须持锁 |
| `client_by_id` | id → 槽位下标，-1 表示已回收 | 必须持锁 |
| `drop_client` | 关闭并压缩数组 | 必须持锁 |
| `has_viewers` | 主线程问「有人在看吗」 | 内部加锁 |

## 五、JPEG 编码

```c
static unsigned long jpeg_encode(struct encoder *e, const uint8_t *bgrx, int w, int h) {
    unsigned char *out = NULL;
    unsigned long out_len = 0;

    if (tjCompress2(e->tj, bgrx, w, w * 4, h, TJPF_BGRX,
                    &out, &out_len, TJSAMP_420, g_quality, TJFLAG_FASTDCT) == 0) {
        e->buf = out;
        return out_len;
    }
    return 0;
}
```

关键点：

| 参数 | 值 | 原因 |
|---|---|---|
| `TJPF_BGRX` | 合成器给的是 XRGB8888（小端序内存里就是 BGRX） | 与 `wl_shm` 格式对应 |
| `stride` | `w * 4` | 每像素 4 字节 |
| `TJSAMP_420` | 4:2:0 色度抽样 | 体积比 4:4:4 小 ~40%，画质够用 |
| `TJFLAG_FASTDCT` | 快速 DCT | 快 ~30%，画质损失可忽略 |

## 六、前端页面

内嵌在 `wstream.c` 的 `INDEX_HTML` 字符串里，**零外部资源**（无 CDN、无字体、无框架）。

### 关键实现

```javascript
/* 画面：浏览器原生支持 multipart/x-mixed-replace */
<img id="v" src="/stream.mjpg">

/* 输入：8 字节定长二进制帧 */
function send(type, a, b) {
    const bf = new ArrayBuffer(8), dv = new DataView(bf);
    dv.setUint8(0, type);
    dv.setUint8(1, 0);
    dv.setUint16(2, 0, true);
    dv.setInt16(4, a | 0, true);   /* 注意：坐标用 uint16 语义 */
    dv.setInt16(6, b | 0, true);
    ws.send(bf);
}

/* 键盘：DOM keyCode → evdev 扫描码 */
const M = { 65: 30, 66: 48, ... };  /* 'A' → 30 */
```

### 接管 / 释放

- **点击画面** → `active = true`，开始转发鼠标键盘
- **Ctrl+Esc** → `active = false`，停止转发

这个设计让用户能明确控制"什么时候 agent 在操作沙箱"，避免误操作。

## 七、启动脚本流程

```
scripts/start.sh
  │
  ├─ 1. 检查是否已有沙箱（/tmp/wstream/run/wayland-*）
  │     └─ 有 → 复用
  │
  ├─ 2. 没有 → 创建
  │     ├─ mkdir /tmp/wstream/{run,cfg/sway,out}
  │     ├─ 写 desktop.sh（两个不停刷新的终端）
  │     ├─ 写 sway config（含 exec desktop.sh）
  │     ├─ setsid env -u DISPLAY sway --unsupported-gpu
  │     │    └─ WLR_BACKENDS=headless WLR_RENDERER=pixman
  │     ├─ 动态探测 socket 名（wayland-0 / wayland-1 / ...）
  │     └─ wlr-randr 设分辨率
  │
  ├─ 3. 起 wstream
  │     └─ setsid env -u DISPLAY wstream -p 8765
  │
  └─ 4. 打印访问地址
```

### 为什么每步都要 `env -u DISPLAY`

GTK/Qt 程序会**优先连 X11**（如果 `DISPLAY` 存在）。如果继承了用户的 `DISPLAY=:0`，
程序会连到用户真实桌面的 Xwayland，窗口直接出现在用户屏幕上。

真实踩过：firefox 泄漏到用户的 workspace 11。

### 为什么 socket 名要动态探测

sway 用 `wayland-{N}`，N 是递增的。如果之前有残留实例，新起的可能是 `wayland-1`
而不是 `wayland-0`。写死会导致连错 socket。

### 为什么用 `setsid`

让进程脱离当前 shell 的会话组。否则 shell 退出（或收到 SIGTERM）时，
子进程会被连带杀掉。

## 八、可扩展点

| 想加的功能 | 改哪里 |
|---|---|
| 音频 | 加 PipeWire 线程，前端加 `<audio>` |
| 剪贴板同步 | `inject_*` 旁边加 `wl_data_device` 处理 |
| 多显示器 | `struct capture` 改成数组，抓多个 output |
| 鉴权 | `http_thread` 的 accept 后加 token 校验（已有 Origin/Host 校验） |
| TLS | 加 OpenSSL 包装 socket |
| 窗口级抓取 | 换成 `ext-image-copy-capture-v1` 协议 |

### 已经实现的扩展点（1.1.0）

| 功能 | 实现在哪 | 设计要点 |
|---|---|---|
| Unicode/中文输入 | `inject_text` + `inject_unicode_codepoint` | 网络线程只入队（`g_tq`），主线程取出来注入 —— 因为要 roundtrip |
| 输入所有权 | `g_owner` + `is_owner()` | **服务端强制**，非持有者的输入直接丢；前端隐藏按钮挡不住脚本 |
| 回归测试 | `tests/wstest.py` | 断言「事件到达应用」，不看截图（截图会因终端刷新而不稳） |

### 文本注入为什么必须过队列

`inject_text()` 要给合成器推新 keymap 并 `roundtrip`，而 Wayland 只允许
**一个线程** dispatch。所以：

```
网络线程                       主线程（抓帧循环）
────────                       ──────────────────
handle_msg(type=5)             if (g_input_dirty)
  → text_enqueue()  ──────►      → text_dequeue()
  → g_input_dirty=1              → inject_text()  ← 这里才 roundtrip
```

队列是固定 8 槽、单条上限 4KB 的环形缓冲。满了就**丢弃并记日志**，
绝不能让网络线程阻塞 —— 那会把整个 HTTP 服务拖死。
