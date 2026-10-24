# 故障排查

按症状索引。每条都是真机踩过的。

---

## 画面相关

### 浏览器一片黑 / 一直转圈

**检查 1：wstream 是否在跑**

```bash
pgrep -a wstream
curl -sS -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8765/
```

**检查 2：沙箱里有没有窗口**

```bash
WL=$(cat /tmp/wstream/wl_name)
XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=$WL grim /tmp/s.png
```

如果 `grim` 出来的图也是黑的 → 沙箱里没有窗口（`desktop.sh` 没跑起来）。

**检查 3：`desktop.sh` 是否执行**

```bash
pgrep -af desktop.sh
tail -20 /tmp/wstream/sway.log
```

sway 没有 `-S` 参数（那是 labwc 的），必须写进配置文件：

```
exec /tmp/wstream/desktop.sh
```

### 只有第一帧，之后不动

抓帧循环里 poll 超时分支漏了 `wl_display_dispatch_pending()`。

合成器在画面不变时不给可读事件，必须主动 dispatch 已缓冲的事件。

### 画面是花屏 / 颜色错乱

buffer 格式用了自己猜的值。必须用 `ev_buffer` 回调里合成器给的 `format`：

```c
static void ev_buffer(..., uint32_t format, ...) {
    c->fb_format = format;   /* 不要写死 WL_SHM_FORMAT_ARGB8888 */
}
```

---

## 鼠标相关

### 鼠标完全不动

**第一步：确认虚拟指针对象创建成功**

```bash
./wstream -v -p 8765 2>&1 | grep -E "就绪|指针"
```

看到 `✅ 虚拟键盘 + 虚拟指针已就绪` 才算创建成功。
如果显示 `（鼠标不可用）`，说明 `create_virtual_pointer` 失败。

**第二步：确认请求真的发出去了**

```bash
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "motion|button"
```

正常应该看到：

```
-> zwlr_virtual_pointer_v1#15.motion_absolute(55719593, 32767, 36408, 1280, 720)
-> zwlr_virtual_pointer_v1#15.frame()
-> zwlr_virtual_pointer_v1#15.button(55719913, 1, 1)
```

如果请求发出了但光标不动 → **合成器不支持**，见下一条。

**第三步：换合成器**

| 合成器 | motion_absolute |
|---|---|
| sway | ✅ |
| labwc | ❌ 静默丢弃 |
| Hyprland | ❌ |

wstream 现在用相对位移累积（兼容性最好），但 labwc 下仍然可能有问题，
**推荐直接用 sway**：

```bash
sudo pacman -S --needed sway
```

### 只有屏幕左半边能点

`handle_msg` 把坐标按 `int16_t` 读了。坐标是**无符号** 0..65535，
`x > 32767` 被解成负数 → 转 uint32 变成天文数字 → 合成器忽略。

```c
/* ❌ 错误：右半边全废 */
int a = (int)(int16_t)((m[4]) | (m[5] << 8));

/* ✅ 正确：坐标用无符号 */
int ua = (int)((m[4]) | (m[5] << 8));
```

### 鼠标能移动但点击无效

检查 `handle_msg` 里 `type == 2` 的分支是否把 `B` 当成了坐标而非 state：

```c
case 2: inject_button((uint32_t)ua, (uint32_t)(sb != 0)); break;
/*                                            ^^ B 是按下/抬起 */
```

### 点击不切换窗口焦点

合成器没有实现点击聚焦。labwc 默认 `<followMouse>no</followMouse>`，
但更可能是 `motion_absolute` 根本没生效（光标位置是错的，点击自然落空）。

**换 sway**，它默认点击即聚焦。

---

## 键盘相关

### 打字出来是乱码（`abc` → `l]c`）

**键码多加了 8。**

```c
/* ❌ */
zwp_virtual_keyboard_v1_key(kbd, t, evdev_code + 8, state);

/* ✅ */
zwp_virtual_keyboard_v1_key(kbd, t, evdev_code, state);
```

协议里的 `code` 是 evdev 扫描码，合成器自己 +8 转 xkb keycode。

### 打字完全没反应，但鼠标正常

**检查 1：keymap 是否推送成功**

```bash
./wstream -v -p 8765 &
cat /tmp/wstream-keymap.txt
```

应该看到完整的 `xkb_keymap { ... }` 结构。

**检查 2：keymap 是否被合成器接受**

```bash
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | grep -E "keymap|key\("
```

看到 `-> zwp_virtual_keyboard_v1#N.keymap(...)` 之后应该有 `key(...)`。

**检查 3：keymap 里的键位是否齐全**

如果用了「只列单个键位」的最小表（wtype 的做法），其余键位是 NoSymbol，
打一个键之后整个键盘就废了。用完整 `include` 形式：

```
"xkb_keycodes \"x\" { include \"evdev+aliases(qwerty)\" };\n"
"xkb_symbols \"x\" { include \"pc+us+inet(evdev)\" };\n"
```

### 组合键（Ctrl+C）不生效

修饰键事件顺序错了。必须是 **modifiers → key**：

```c
zwp_virtual_keyboard_v1_modifiers(kbd, mods, 0, 0, 0);   /* 先 */
zwp_virtual_keyboard_v1_key(kbd, t, code, 1);            /* 后 */
```

### 按键被静默丢弃

**检查 WebSocket 帧长度。**

服务端 `handle_msg` 开头是 `if (n < 8) return;`。
如果前端发的是 5 字节帧，所有事件会被静默丢弃——**表现为「鼠标能动、打字毫无反应」**。

用 `wireshark` 或抓包确认帧是 8 字节。

---

## 启动相关

### `sway 没起来`

```bash
tail -50 /tmp/wstream/sway.log
```

常见原因：

| 日志 | 原因 | 解法 |
|---|---|---|
| `failed to execute 'swaybg'` | 没有壁纸工具 | 无害，忽略 |
| `Cannot open backend` | 缺 `WLR_BACKENDS=headless` | 检查环境变量 |
| `Permission denied` | `XDG_RUNTIME_DIR` 权限 | `chmod 700` |

### `连不上 Wayland`

```
wstream: 连不上 Wayland（WAYLAND_DISPLAY=..., XDG_RUNTIME_DIR=...）
```

**检查 socket 是否真的存在**

```bash
ls -l /tmp/wstream/run/
cat /tmp/wstream/wl_name     # 实际 socket 名
```

sway 的 socket 名不一定是 `wayland-0`，多实例时会是 `wayland-1`。

### 窗口跑到我真实桌面上去了 ⚠️

**这是最严重的问题，会导致用户失去对自己电脑的控制。**

原因：GUI 程序继承了 `DISPLAY=:0`，连到了真实桌面的 Xwayland。

**必须这样启动**：

```bash
setsid env -u DISPLAY \
    XDG_RUNTIME_DIR=/tmp/wstream/run \
    WAYLAND_DISPLAY=wayland-1 \
    GDK_BACKEND=wayland \
    <程序>
```

三个环境变量缺一不可：
- `-u DISPLAY` —— 去掉 X11 入口
- `XDG_RUNTIME_DIR` + `WAYLAND_DISPLAY` —— 指向沙箱
- `GDK_BACKEND=wayland` —— 强制 GTK 走 Wayland

**检查是否泄漏**：

```bash
# 看沙箱进程的环境对不对
for p in $(pgrep -x firefox); do
  tr '\0' '\n' < /proc/$p/environ | grep -E "^(DISPLAY|XDG_RUNTIME_DIR)="
done

# 看真实桌面有没有多出窗口
hyprctl clients | grep "^Window" | wc -l
```

---

## 性能相关

### CPU 占用高

```bash
top -p $(pgrep wstream)
```

| CPU | 可能原因 |
|---|---|
| wstream > 20% | JPEG 质量太高（`-q` 降到 60）或分辨率太大 |
| sway > 10% | 沙箱里有程序在疯狂重绘（如 `while true; do ...; done` 无 sleep） |

### 带宽高

```bash
# 实测下行速率
curl -sS http://127.0.0.1:8765/stream.mjpg | pv > /dev/null
```

正常值 @10fps 质量 70：**~0.16 MB/s**。

如果远高于此：

| 原因 | 解法 |
|---|---|
| 用了 PNG | 换 libjpeg-turbo（本项目已用） |
| 质量太高 | `-q 50` |
| 帧率太高 | `-f 5` |

### 延迟高

MJPEG 每帧独立，延迟 ≈ 1/帧率 + 编码时间。@10fps 约 100ms。

要更低延迟就提高帧率（但带宽线性增长）：

```bash
./wstream -p 8765 -f 20 -q 60
```

---

## 调试工具箱

```bash
# 1. 单帧快照（不需要浏览器）
./wstream -p 8899 -s /tmp/shot.ppm
ffmpeg -i /tmp/shot.ppm /tmp/shot.png

# 2. 抓沙箱屏幕（会渲染光标，用 grim -c）
WL=$(cat /tmp/wstream/wl_name)
XDG_RUNTIME_DIR=/tmp/wstream/run WAYLAND_DISPLAY=$WL grim -c /tmp/s.png

# 3. 抓 Wayland 协议流
WAYLAND_DEBUG=client ./wstream -p 8765 2>&1 | tee /tmp/wl.log
grep -E "motion|button|keymap|key\(" /tmp/wl.log

# 4. 检查沙箱进程
pgrep -a sway
pgrep -a wstream
ls -l /tmp/wstream/run/

# 5. 检查是否有进程泄漏到真实桌面
hyprctl clients | grep "^Window"

# 6. 查看 wstream 日志
tail -f /tmp/wstream/wstream.log
```

---

## 还是没有解决？

提 issue 时请附上：

1. `./wstream -v` 的完整日志
2. `WAYLAND_DEBUG=client` 的协议流（截取相关片段）
3. 合成器类型和版本（`sway --version` / `labwc --version`）
4. `wayland-info` 输出（确认协议可用性）
