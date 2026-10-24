# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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
