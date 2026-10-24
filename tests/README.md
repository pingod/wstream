# 测试

## 为什么需要它

wstream 有 7 个「踩过就几小时」的坑（见 `docs/HANDOFF.md` 第 3 节）。在这之前，
它们全靠人工复验 —— 每次改注入相关代码，都得手动点一遍、打个字、看看对不对。
这个目录把那些检查变成了自动断言。

**核心设计：断言「事件真的到达应用」，而不是比对截图。**
截图会因终端每秒刷新而变化，断言极不稳定。这里用一个 X11 探针
（`x11probe.c`）把收到的按键/鼠标事件按行打印，逐行精确比对。

## 快速跑

```bash
./build.sh
./scripts/start.sh 1280x720
gcc -O1 -o tests/x11probe tests/x11probe.c $(pkg-config --cflags --libs x11)
python3 tests/wstest.py
./scripts/start.sh stop
```

退出码 0 = 全通过。

## 覆盖什么

| # | 项目 | 防的是哪个坑 |
|---|---|---|
| 1 | 来源校验（恶意 Origin/Host 必须 403） | 1.0.1 的安全修复 |
| 2 | 鼠标点击到达 | 按钮码要 BTN_LEFT(0x110)，不是 1/2/3 |
| 3 | 鼠标坐标准确 | `motion_absolute` 的 x/y 是**像素**，不是 0..65535 |
| 4 | ASCII 键盘到达 | 键码不能 +8 |
| 5 | 中文注入到达 | Unicode 要走 keymap 重映射 |
| 6 | 中文之后 ASCII 仍正常 | 用完必须把完整 evdev keymap 推回去（wtype 的老毛病） |
| 7 | 多客户端所有权 | 非持有者的输入必须被服务端丢弃 |
| 8 | 剪贴板双向同步 | data-control 不能要求焦点；type 7/8 不能被所有权拦掉 |

## 三个容易踩的测试环境问题

**① `SWAYSOCK` 必须显式设置。**
`swaymsg` 靠它找 IPC socket，没设就报 `Unable to retrieve socket path`，
查不到窗口坐标。测试会退化成「点屏幕中心」，然后一路失败。

**② 新窗口会让 sway 重新平铺。**
起一个终端之后，之前探针的位置就变了。每次点击前都要**重新查一次坐标**，
不能缓存（踩过：步骤 5 之后探针从 (1067,360) 挪到了 (800,360)）。

**③ `wl-copy` 会 fork 常驻，不能 `subprocess.run`。**
它要活着持有 selection source（数据源），`run()` 会一直等到超时。用 `Popen`
且不要 `wait()`。

## 中文为什么不对 X11 客户端断言

Unicode 注入的做法是把字符映射到 keycode 9 再按下去。这个 keycode 在
**原生 Wayland 客户端**那边是我们刚推的自定义 keymap（正确）；但
**Xwayland 有自己独立的一套 keymap**，keycode 9 在它那里永远是 Escape。

所以中文只对原生 Wayland 客户端有效（这也是 wstream 的设计前提），
测试改用 alacritty（原生 Wayland）写文件来断言。
