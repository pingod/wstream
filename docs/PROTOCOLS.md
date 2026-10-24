# 协议与协议设计

本文详细说明 wstream 用到的每个 Wayland 协议，以及为什么这样设计。

## 一、用到的协议

| 协议 | 版本 | 方向 | 用途 |
|---|---|---|---|
| `zwlr_screencopy_manager_v1` | 3 | 抓帧 | 从合成器抓取屏幕内容 |
| `zwlr_virtual_pointer_manager_v1` | 2 | 注入 | 模拟鼠标移动/点击/滚轮 |
| `zwp_virtual_keyboard_manager_v1` | 1 | 注入 | 模拟键盘输入 |
| `wl_shm` | 1 | 缓冲 | 共享内存传像素 |
| `wl_output` | 4 | 查询 | 获取输出尺寸/格式 |
| `wl_seat` | 7 | 绑定 | 虚拟设备挂载点 |

### 都是 wlroots 系协议

`zwlr_*` 前缀表示 wlroots 私有协议，`zwp_*` 表示 wayland-protocols 标准协议。
支持情况：

| 合成器 | screencopy | virtual_pointer | virtual_keyboard |
|---|---|---|---|
| sway | ✅ | ✅（含 absolute） | ✅ |
| labwc | ✅ | ⚠️ **只支持相对位移** | ✅ |
| Hyprland | ⚠️ 部分 | ❌ | ❌ |
| GNOME / KDE | ❌（用 portal） | ❌ | ❌ |

> 这是 wstream 只支持 wlroots 系合成器的根本原因。

---

## 二、抓帧：`zwlr_screencopy_manager_v1`

### 基本流程

```
1. wl_registry 绑定 zwlr_screencopy_manager_v1（version ≤ 3）
2. 绑定一个真实的 wl_output（不能传 NULL！）
3. capture_output(overlay_cursor, output)
       ↓
4. 等 buffer 事件 → 拿到 format / width / height / stride
       ↓
5. 按这个 format 创建 wl_shm buffer（不能自己猜！）
       ↓
6. copy(buffer) → 等 ready 事件
       ↓
7. 处理像素 / 编码 JPEG
       ↓
8. frame.destroy()，重复 3
```

### 三个必须注意的点

**① `capture_output` 的 output 参数不能是 NULL**

```c
/* ❌ 报错：null value passed for arg 2 */
zwlr_screencopy_manager_v1_capture_output(mgr, 1, NULL);

/* ✅ 必须绑定真实的 wl_output */
zwlr_screencopy_manager_v1_capture_output(mgr, 1, cap.output);
```

**② buffer 格式必须用合成器协商出来的**

```c
static void ev_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    /* format 是合成器告诉你它要什么，不是你告诉它 */
    c->fb_format = format;   /* sway/labwc 给 0x1 = XRGB8888 */
}
```

自己写死 `WL_SHM_FORMAT_ARGB8888`（0x0）会报 `invalid buffer format`。

**③ 监听器里未实现的事件要给空函数，不能留 NULL**

```c
/* ❌ abort: listener function for opcode N is NULL */
static const struct zwlr_screencopy_frame_v1_listener l = { ev_buffer, ev_ready, NULL };

/* ✅ 三个都要有 */
static const struct zwlr_screencopy_frame_v1_listener l = { ev_buffer, ev_ready, ev_failed };
```

### 为什么不能裸 poll 等第二帧

合成器在**画面没有变化**时不会给 socket 可读事件。如果这样写：

```c
while (!frame_ready) poll(fd, -1);   /* ❌ 画面不动就永远卡住 */
```

第一帧之后永远等不到第二帧。正确做法是让 libwayland 处理事件：

```c
while (!frame_ready) {
    poll(fd, 100);
    if (revents & POLLIN) wl_display_dispatch(display);
    else                  wl_display_dispatch_pending(display);
}
```

---

## 三、鼠标注入：`zwlr_virtual_pointer_manager_v1`

### 创建

```c
/* seat 参数是"建议"，允许为 NULL */
cap.ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(cap.ptr_mgr, cap.seat);
if (!cap.ptr) {
    /* 创建失败 → 后面所有 inject_* 都会 return，鼠标完全不可用 */
}
```

> ⚠️ **必须真的创建。** `inject_move()` 第一行是 `if (!cap.ptr) return;`，
> 如果忘了创建，所有鼠标事件会被静默丢弃，而日志可能还在说"已就绪"。

### 绝对定位 vs 相对位移

协议提供两个请求：

```c
/* 相对位移 */
zwlr_virtual_pointer_v1_motion(ptr, time, wl_fixed_from_int(dx), wl_fixed_from_int(dy));

/* 绝对定位 */
zwlr_virtual_pointer_v1_motion_absolute(ptr, time, x, y, x_extent, y_extent);
```

**关键：`motion_absolute` 不是所有合成器都实现。**

| 合成器 | motion_absolute | 现象 |
|---|---|---|
| sway | ✅ | 正常 |
| labwc | ❌ **静默丢弃** | 请求发出去了，光标纹丝不动 |

诊断方法：

```bash
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "motion|button"
```

如果看到 `motion_absolute(...)` 正常发送但光标不动 → 合成器不支持。

**当前实现**：用相对位移累积模拟绝对定位（兼容性最好）：

```c
static int g_cur_x, g_cur_y;   /* 我们以为光标在哪 */

static void inject_move(int vx, int vy, int W, int H) {
    int tx = (int)((long long)vx * W / 65535);
    int ty = (int)((long long)vy * H / 65535);
    int dx = tx - g_cur_x, dy = ty - g_cur_y;
    if (dx == 0 && dy == 0) return;
    zwlr_virtual_pointer_v1_motion(cap.ptr, now_ms(),
        wl_fixed_from_int(dx), wl_fixed_from_int(dy));
    zwlr_virtual_pointer_v1_frame(cap.ptr);
    wl_display_flush(cap.display);
    g_cur_x = tx; g_cur_y = ty;
}
```

### 坐标范围

`motion_absolute` 的 `x/y` 是 **uint32**，`x_extent/y_extent` 是输出尺寸：

```c
zwlr_virtual_pointer_v1_motion_absolute(ptr, time,
    32767, 36408,    /* x, y：0..65535 归一化 */
    1280, 720);      /* x_extent, y_extent：实际像素尺寸 */
```

---

## 四、键盘注入：`zwp_virtual_keyboard_manager_v1`

### keymap 必须先推

```c
/* 1. 生成 keymap 文本 */
static const char KM[] =
    "xkb_keymap {\n"
    "xkb_keycodes \"x\" { include \"evdev+aliases(qwerty)\" };\n"
    "xkb_types \"x\" { include \"complete\" };\n"
    "xkb_compatibility \"x\" { include \"complete\" };\n"
    "xkb_symbols \"x\" { include \"pc+us+inet(evdev)\" };\n"
    "};\n";

/* 2. 写入 memfd */
int fd = memfd_create("keymap", MFD_CLOEXEC);
ftruncate(fd, len);
void *m = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
memcpy(m, KM, len);
munmap(m, len);

/* 3. 传给合成器（传完即可 close，合成器已 mmap） */
zwp_virtual_keyboard_v1_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
close(fd);

/* 4. 必须等合成器处理完，否则后面的 key 事件会被丢 */
wl_display_roundtrip(display);
```

**为什么用完整 `include` 形式而不是 wtype 那种单键位小表？**

wtype 的做法是为每个键生成一张只含该键位的 keymap。这在简单场景能用，
但会**把其余键位变成 NoSymbol**——打一个键之后整个键盘就"失灵"了。
完整布局由 libxkbcommon 自己展开，稳定可靠。

### 键码：**不要 +8**

```c
/* ✅ 正确 */
zwp_virtual_keyboard_v1_key(kbd, time, evdev_code, state);

/* ❌ 错误：会 +16，敲 a 出来 l */
zwp_virtual_keyboard_v1_key(kbd, time, evdev_code + 8, state);
```

协议里 `code` 参数是 **evdev 扫描码**（`a` = 30），合成器内部自己 +8 得到
xkb keycode（38）。如果你也 +8，就变成 +16：

| 按键 | evdev | 正确 xkb | 错误（+8 两次） | 实际输出 |
|---|---|---|---|---|
| a | 30 | 38 | 46 | `l` |
| b | 48 | 56 | 64 | （无） |
| c | 46 | 54 | 62 | `]` |

实测 `abc` → `l]c`，与上表完全吻合。

### 修饰键

```c
/* mods: 低 8 位 = 当前按住，高 8 位 = 有效 */
uint32_t mods = 0;
if (code == 29 || code == 97)   mods = 4;   /* Ctrl  → bit2 */
else if (code == 42 || code == 54)  mods = 1;   /* Shift → bit0 */
else if (code == 56 || code == 100) mods = 8;   /* Alt   → bit3 */
else if (code == 125 || code == 126) mods = 64; /* Super → bit6 */

zwp_virtual_keyboard_v1_modifiers(kbd, mods, 0, 0, 0);
```

组合键顺序必须是 **modifiers → key**，否则应用收不到组合。

---

## 五、HTTP / WebSocket

### 为什么不用现成库

- 只需要两个端点，自己实现 ~200 行
- 避免引入 libwebsockets / civetweb 依赖
- 完全控制行为（超时、缓冲、并发模型）

### WebSocket 握手

```c
/* 1. 从请求头取 Sec-WebSocket-Key */
/* 2. 拼上固定 GUID */
"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* 3. SHA1 + Base64 */
SHA1(key + GUID) → Base64 → Sec-WebSocket-Accept

/* 4. 返回 101 */
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <base64>
```

### 握手前的来源校验（1.0.1 起）

标准握手**不校验来源**，而这是真实漏洞：任意网页的 JS 都能连 `ws://127.0.0.1:8765/ws`，
往沙箱里点坐标敲键。`bind(127.0.0.1)` 挡不住——发起连接的正是用户自己的浏览器。

所以 `ws_authorize()` 在回 101 之前先查两个头：

```c
Host   // 必须 127.0.0.1 / localhost / [::1]      → 挡 DNS rebinding
Origin // 带了就必须是本页来源；不带则放行（curl/脚本）
```

不合法回 `403`。注意 `hdr_get()` 是**逐行**解析的，并且要跳过名字后的冒号——
用 `strstr` 在整个请求里找会被 `X-Origin:` 这类头误命中，而漏跳冒号会让值变成 `: ...`。

### 帧格式

客户端 → 服务端的帧**必须加掩码**（RFC 6455 规定）：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127  |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
```

### 应用层协议

见 [README 的通信协议章节](../README.md#通信协议)。

**核心要点：8 字节定长，小端序，A/B 的符号性随 type 变化。**

---

## 六、并发模型

### 约束

Wayland 的 `wl_display` **只能由一个线程 dispatch**。两个线程同时 dispatch 会崩溃：

```
wl_display_dispatch_queue: Assertion 'ret == -1 || ret > 0' failed
```

### 方案

```
主线程                         网络线程
───────                        ────────
wl_display_dispatch()           poll() 所有连接
  ↓                              ↓
screencopy 抓帧                 读 HTTP / WS
  ↓                              ↓
JPEG 编码                       handle_msg()
  ↓                              ↓
push_frame() ──► clients[] ◄── inject_*()
                    ↑              ↓
                g_cl_mu        wl_display_flush()
                    ↑              ↓
                    └──── g_input_dirty = 1
                                   ↓
                    主线程看到标志 → wl_display_roundtrip()
```

**关键**：网络线程只 `flush()` + 置标志，roundtrip 由主线程做。

### `clients[]` 的句柄：用 id，不要用下标或 fd

`http_thread` 每轮先建 `pollfd[]`，`poll()` 回来再处理。**这中间主线程可能改数组**：
`push_frame()` 失败会调 `drop_client()`，而它把末尾元素搬进被删的槽并 `nclients--`。

于是「poll 前记下的下标」在 poll 回来后可能指向另一个客户端，甚至 `>= nclients` 越界读。
拿 fd 当键也不行——fd 会被内核复用。

所以 `struct client` 有单调递增的 `id`：每轮把 id 存进与 `pfd` 平行的数组，
回来后 `client_by_id()` 反查，查不到（已被回收）就跳过。

```c
/* 必须在持 g_cl_mu 时调用 */
static int client_by_id(unsigned long id) { ... }
```

三条纪律：

1. `client_add` / `drop_client` / 遍历 `clients[]` 一律持 `g_cl_mu`
2. 主线程判断「有没有人在看」用 `has_viewers()`，**不要裸读 `nclients`**（数据竞争）
3. 退出时先置 `g_net_stop` 让网络线程退循环，再持锁关 fd

---

## 七、协议扩展方向

| 需求 | 需要的协议 |
|---|---|
| 剪贴板同步 | `wl_data_device_manager` + `wl_data_source` |
| 音频 | PipeWire（不是 Wayland 协议） |
| 多显示器 | 多个 `wl_output` + 分屏抓取 |
| X11 窗口支持 | `xcb` + XTEST 扩展 |
| 窗口级抓取（非全屏） | `ext-image-copy-capture-v1`（新协议） |
