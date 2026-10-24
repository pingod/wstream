# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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
