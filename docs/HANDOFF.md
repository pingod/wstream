# 交接文档

> **给接手这个项目的 AI agent 或开发者。**
> 读完这份就能知道：现在到哪一步了、哪些坑不能再踩、下一步该做什么。
>
> 最后更新：2026-10-24

---

## 0. 一句话现状

**wstream 1.0.1 已可用。** 核心功能（抓帧 / 推流 / 鼠标 / 键盘）全部在真机验证通过。
1.0.1 修了一个**安全漏洞**：WebSocket 原先不校验 `Origin`，任意网页的 JS 都能连进来
操作沙箱（已实测复现并修复）。

```bash
git log --oneline
# b52df85 修复安全漏洞：WebSocket 不校验来源 + clients[] 并发越界（1.0.1）
# 6f43015 新增交接文档与 AI agent skill
# a2816cb CI 冒烟测试改用快照模式验证抓帧，不再依赖 MJPEG 流
# e8c7316 修复容器/CI 里 sway 无法执行：绕开 cap_sys_nice 文件能力
# de64658 修复 CI 冒烟测试超时：sway 启动等待从 15s 放宽到 60s
# 6ae7806 wstream 1.0.0：把无头 Wayland 桌面推流到浏览器并支持反向操作
```

tag：`v1.0.0`（首个可用版本）、`v1.0.1`（安全修复，**推荐**）。

---

## 1. 这个项目是干什么的

**让 AI agent 操作 GUI 时，完全不干扰用户自己的键盘鼠标和焦点。**

传统做法是在用户桌面上开窗口模拟点击 → 会抢焦点，用户什么都干不了。
wstream 的做法是给 agent 一个**完全独立的无头虚拟桌面**，用户在浏览器里看，
agent 在沙箱里操作，两者物理上不共享输入设备。

### 技术栈

| 层 | 选型 | 为什么 |
|---|---|---|
| 沙箱合成器 | **sway**（`--unsupported-gpu` + `WLR_BACKENDS=headless`） | 只有它完整实现 `motion_absolute` |
| 抓帧 | `zwlr_screencopy_manager_v1` | wlroots 原生，无需 portal |
| 编码 | libjpeg-turbo（MJPEG） | 浏览器 `<img>` 原生支持，零 JS |
| 上行 | 手写 WebSocket + 8 字节定长帧 | 只需 2 个端点，避免额外依赖 |
| 注入 | `zwlr_virtual_pointer_v1` + `zwp_virtual_keyboard_v1` | wlroots 原生 |

---

## 2. 已经做完并验证过的

| 功能 | 验证方式 | 状态 |
|---|---|---|
| 抓帧 + JPEG 编码 | 快照模式出图，尺寸正确 | ✅ |
| MJPEG 推流 | `curl stream.mjpg` 拿到 558KB 连续帧 | ✅ |
| 浏览器页面 | HTTP 200，内嵌零依赖单页 | ✅ |
| 鼠标移动 | 光标出现在目标坐标（`grim -c` 可见） | ✅ |
| 鼠标点击 + 焦点切换 | 点击左右两个窗口，输入分别落到各自窗口 | ✅ |
| 键盘完整 ASCII | `echo ok 2026` / `hello world` / `test.ts 192.168` 逐字符正确 | ✅ |
| 修饰键组合 | Shift 大写、Ctrl+L、Alt+Tab、Super+方向键 | ✅ |
| 沙箱隔离 | 向沙箱注入 500 次移动 + 点击，用户真实光标完全不动 | ✅ |
| CI | GitHub Actions 编译 + 零警告 + 冒烟测试 | ✅ 全绿 |
| 来源校验 | 恶意 `Origin` / `Host` 回 403，正常客户端不受影响 | ✅ |
| 并发安全 | ASan + UBSan 下 5 路 WS 反复连断 + MJPEG 流压测 | ✅ 无报告 |

---

## 3. ⚠️ 七个不能再踩的坑（最重要的一节）

这些是花了大量时间定位出来的，**改动相关代码前务必先读**。
前 5 条会直接让功能失效，坑 6 是安全问题，坑 7 会随机崩溃/越界。

### 坑 1：键盘键码**绝对不能 +8**

```c
zwp_virtual_keyboard_v1_key(kbd, t, evdev_code, state);      /* ✅ */
zwp_virtual_keyboard_v1_key(kbd, t, evdev_code + 8, state);  /* ❌ a 会变成 l */
```

`code` 参数是 **evdev 扫描码**，合成器内部自己 +8 转 xkb keycode。加两次 = +16。

实测对照：`abc` → `l]c`，`b` → `m`。

### 坑 2：WebSocket 帧必须是 **8 字节**，前后端逐字节一致

```
[0]=type  [1]=保留  [2..3]=保留  [4..5]=A  [6..7]=B   （小端）
```

服务端 `handle_msg` 开头是 `if (n < 8) return;`。
曾经前端发 5 字节帧（`type|flag|rsv|A|B`），结果 **所有按键事件被静默丢弃** ——
表现为「鼠标能动、打字毫无反应」，排查了很久。

### 坑 3：坐标必须按**无符号** 16 位读

```c
int ua = (int)((m[4]) | (m[5] << 8));            /* ✅ 0..65535 */
int sa = (int)(int16_t)((m[4]) | (m[5] << 8));   /* ❌ x>32767 变负数 */
```

鼠标坐标是 uint16，滚动步数才是 int16。混用会导致**屏幕右半边完全点不动**。

### 坑 4：虚拟指针对象必须**真的创建**

```c
c->ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(c->ptr_mgr, c->seat);
```

`inject_move` / `inject_button` 第一行都是 `if (!cap.ptr) return;`。
曾经忘了创建 → 所有鼠标事件静默丢弃，而日志还骗人地说「虚拟指针已就绪」。
**日志措辞必须反映真实状态。**

### 坑 5：沙箱里的 GUI 程序必须 `env -u DISPLAY`

```bash
setsid env -u DISPLAY XDG_RUNTIME_DIR=$ROOT/run WAYLAND_DISPLAY=$WL GDK_BACKEND=wayland <程序>
```

否则程序会继承用户的 `DISPLAY=:0`，连到**真实桌面的 Xwayland**，
窗口直接出现在用户屏幕上 —— **这是最严重的事故类型，会让用户失去对电脑的控制。**

真实踩过：firefox 泄漏到用户的 workspace 11。

**每次改动启动逻辑后，都要检查：**

```bash
# 沙箱进程的环境对不对
for p in $(pgrep -x <程序名>); do
  tr '\0' '\n' < /proc/$p/environ | grep -E "^(DISPLAY|XDG_RUNTIME_DIR)="
done

# 用户真实桌面有没有多出窗口
hyprctl clients | grep -c "^Window"
```

---

### 坑 6：只监听 127.0.0.1 **不等于**安全

1.0.0 的 `ws_handshake` 只做 RFC6455 握手，不看 `Origin`、不看 `Host`。实测后果：

```bash
# 任意网页的 JS 都能这样连进来（Origin 随便写）
Origin: http://evil.example  →  HTTP/1.1 101 Switching Protocols
# 然后注入点击 + 打字，沙箱截图里真的出现了敲进去的命令
```

根因是想当然：以为「只 bind 127.0.0.1，外网连不上」就安全了。但**发起连接的
恰恰是用户自己的浏览器**，回环绑定对它毫无阻力。另外 `Host` 不校验还留了
DNS rebinding 的路子。

现在 `ws_authorize()` 的规则（改这里务必同步 `host_is_loopback` / `origin_trusted`）：

| 头 | 规则 | 挡什么 |
|---|---|---|
| `Host` | 必须 `127.0.0.1` / `localhost` / `[::1]` | DNS rebinding |
| `Origin` | 带了就必须是本页来源 | 恶意网页注入键鼠 |
| 无 `Origin` | 放行 | curl / skill 脚本是本机进程，直连本就有等价权限 |

**回归测试**（改完必跑，注意 `/tmp` 在部分环境每次调用会重置，需在同一条命令里跑）：

```bash
./scripts/start.sh 1280x720
# 期望 403
curl -si -H "Host: 127.0.0.1:8765" -H "Origin: http://evil.example"      -H "Upgrade: websocket" -H "Connection: Upgrade"      -H "Sec-WebSocket-Key: YWJjZGVmZ2gxMjM0NTY3OA=="      -H "Sec-WebSocket-Version: 13" http://127.0.0.1:8765/ws | head -1
# 期望 101
curl -si -H "Host: 127.0.0.1:8765" -H "Origin: http://127.0.0.1:8765"      -H "Upgrade: websocket" -H "Connection: Upgrade"      -H "Sec-WebSocket-Key: YWJjZGVmZ2gxMjM0NTY3OA=="      -H "Sec-WebSocket-Version: 13" http://127.0.0.1:8765/ws | head -1
```

### 坑 7：`clients[]` 不能用下标跨 `poll()` 传递

`http_thread` 曾经在 `poll()` 前记下 `ws_idx[]`，回来后按这个下标访问 `clients[]`。
但中间主线程的 `push_frame()` 会调 `drop_client()`，那个函数把**末尾元素搬进被删的槽**
并 `nclients--` —— 于是下标指向了别的客户端，甚至 `>= nclients` 越界读。

现在每轮分配单调递增的 `id`，回来后 `client_by_id()` 反查，查不到就跳过。
**用 fd 也不行**（fd 会被内核复用）。`client_add` / `drop_client` / 遍历一律持 `g_cl_mu`。
主线程判断「有没有人在看」也必须走 `has_viewers()`，不能裸读 `nclients`。

---

## 4. 已知限制 / 下一步可做的

### 明确的限制

| 限制 | 原因 | 难度 |
|---|---|---|
| **只能操作原生 Wayland 客户端** | XWayland 窗口走另一条合成路径，抓不到也注不进去 | 中（需实现 X11 输入注入） |
| 无音频 | 只传画面 | 中（PipeWire） |
| 无剪贴板同步 | 未实现 `wl_data_device` | 中 |
| 多客户端会竞争 | `clients[]` 没有所有权概念 | 低 |
| 无鉴权 token / 无 TLS | 已加 Origin+Host 校验，但无 token | 低（要远程访问则必做） |
| 仅 wlroots 系合成器 | screencopy / virtual_pointer 是 wlroots 私有协议 | 高 |

### 如果要继续做，建议顺序

1. **剪贴板 / 正文直传** —— 实用性提升最大。顺带解决「打不了中文」：
   逐键注入永远解决不了非 ASCII，必须在 WS 协议里加一条「把这段文本塞进去」的通道
   （或走 `wl_data_device` 剪贴板）
2. **CI 加键鼠注入回归测试** —— 现在 5 个坑全靠人工复验，CI 只测了快照抓帧
3. **多客户端所有权** —— 并发越界已修，但「谁在操作」仍无概念，两个浏览器同时接管会互相抢
4. **XWayland 支持** —— 注意：firefox 加 `MOZ_ENABLE_WAYLAND=1`、Electron 加
   `--ozone-platform=wayland` 就是原生 Wayland，**成本远低于实现 X11 输入注入**，先试这个
5. 音频 —— 依赖 PipeWire，与 Wayland 协议无关，是独立子系统

### 代码里刻意留下的扩展点

`src/wstream.c` 的 `struct capture` 是单一输出。要支持多显示器，把它改成数组即可。
`docs/ARCHITECTURE.md` 最后一节列了每个扩展方向该改哪里。

---

## 5. 怎么验证改动没搞坏

```bash
# 1. 编译（必须零警告）
./build.sh
./build.sh 2>&1 | grep -i warning && echo "有警告！" || echo "干净"

# 2. 起沙箱
./scripts/start.sh 1280x720

# 3. 首页可达
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8765/

# 4. 抓帧正常（不需要浏览器）
WL=$(cat /tmp/wstream/wl_name)
XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=$WL ./wstream -p 8899 -s /tmp/shot.ppm
ffmpeg -i /tmp/shot.ppm /tmp/shot.png && echo "看图确认有内容"

# 5. 键鼠注入（最关键，必须人工确认）
#    通过 ws://127.0.0.1:8765/ws 发 8 字节帧，然后在沙箱里截图看结果
#    或直接用浏览器打开 http://127.0.0.1:8765 点画面打字

# 6. 停
./scripts/start.sh stop
```

### 调试神器

```bash
# 看 Wayland 请求是否真的发出去了
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "motion|button|keymap|key\("

# 抓沙箱屏幕（-c 会渲染光标）
XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=$WL grim -c /tmp/s.png
```

---

## 6. 这台机器的环境事实

接手时省得重新摸索。

| 项 | 值 |
|---|---|
| 发行版 | CachyOS（Arch 系），内核 `7.3.0-2-cachyos` |
| 用户桌面 | Hyprland 0.56.2，`WAYLAND_DISPLAY=wayland-1`，`XDG_RUNTIME_DIR=/run/user/1000` |
| 仓库位置 | `~/githome/wstream` |
| 远端 | `https://github.com/pingod/wstream`（public） |
| 已装相关包 | sway / labwc / cage / grim / wtype / wayland-info / wlr-protocols / ydotool |
| **`/dev/uinput` 不可用** | 内核没编该模块 → xdotool/ydotool **对真实会话无效**，别在这上面浪费时间 |
| 真实会话的 Wayland socket | 直接 `connect()` 被策略拒绝（EACCES）→ **不要试图中继到真实会话** |

### 环境相关的两个陷阱

**① Python 必须清掉继承的环境变量**

```bash
env -u PYTHONHOME -u PYTHONPATH python3 ...
```

否则报 `Fatal Python error: Failed to import encodings module`（继承的 `PYTHONHOME`
指向某个 AppImage 挂载点）。

**② 后台进程用 `setsid` + `nohup`，否则会被父 shell 的 SIGTERM 连带杀掉**

```bash
nohup setsid env ... <cmd> > log 2>&1 < /dev/null &
disown
```

---

## 7. 文档地图

| 想知道 | 看哪里 |
|---|---|
| 项目是什么、怎么用 | `README.md` |
| Wayland 协议细节、为什么这么设计 | `docs/PROTOCOLS.md` |
| 线程模型、主循环、数据结构 | `docs/ARCHITECTURE.md` |
| 某个症状怎么排查 | `docs/TROUBLESHOOTING.md` |
| 版本历史、修过哪些 bug | `CHANGELOG.md` |
| 给 AI agent 的操作手册 | `skills/wstream/SKILL.md` |

---

## 8. 接手时的建议动作

```bash
cd ~/githome/wstream
git log --oneline          # 看历史
cat CHANGELOG.md           # 看修过什么
./build.sh                 # 确认能编译
./scripts/start.sh         # 确认能跑
# 浏览器打开 http://127.0.0.1:8765，点画面，打字试试
```

**动手改代码之前，先把第 3 节的七个坑读一遍。** 前 5 个是几小时起步的排查成本，
坑 6 是安全漏洞，坑 7 是随机越界。

---

## 9. 一条原则

这个项目的**全部价值**在于「不干扰用户」。

任何时候如果改动可能让沙箱的东西跑到用户真实桌面上（窗口、输入、焦点），
**停下来重新设计**，而不是加个 flag 绕过。

隔离性一旦破了，这个项目就没有存在意义了。
