# 交接文档

> **给接手这个项目的 AI agent 或开发者。**
> 读完这份就能知道：现在到哪一步了、哪些坑不能再踩、下一步该做什么。
>
> 最后更新：2026-10-24

---

## 0. 一句话现状

**wstream 1.3.1 已可用。** 核心功能（抓帧 / 推流 / 鼠标 / 键盘 / 中文 / 多客户端 /
剪贴板 / 快照端点 / 窗口列表 / Python 客户端）全部在真机验证通过。

两个重要的版本变化：
- **1.0.1** 修了安全漏洞：WebSocket 原先不校验 `Origin`，任意网页的 JS 都能连进来
  操作沙箱（已实测复现并修复）。
- **1.1.0** 加了中文输入、多客户端所有权、自动化回归测试；并修了三个
  「点不准/点不动」的真 bug（坐标、按钮码、光标初始位置假设）。
- **1.2.0** 加了剪贴板双向同步，顺带解决了「XWayland 里打不了中文」。
- **1.3.0** 加了 `/snapshot.jpg`、`/windows`、`clients/wstream_client.py`；
  修了两个只有测大小写/粘贴才会暴露的修饰键 bug。
- **1.3.1** 修 CI（僵尸进程误判 + 容器缺 Xwayland）。**运行时代码零改动**。

```bash
git log --oneline      # 看最新几条即可，历史详见 CHANGELOG
```

tag：`v1.0.0`、`v1.0.1`（安全修复）、`v1.1.0`（中文+所有权+测试）、
`v1.2.0`（剪贴板同步）、`v1.3.0`（快照/窗口/客户端库）、
`v1.3.1`（CI 修复，**推荐**）。

**改完代码先跑 `python3 tests/wstest.py`**（24 项断言，见第 5 节）。

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
| **中文 / Unicode 输入** | `before 中文 after` 顺序注入，三者全对 | ✅ |
| **多客户端所有权** | 无权客户端注入的 `hacker` 完全没出现 | ✅ |
| **剪贴板 沙箱→浏览器** | 页面读到「沙箱剪贴板内容 abc」 | ✅ |
| **剪贴板 浏览器→沙箱** | 沙箱 `wl-paste` 读到「页面推来的内容」 | ✅ |
| **XWayland 中文（走剪贴板）** | X11 客户端收到 `CLIP 中文通过剪贴板` | ✅ |
| **注入回归测试** | `tests/wstest.py` 24 项断言（含 Shift 释放） | ✅ 全过 |
| **`/snapshot.jpg`** | 20 路并发全部完整 JPEG | ✅ |
| **`/windows`** | 坐标与 `swaymsg` 一致 | ✅ |
| **Python 客户端库** | ASCII/中文/大写全部「真的落到终端」 | ✅ |
| CI | GitHub Actions 编译 + 零警告 + 冒烟测试 + 注入回归 | ✅ 全绿 |
| 来源校验 | 恶意 `Origin` / `Host` 回 403，正常客户端不受影响 | ✅ |
| 并发安全 | ASan + UBSan 下 5 路 WS 反复连断 + MJPEG 流压测 | ✅ 无报告 |

---

## 3. ⚠️ 二十九个不能再踩的坑（最重要的一节）

这些是花了大量时间定位出来的，**改动相关代码前务必先读**。
前 5 条会直接让功能失效，坑 6 是安全问题，坑 7 会随机崩溃/越界，
坑 8~10 让鼠标「点不准/点不动」，坑 11 会打坏键盘，坑 12 是文档纠错，
坑 13~16 是剪贴板，坑 17~20 是 1.3.0 新踩的，坑 21~22 是多实例，坑 23~24 是**隔离性事故**（最严重），坑 25~27 是进程泄漏，坑 28~29 是测试自身的坑（`setsid` 让 terminate 失效、sway JSON 的 null）。

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

### 坑 8：`motion_absolute` 的 x/y 是**像素**，不是 0..65535

协议原文：`Value of x can range from 0 to x_extent`。想当然按归一化 0..65535 传，
坐标全部超出 extent 被钳到边缘 —— 现象是「光标只在角落、点哪都不对」。
实测对照：

```
motion_absolute(640, 360, 1280, 720)   → 探针收到 (638,358)  ✅
motion_absolute(32000, 32000, 1280,720) → 无任何移动          ❌
```

现在 `inject_move` 里做归一化→像素换算。前端仍发 0..65535（与画面缩放解耦）。

### 坑 9：相对位移累积依赖「光标初始位置」，而这个假设是错的

1.0.x 的 `inject_move` 用 `dx = 目标 - g_cur`，`g_cur` 初值假定 (0,0)。
**但合成器重启后光标不在 (0,0)** —— 实测初始在 (99,99)，于是每次点击都偏移
99px，而且越点越偏。这类「隐式状态假设」的 bug 极难从现象反推。

现在改用 `motion_absolute`，不再依赖任何「当前位置」状态。

### 坑 10：按钮码要 BTN_LEFT，不是 1/2/3

前端习惯发 `1=左 2=中 3=右`，但 `zwlr_virtual_pointer_v1.button` 要的是
Linux input-event-codes 的 `BTN_*`（0x110 起）。直接发 1/2/3，
X11 客户端收到的是 `button=104/96/197` 这种垃圾值，点击自然没反应。

`map_button()` 负责翻译（1→0x110、2→0x112、3→0x111），也接受标准码直传。

### 坑 11：Unicode 注入用完必须把 evdev keymap 推回去

打中文要临时推一张「只含该字符」的 keymap（`key <K1> {[U4E2D]};`），
按完必须立刻 `push_keymap_snapshot()` 恢复完整布局。

**不恢复的后果**：后续普通按键全落到那张只有一个键的表上，表现为
「打过一次中文之后，键盘就再也不灵了」。wtype 有这个问题，我们不能重犯。

### 坑 12：XWayland **不是**「抓不到也注不进去」

1.0.0 的文档写错了。实测在沙箱里跑一个 X11 客户端：

| 能力 | 实测结果 |
|---|---|
| 窗口被抓进画面 | ✅ 截图可见 |
| 鼠标点击注入 | ✅ 探针收到 `BTN 1` |
| ASCII 键盘注入 | ✅ 探针收到 `KEY a` |
| 中文注入 | ❌ keycode 9 在 Xwayland 里是 Escape |
| 隔离性 | ✅ `XDG_RUNTIME_DIR` 指向沙箱，用户桌面窗口数不变 |

**结论**：XWayland 的键鼠是可用的，只有中文受限。「做 XWayland 支持」这个
待办可以撤掉。真要让 firefox / Electron 更好用，加
`MOZ_ENABLE_WAYLAND=1` / `--ozone-platform=wayland` 走原生 Wayland 更简单。

### 坑 13：剪贴板必须用 data-control，不能用 `wl_data_device`

`wl_data_device.set_selection()` 要求调用方的 surface **有焦点**。wstream 根本
没有窗口（这是核心设计），所以那条路走不通。

`zwlr_data_control` 是**专门给剪贴板管理器**的协议，不需要焦点。实测无头 sway
里 `wl-copy`/`wl-paste` 完全正常。

### 坑 14：`offer` 监听器必须在 `data_offer` 里就挂上

合成器发事件的顺序是：

```
data_offer(offer对象)  →  offer("text/plain") × N  →  selection(offer)
```

如果等到 `selection` 里才 `add_listener`，前面那些 `offer` 事件到达时没有监听者，
被 libwayland 直接 **discard**（日志里是一堆 `discarded ...offer("text/plain")`），
于是永远不知道有哪些 mime 可用，也就读不到数据。

现在在 `dc_data_offer` 里立刻挂监听器，并用 `set_user_data` 把上下文关联到对象上。

### 坑 15：剪贴板帧不能走输入所有权检查

type 7/8（剪贴板读写）如果放在 `is_owner()` 检查之后，会被当成输入事件丢掉。
现象是「页面读剪贴板一直超时」。

剪贴板不该要求接管 —— 围观者也能复制沙箱里的文字。所以这两个 type 要在
所有权检查**之前**处理。

### 坑 16：`wl-copy` 会 fork 常驻，别用 `subprocess.run` 等它

`wl-copy` 默认 fork 到后台持有 selection source（数据源必须活着，否则数据消失）。
用 `subprocess.run` 会一直等到超时。测试里要用 `Popen` 且不 `wait()`。

（另一个坑：`printf X | wl-copy </dev/null` —— `</dev/null` 会覆盖管道，
wl-copy 读到空内容。别画蛇添足加重定向。）

### 坑 17：不要每键都推 keymap —— 会把修饰位清零

1.1.0 为了「盖掉别的客户端留下的残缺 keymap」，改成**每个键都推一次**完整
evdev keymap。后果：合成器收到新 keymap 会重建键盘状态，**修饰位被清零**，
于是 Ctrl+V 退化成傻按一个 `v`、`Shift+v` 出小写 `v`。

现在用 `g_keymap_fresh` 标记：正常只推一次，Unicode 注入换掉 keymap 后才标脏、
下次按键前重推。

> 这个 bug 在 1.1.0/1.2.0 存在但没被测出来，因为**没有一条测试测大写字母**。

### 坑 18：修饰键全释放时也必须发 modifiers

```c
if (g_mods) zwp_virtual_keyboard_v1_modifiers(...);   /* ❌ 归零时不发 */
```

修饰键全部松开时 `g_mods` 变成 0，`if (g_mods)` 让这次 modifiers **不发**，
合成器就一直以为 Shift 还按着 —— 现象是打 `Upper` 得到 `UPPER`。

正确做法：比较「上次发出去的值」（`g_mods_sent`），**变了就发，包括发 0**。

### 坑 19：终端里粘贴是 Ctrl+Shift+V，不是 Ctrl+V

`Ctrl+V` 在终端里是「引号转义」的意思，不会粘贴。GUI 程序（浏览器/编辑器）
才是 Ctrl+V。`clients/wstream_client.py` 的 `paste()` 默认用 `ctrl+shift+v`，
往 GUI 程序里贴要显式传 `keys="ctrl+v"`。

### 坑 20：`/windows` 不能用 `zwlr_foreign_toplevel_management`

那个协议只给 title / app_id，**不给坐标**。`set_rectangle` 是给任务栏的提示，
不是真实位置。而 agent 最需要的恰恰是坐标。

只能读 sway 的 i3-ipc `GET_TREE`（沙箱里本来就跑着 sway）。
解析时**不要用 strstr 硬找**：sway 的 JSON 里 `rect` 在 `app_id` **之前**、
节点还是嵌套的，按「app_id 之后找 rect」会错位（实测两个窗口只认出 1 个、
坐标还串了）。仓库里有一个最小递归下降扫描器（`js_object`），够用且零依赖。

### 坑 21：`pgrep -x wstream` 是全局的，多实例会互相踢掉

支持多实例（`WSTREAM_ROOT` + `WSTREAM_PORT` 各自一套），但**进程查找不能只看名字**。

1.3.1 修僵尸 bug 时我加了一段「有进程但端口不服务 → `kill -9` 全部 wstream」。
看着没问题，实际是：起 A(8801) 后起 B(8802)，B 发现「有 wstream 进程但 8802 没在
服务」，就把 **A 也杀了**。结果是「后起的踢掉先起的」。

正确做法：按 `XDG_RUNTIME_DIR` 判断进程归属，只操作本实例的。

```bash
belongs_to_root() {   # 见 scripts/start.sh
  rt=$(tr '\0' '\n' < "/proc/$1/environ" | sed -n 's/^XDG_RUNTIME_DIR=//p')
  [ "$rt" = "$ROOT/run" ]
}
```

`stop_all` 里本来就是这个写法（是对的），只有 `start` 那段漏了。

### 坑 22：`stop` 必须带 `WSTREAM_ROOT`

```bash
WSTREAM_ROOT=/tmp/ws-a ./scripts/start.sh stop   # ✅ 停 A
./scripts/start.sh stop                          # ❌ 按默认 /tmp/wstream 处理
```

不带的话停的是默认实例，或者什么都没停 —— 而且**看起来是成功的**（打印「已停止」）。

### 坑 23：测试里 `DISPLAY=":0"` 把窗口开到了用户真实桌面上

**这是我犯过的性质最严重的错。** 不是测试失败，是**隔离性被破坏**。

`tests/wstest.py` 原来写：

```python
env["DISPLAY"] = ":0"        # 沙箱内的 XWayland
```

注释是对的意图，假设是错的。沙箱 Xwayland 的 display 号**取决于宿主机已占用几个 X**：

| 场景 | `:0` 是谁的 |
|---|---|
| CI 裸容器（没跑桌面） | 沙箱的 ✅ |
| 用户桌面跑着 Hyprland/GNOME | **用户真实的桌面** ❌ |

实测：用户桌面 `:0` = 4000x2560 的 Hyprland，沙箱 Xwayland = `:1` (1280x720)。
用测试原样的方式起探针，`xdotool` 在 `:0` 上查到了 `WSTREAM_X11_PROBE` 窗口 ——
就开在用户屏幕上。接下来的鼠标点击、键盘注入全都进了用户的真实会话。

**为什么难以发现**：表现为「3 项断言失败」（点击没到达、按键没到达），
看起来像注入代码坏了，实际是「探针根本不在沙箱里」。而 CI 是裸容器，
`:0` 恰好就是沙箱的，所以 **CI 一直是绿的** —— 最坏的一种：本地掩盖、CI 掩盖。

**修法**：`sandbox_display(env)` 三步探测（swaymsg exec 回显 → 扫 /proc 按
XDG_RUNTIME_DIR 找 Xwayland → 都没有就删掉 DISPLAY）。

**核心教训**：兜底的方向。原来 `or ":0"` 的意思是「探测失败就用默认」，
但默认值恰恰是最危险的那个。**失败要往「连不上」的方向失败，不能往
「连到用户桌面」的方向失败。** 现在探测失败就 `env.pop("DISPLAY")`，探针直接连不上。

同类风险清单（改测试时必须过一遍）：
- `grim` / `wlr-randr` / `wl-copy` / `wl-paste` / `ydotool` —— 任何碰 `WAYLAND_DISPLAY`
  或 `XDG_RUNTIME_DIR` 的工具，都必须显式指向沙箱
- X11 工具（`xdotool`/`xclip`/`xwd`）—— 同理，靠 `DISPLAY`
- 之前还发生过一次：抓截图忘了设 `XDG_RUNTIME_DIR`，抓出了用户真实桌面（看到其浏览器/终端）

### 坑 24：验证「隔离」不能只看断言通过

上面那个 bug 能长期潜伏，是因为我一直用「断言是否通过」判断测试好坏。
断言失败可能是功能坏了，**也可能是测试打错了地方**。

**必须单独验证「东西确实在沙箱里」**，例如：
```bash
DISPLAY=:0 xdotool search --name "WSTREAM_X11_PROBE" | wc -l   # 真桌面应为 0
DISPLAY=:1 xdotool search --name "WSTREAM_X11_PROBE" | wc -l   # 沙箱应为 1
```

### 坑 25：`own_pids` 漏了 `desktop.sh`，每轮 start/stop 漏一批进程

`desktop.sh` 是写进 sway 配置的 `exec` 钩子，内部是 `while ... sleep 36000`，
它自己再起两个 alacritty。原来 `own_pids()` 只列了三个名字：

```bash
for name in wstream sway-nocap sway; do ...   # ← desktop.sh 和窗口程序都不在里面
```

于是 `stop` 时 `desktop.sh` 和残留的 alacritty 从来没被杀过。**每跑一次
start/stop 就漏一批**，实测累积到 17 个孤儿 `desktop.sh`（有的已经跑了两小时），
它们还占着内存、持有旧的 XDG_RUNTIME_DIR。

修法：`own_pids` 覆盖沙箱里会出现的所有程序名
（`wstream sway-nocap sway desktop.sh alacritty foot kitty xterm`）。
列的名字多一点**不会**误伤用户桌面 —— 归属判据仍是 `XDG_RUNTIME_DIR == $ROOT/run`。

### 坑 26：`ls run/wayland-*` 判「沙箱已在运行」→ 进程死了就永久卡死

```bash
if [ -n "$(ls "$ROOT"/run/wayland-* ...)" ]; then
  log "沙箱已在运行（$WL），复用"     # ← 只看了文件存在
```

socket 文件是 sway 退出后残留的普通文件。sway 崩了 / 被 OOM 杀掉之后：
1. 文件还在 → 判定「复用」→ 跳过启动 sway
2. 但没人监听那个 socket → wstream 连不上，立刻退出
3. **永久卡住**：每次 start 都走同一条错分支，只能手动 `rm -rf $ROOT`
4. 报错只说「连不上 Wayland」，看不出根因

修法：`sandbox_alive()` —— 用 `wlr-randr` 真正握手探一下（没有就退化成查 sway.pid
的进程是否活着）。死 socket 会被识别并打印「发现残留的 wayland-N（进程已死），重建沙箱」。

> 这和坑 13/17 是同一个教训的两次重犯：**「文件存在」不等于「服务可用」，
> 判据要落在「真的能连上」上。**（第一次是僵尸进程 pgrep 能匹配，第二次是这个。）

### 坑 27：实例目录被删后，孤儿进程永远清不掉

`stop-all.sh` 最初的归属判据要求 `$root` 目录**存在**且有 pid 文件。
但如果实例目录被 `rm -rf` 了（我的测试脚本就这么干过），它的 `desktop.sh`
还在跑，却因为「目录没了」永远匹配不上任何实例 —— 清不掉。

修法：判据放宽成「环境变量 `XDG_RUNTIME_DIR=<root>/run` **且** cmdline 里有
`<root>/` 开头的路径」。目录删了也能从 cmdline 认出归属。

### 坑 28：`Popen(["setsid", ...], start_new_session=True)` 让 `terminate()` 失效

测试里起探针和终端原来是这么写的：

```python
subprocess.Popen(["setsid", "./tests/x11probe"], ...,
                 start_new_session=True)
```

看着挺稳妥（「脱离当前会话组，免得父 shell 挂了连带杀掉」），实际上
**`start_new_session=True` 已经让子进程成了新 session 的 leader**。
此时再执行 `setsid` 命令，它发现自己已经是 pgroup leader，必须 fork 一次
才能 setsid —— 然后**父进程（那个 setsid）立即退出**。

于是 `p.pid` 指向那个短命的 `setsid`，真正的 x11probe / alacritty 成了孤儿
被 init 收养。**`p.terminate()` 打在一个已经退出的 pid 上，什么都没杀到。**

危害不是「泄漏几个进程」那么轻。每跑一轮测试就在沙箱里多留一个
`WSTREAM_X11_PROBE` 窗口和一对 `WStreamTest` 终端，而 `get_geometry()`
按 window tree 顺序返回**第一个**匹配 —— 第二轮拿到的是上一轮那个没人读的
幽灵窗口坐标，之后所有点击和按键全打进那里。

**表现极具迷惑**：同一份代码第一轮 22/22 全过，第二轮固定挂 3~4 项
（鼠标点击到达 / ASCII 按键 / Enter），看起来像输入注入坏了。CI 是干净容器、
只跑一轮，所以**一直是绿的**。

修法：`start_new_session=True` 就够了，**去掉 `"setsid"`**。
另外加两道保险：
- `kill_stale_test_windows()` 开头清残留（万一被 Ctrl-C 打断）
- 结尾断言 `跑完不留测试窗口` —— 这条很重要，因为开头的清理会**掩盖**
  「terminate() 失效」这个 bug，测试照样全绿，只有一行「清掉 N 个残留」
  的提示容易被无视。把它变成硬断言，bug 就藏不住了。

> 反向验证过：把 `setsid` 加回去，断言立刻变红
> `❌ 跑完不留测试窗口 (残留=['WSTREAM_X11_PROBE'])`，22 通过 / 1 失败。

### 坑 29：sway JSON 里 `app_id` 是显式 `null`，`get(key, "")` 不管用

解析 `swaymsg -t get_tree` 时写 `node.get("app_id", "").startswith(...)`，
容器节点和 X11 窗口的 `app_id` 是显式的 `null`。`get` 的默认值**只在键不存在
时生效**，值是 `null` 时照样返回 `None` → `'NoneType' object has no attribute
'startswith'`，测试直接 Traceback。

修法：用 `(node.get("app_id") or "")`，`or` 能兜住 `None`。

顺带一条同源的：窗口刚 map 时 sway 还没算完布局，第一次读到的坐标是过渡态。
必须**连续两次读到同一坐标**才算稳定（`wait_window_settled()`），
否则照过渡坐标点下去会点到别处。关窗口也一样 —— `terminate()` 之后
要等它真的从 window tree 消失（`terminate_and_wait()`），不能只看进程没了。

---

## 4. 已知限制 / 下一步可做的

### 明确的限制

| 限制 | 原因 | 难度 |
|---|---|---|
| 无音频 | 只传画面 | 中（PipeWire） |
| 无鉴权 token / 无 TLS | 已加 Origin+Host 校验，但无 token | 低（要远程访问则必做） |
| 仅 wlroots 系合成器 | screencopy / virtual_pointer 是 wlroots 私有协议 | 高 |

### 如果要继续做，建议顺序

1. **音频** —— 依赖 PipeWire，与 Wayland 协议无关，是独立子系统
2. **文件传输** —— 沙箱与宿主传文件，目前得靠共享家目录
3. **鉴权 token** —— 已有 Origin/Host 校验；要开放到公网还需 token + TLS

> 已完成的：
> - 1.1.0：中文输入、多客户端所有权、注入回归测试
> - 1.2.0：剪贴板双向同步（顺带解决 XWayland 中文）
> - ~~XWayland 支持~~ 已撤销，实测本来就能用（见坑 12）

### 代码里刻意留下的扩展点

`src/wstream.c` 的 `struct capture` 是单一输出。要支持多显示器，把它改成数组即可。
`docs/ARCHITECTURE.md` 最后一节列了每个扩展方向该改哪里。

---

## 5. 怎么验证改动没搞坏

```bash
# 0. 自动化回归（先跑这个 —— 19 项断言，覆盖 12 个历史坑）
./build.sh
gcc -O1 -Wall -Wextra -o tests/x11probe tests/x11probe.c \
    $(pkg-config --cflags --libs x11)
./scripts/start.sh 1280x720
python3 tests/wstest.py          # 退出码 0 = 全过
./scripts/start.sh stop
# 细节见 tests/README.md。注意两个坑：SWAYSOCK 要设；新窗口会让 sway 重排。

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

**动手改代码之前，先把第 3 节的二十九个坑读一遍。** 每个都是几小时起步的排查成本。

---

## 9. 一条原则

这个项目的**全部价值**在于「不干扰用户」。

任何时候如果改动可能让沙箱的东西跑到用户真实桌面上（窗口、输入、焦点），
**停下来重新设计**，而不是加个 flag 绕过。

隔离性一旦破了，这个项目就没有存在意义了。
