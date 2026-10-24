# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [1.3.1] - 2026-10-24

修 CI。1.1.0 起 CI 连续三次失败而本地全过，根因都在环境差异，不在业务逻辑。

> 这个版本只动 `scripts/start.sh`、`.github/workflows/build.yml`、`tests/`，
> **运行时代码零改动**，行为与 1.3.0 完全一致。

### 修复

| # | 现象 | 原因 |
|---|---|---|
| A | `stop` 后紧接 `start`，报「wstream 已在运行」，但端口 `Connection refused` | 容器里 PID 1 不回收子进程，被 kill 的进程变 `<defunct>`，`pgrep` 照样匹配到 |
| B | 回归测试报 `X11 探针没能启动` | CI 的 archlinux 镜像不带 Xwayland，sway 报 `Cannot find Xwayland binary` |

**A 的修法**：判据从「有没有同名进程」换成三层——
`is_zombie()`（读 `/proc/PID/stat` 第 3 字段）+ `alive()` + `port_serving()`。
**「端口能响应」才是唯一可靠标准。** 另加：stop 后等进程真退出（最多 5s）、
start 后等端口真响应（最多 20s，失败时打印日志并报错，不再靠 `sleep 2` 猜）。

**B 的修法**分两层：
1. CI 依赖补 `xorg-xwayland`（与本地对齐）
2. **更重要的：测试不再硬依赖 X11。** 探针只是观测手段、不是被测功能，
   起不来就降级跳过并说明原因；核心验证（keymap 恢复、修饰键、所有权）
   改成用原生 Wayland 终端跑 —— 「文本真的落地」比「探针收到事件」更接近真实场景，
   且本地与 CI 走同一条主路径

顺带修了测试自身两个 bug：步骤 8 复用了已 close 的连接；探针失败时没有可操作的提示。

### 验证

- 有探针：22 项全过
- 无探针（模拟 CI）：18 项全过，不误报
- CI：编译 / 零警告 / 冒烟测试 / 键鼠注入回归 全部通过

### 教训

「本地通过」不等于「CI 通过」，差异往往在进程回收行为、装了哪些包这类环境细节上。
前三个版本只跑本地就宣布通过，CI 其实一直红着 —— 这类问题只有主动去看才暴露。

## [1.3.0] - 2026-10-24

补齐 agent 实际使用时最缺的三样东西：能看、能定位、能复用。

### 新增：`GET /snapshot.jpg`

之前 agent 想看画面，只能 ① 用 `-s` 重启一个 wstream（会中断推流）
② 装 grim。都不合适。

现在一个 HTTP 请求就拿到 JPEG。带 **800ms 缓存**：连续/并发请求直接命中缓存，
不阻塞 accept 循环（实测 20 路并发全部返回完整 JPEG）。

### 新增：`GET /windows`

返回沙箱里的窗口列表和**精确坐标**，agent 据此知道该点哪里：

```json
{"windows":[{"app_id":"AgentTop","title":"Alacritty@CachyOS",
             "x":0,"y":0,"width":640,"height":720,"cx":320,"cy":360}]}
```

**为什么不用 `zwlr_foreign_toplevel_management`**：那个协议只给 title/app_id，
**不给坐标**（set_rectangle 是给任务栏的提示，不是真实位置）。而坐标恰恰是要的。
所以直接读 sway 的 i3-ipc `GET_TREE`（沙箱里本来就跑着 sway）。

JSON 解析没引第三方库（项目刻意零依赖），写了个最小递归下降扫描器。
一开始用 strstr 硬找，但 sway 的 JSON 里 `rect` 在 `app_id` **之前**、
节点又是嵌套的，按「app_id 之后找 rect」会错位 —— 实测两个窗口只认出 1 个
且坐标串了。换成正经扫描器后与 `swaymsg` 输出完全一致。

### 新增：`clients/wstream_client.py`

可 import 的 Python 客户端，把 SKILL.md 里要手工复制的 117 行代码变成模块。
零依赖（只用标准库）。

### 修复：两个只有测大小写/粘贴才会暴露的真 bug

| # | 现象 | 原因 |
|---|---|---|
| A | `Upper` 打成 `UPPER`（Shift 卡住） | 每键都推 keymap，合成器重建键盘状态时把修饰位清零。1.1.0 为「盖掉别的客户端留下的残缺 keymap」加的每键推送，把组合键全弄坏了 |
| B | Ctrl+V 没反应、只多出字母 `v` | 修饰键全释放时（g_mods 归 0）不发 modifiers，合成器一直以为 Shift 按着 |

为什么之前 24 项测试没发现：**没有一条测过大写字母或粘贴**。
现在补了断言（`Shift+a` 必须出大写 A、随后 `b` 必须是小写）。

顺带确认：终端里粘贴是 **Ctrl+Shift+V**，`Ctrl+V` 是「引号转义」。
客户端库 `paste()` 默认用前者。

### 验证

- 回归测试 24 项全过（新增快照/窗口/修饰键）
- ASan + UBSan 下压测：4 路 WS 反复连断 + 并发快照 + 并发窗口列表 + MJPEG 流，
  无 sanitizer 报告，进程存活
- `clients/` 端到端测试全过：ASCII/中文/大写全部验证「真的落到终端」

## [1.2.0] - 2026-10-24

剪贴板双向同步。顺带解决了「XWayland 里打不了中文」这个 1.1.0 遗留限制。

### 新增：剪贴板同步

沙箱和浏览器之间的剪贴板双向打通，用的是 `zwlr_data_control`
（**不是** `wl_data_device`）。

**为什么必须用 data-control**：`wl_data_device.set_selection()` 要求 surface
有焦点，而 wstream 根本没有窗口 —— 这是项目的核心设计。data-control 是专门
给「剪贴板管理器」用的协议，不需要焦点。实测确认无头 sway 里可用。

| 方向 | 实现 | 验证 |
|---|---|---|
| 沙箱 → 浏览器 | 合成器发 `selection` → `receive` 数据 → 缓存 → WS 下行 `0x81` | ✅ 读到「沙箱剪贴板内容 abc」 |
| 浏览器 → 沙箱 | 页面发 type 7 → `create_data_source` + `set_selection` | ✅ 沙箱 `wl-paste` 读到「页面推来的内容」 |

前端：
- 每 2 秒自动拉一次沙箱剪贴板，状态栏显示内容预览
- 粘贴时**含非 ASCII 就走剪贴板**（写进沙箱剪贴板再发 Ctrl+V），纯 ASCII 仍逐键打
- `Ctrl+Shift+V` 把沙箱剪贴板内容复制到宿主机的剪贴板

### 顺带解决：XWayland 里能打中文了

1.1.0 的记录是「XWayland 打不了中文（keycode 9 冲突）」。现在走剪贴板粘贴
就绕开了这个限制 —— 实测 X11 客户端收到 `CLIP 中文通过剪贴板`。

### 修复（实现过程中踩的）

| # | 现象 | 原因 |
|---|---|---|
| A | 沙箱剪贴板读不到，日志全是 `discarded ...offer(...)` | 监听器加晚了：合成器先发 `data_offer` 再发 `offer` 事件，要立刻 `add_listener` |
| B | 页面读剪贴板一直超时 | 主循环只检查 `g_input_dirty`，而剪贴板请求置的是 `g_clip_dirty` |
| C | 页面推的文本进不了沙箱 | type 7/8 被输入所有权检查拦掉了；剪贴板不该要求接管 |

### 安全说明

沙箱剪贴板与宿主机剪贴板**真隔离**（实测：沙箱写入的内容，宿主机 `wl-paste`
读到的是用户自己的内容）。桥接的是「浏览器页面」和「沙箱」，不碰用户桌面。

剪贴板帧**不受输入所有权限制** —— 围观者也能复制沙箱里的文字、也能贴东西进来，
这不需要接管。数据上限 256KB，超限截断（剪贴板里可能是几个 G 的文件）。

## [1.1.0] - 2026-10-24

三个新能力 + 一批实测发现。**推荐升级**——修了两个会让「点不准、点不动」的真 bug。

### 新增

- **中文 / 任意 Unicode 输入**（`inject_text`）。之前只能逐键注入 ASCII，
  中文根本打不进去。现在的做法是：把字符映射成 xkb 的 Unicode keysym
  （`U4E2D` 这种）推一张临时 keymap，按下后立刻把**完整 evdev keymap 推回去**。
  前端支持 IME（`compositionstart`/`compositionend`）、粘贴、非 ASCII `input` 兜底。
  - 实测：`before 中文 after` 顺序注入，三者全部正确
  - 关键收尾：用完必须恢复 keymap，否则后续普通按键全失效（wtype 的老毛病）
  - 限制：只对**原生 Wayland 客户端**有效。Xwayland 有独立 keymap，
    keycode 9 在它那里永远是 Escape（实测探针收到 `sym=0xff1b`）

- **多客户端输入所有权**。之前两个浏览器同时连会互相抢鼠标（A 移到左边、
  B 移到右边，两边都觉得「点了没反应」）。现在同一时刻只有持有者能操作：
  - `type 6` 控制帧：`action=1` 取权、`action=0` 释放
  - 持有者断开 → 自动释放
  - **非持有者的输入由服务端丢弃**（前端隐藏按钮不够，脚本能直接发帧）
  - 实测：无权客户端注入的 `hacker` 完全没有出现，持有者的 `owner` 正常落地

- **注入回归测试**（`tests/`）。7 个历史坑从「人工复验」变成自动断言。
  用一个 X11 探针把收到的按键/鼠标事件按行打印，精确比对而不是比截图
  （截图会因终端每秒刷新而不稳定）。19 项断言，已接入 CI。

### 修复（实测发现，都会影响日常使用）

| # | 问题 | 原因 |
|---|---|---|
| A | 点击位置偏移 99px，越点越偏 | 相对位移累积假定光标初始在 (0,0)，实际在 (99,99) |
| B | 点击完全没反应 | 按钮码直接发了 1/2/3，协议要的是 BTN_LEFT(0x110) 等 |
| C | 鼠标坐标全被钳到边缘 | `motion_absolute` 的 x/y 是**像素**，我按 0..65535 归一化传了 |

三个都在同一处：`inject_move` / `inject_button`。现在改用 `motion_absolute`
（不再依赖「当前位置」假设），坐标按像素传，按钮码做映射。

实测对照（探针收到的位置）：
```
目标 (200,150)  → MOTION x=197 y=147   ✅ 差 3px 是窗口边框
目标 (1000,600) → MOTION x=997 y=597   ✅
点击            → BTN 1                ✅ 就是左键
```

### 文档纠正

- **XWayland 并不是「抓不到也注不进去」**（这是 1.0.0 的错误结论）。
  实测在沙箱里跑一个 X11 客户端：
  - 窗口**能**被抓进画面（截图可见）
  - 鼠标点击、ASCII 键盘**都能**注入到达（探针收到 `BTN 1`、`KEY a`）
  - 只有**中文**对 X11 客户端无效（keycode 9 冲突，见上）
  - 隔离性也没问题：`XDG_RUNTIME_DIR` 指向沙箱，用户桌面窗口数不变
- 因此「下一步做 XWayland 支持」这个待办可以撤掉了。真的要让 firefox /
  Electron 走原生 Wayland，加 `MOZ_ENABLE_WAYLAND=1` /
  `--ozone-platform=wayland` 更简单。

## [1.0.1] - 2026-10-24

安全性修复。**推荐所有 1.0.0 用户升级**——修掉了一个能被任意网页利用的注入漏洞。

### 安全

- **WebSocket / HTTP 端点新增来源校验**（`ws_authorize` / `hdr_get` / `host_is_loopback` /
  `origin_trusted`）。此前握手完全不看 `Origin` 和 `Host`，实测可复现：
  - 任意网页的 JS 能连 `ws://127.0.0.1:8765/ws`，往沙箱里点任意坐标、敲任意键
    （实测用 `Origin: http://evil.example` 成功在沙箱终端里敲出命令）
  - `Host` 也不校验 → 存在 DNS rebinding 路径

  正因如此，文档里「只监听 127.0.0.1 所以安全」这个结论**原本并不成立**：
  发起连接的正是用户自己的浏览器，绑定回环地址挡不住它。
  现在的规则：
  - `Host` 必须是 `127.0.0.1` / `localhost` / `[::1]`
  - 带 `Origin` 时必须是 `http(s)://127.0.0.1:<port>` 等本页来源
  - 无 `Origin`（curl / skill 里的 Python 脚本）放行——本机进程能直连本就有等价权限
  - 不合法一律回 `403`

### 修复

- **`clients[]` 并发越界**：`http_thread` 在 `poll()` 前记下数组下标，回来后按下标访问，
  中间主线程的 `push_frame()` 可能 `drop_client()` 搬移元素并 `nclients--`，
  导致操作到错的客户端甚至越界读。改为每轮分配单调递增的 `id`，回来后按 id 反查
  （不存在就跳过）；`client_add` / `drop_client` / 遍历统一持 `g_cl_mu`。
- 主线程读 `nclients` 决定是否编码，原来无锁（数据竞争）→ 改走 `has_viewers()`。
- 退出路径：先置 `g_net_stop` 让网络线程收工，再持锁关闭 fd，避免踩已释放的 fd。
- `hdr_get` 逐行解析请求头并跳过名字后的冒号（先前会把 `: ` 带进值里）。

已在 ASan + UBSan 下用「5 路 WebSocket 反复连断（RST）+ 1 路 MJPEG 流」压测，
无 sanitizer 报告，进程存活。

## [1.0.0] - 2026-10-24

首个可用版本。

### 新增

- **画面推流**：`zwlr_screencopy_manager_v1` 抓帧 → libjpeg-turbo 编码 →
  HTTP `multipart/x-mixed-replace` 推 MJPEG，浏览器 `<img>` 原生播放
- **鼠标注入**：`zwlr_virtual_pointer_manager_v1`，支持移动 / 左中右键 / 滚轮
- **键盘注入**：`zwp_virtual_keyboard_manager_v1`，支持完整 ASCII、
  Ctrl / Shift / Alt / Super 组合键
- **WebSocket 上行**：自实现握手（SHA1 + Base64）+ 8 字节定长二进制帧
- **内嵌前端**：零外部依赖的单页（无 CDN、无框架），点击接管、`Ctrl+Esc` 释放
- **一键沙箱**：`scripts/start.sh` 自动起无头 sway + wstream
- **快照模式**：`-s file.ppm` 不需要浏览器就能验证抓帧
- **双缓冲**：`wl_shm` 两块 buffer 轮转，避免撕裂

### 修复（开发期踩的坑，都已解决）

| # | 问题 | 原因 |
|---|---|---|
| 1 | `null value passed for arg 2` | `capture_output` 第三参数不能传 NULL |
| 2 | `listener function for opcode N is NULL` | 监听器结构体未实现的事件必须给空函数 |
| 3 | `curl: (52) Empty reply from server` | `accept4(SOCK_NONBLOCK)` 后直接 `recv` 得到 EAGAIN |
| 4 | `invalid buffer format` | buffer 格式必须用合成器协商出来的（XRGB8888=0x1） |
| 5 | 只有第一帧 | 画面不变时合成器不给可读事件，poll 超时要 `dispatch_pending` |
| 6 | 48 MB/s 带宽 | PNG store 模式每帧 4.8MB，换 libjpeg-turbo 后 80KB |
| 7 | `wl_display_dispatch_queue: Assertion failed` | 两线程同时 dispatch 同一个 `wl_display` |
| 8 | `abc` 打成 `l]c` | 键盘键码多加了 8（合成器自己会加） |
| 9 | 打字完全无反应 | WebSocket 帧 5 字节 vs 服务端期望 8 字节 |
| 10 | 屏幕右半边点不动 | 坐标按 `int16_t` 读，> 32767 变负数 |
| 11 | 鼠标完全不动（一） | `cap.ptr` 从未被赋值（忘了 `create_virtual_pointer`） |
| 12 | 鼠标完全不动（二） | labwc 不实现 `motion_absolute`，请求被静默丢弃 |
| 13 | 窗口泄漏到用户桌面 | 沙箱继承了 `DISPLAY=:0`，程序连到真实 Xwayland |
| 14 | 沙箱黑屏 | sway 没有 labwc 的 `-S` 启动钩子，`desktop.sh` 从未执行 |

### 已知限制

- 只能操作**原生 Wayland 客户端**，XWayland 窗口抓不到也注不进去
- 无音频
- 无剪贴板同步
- 仅支持 wlroots 系合成器（screencopy / virtual_pointer 是 wlroots 私有协议）
- Hyprland 的 headless 后端受限于 aquamarine 缺软件渲染，暂不可用

### 技术选型说明

- **为什么是 MJPEG 不是 H.264**：本地回环带宽不是瓶颈，MJPEG 实现复杂度低一个数量级
- **为什么是 sway 不是 labwc**：labwc 不实现 `motion_absolute`，鼠标完全不可用
- **为什么手写 WebSocket**：只需 2 个端点，约 200 行，避免额外依赖
