# 测试

## 为什么需要它

wstream 有一堆「踩过就几小时」的坑（见 `docs/HANDOFF.md` 第 3 节，目前二十七个）。在这之前，
它们全靠人工复验 —— 每次改注入相关代码，都得手动点一遍、打个字、看看对不对。
这个目录把那些检查变成了自动断言。

**核心设计：断言「事件真的到达应用」，而不是比对截图。**
截图会因终端每秒刷新而变化，断言极不稳定。这里用一个 X11 探针
（`x11probe.c`）把收到的按键/鼠标事件按行打印，逐行精确比对。

## 只在本地跑

```bash
./build.sh
./scripts/start.sh 1280x720
gcc -O1 -o tests/x11probe tests/x11probe.c $(pkg-config --cflags --libs x11)
python3 tests/wstest.py
./scripts/start.sh stop
```

退出码 0 = 全通过。

## 一套测试只跑一遍（重要）

CI 跑的就是上面这两条命令。**本地跑完再推、推完等 CI 再跑一遍，是纯浪费** ——
同一份代码、同一套断言、两个地方，多花的时间和 token 买不到任何信息。

| 情况 | 谁跑 |
|---|---|
| 改了代码要提交 | **CI 跑**。推上去，读 CI 结果 |
| 需要断点/ASan/看中间状态 | 本地跑（CI 做不到这些） |
| CI 红了要复现 | 本地跑（为了迭代快） |
| 真的没网/CI 挂了 | 本地跑，并在提交信息里注明「CI 未验证」 |

对照 1.3.x 的教训：「本地绿 ≠ CI 绿」的正确解法是**去读 CI 的结果**，
不是「两边都跑一遍」。前者是验证，后者只是重复劳动。

同理，CI 已经在跑的时候不要再去手动重跑同一套。等结果就行。

## 两个测试文件

| 文件 | 测什么 | 什么时候跑 |
|---|---|---|
| `tests/wstest.py` | **功能**：注入、来源校验、剪贴板、所有权（22 项） | 改了 src/wstream.c、前端 |
| `tests/lifecycle.py` | **生命周期**：起停是否留垃圾、死 socket、孤儿清理（15 项） | 改了 scripts/ |

功能正常但每跑一次漏一批进程，这种 bug 不会让任何功能断言变红，只能靠
`lifecycle.py` 抓。它只操作 `/tmp/wstream-lc-test`，不碰你的默认实例。

```bash
python3 tests/lifecycle.py     # 约 2 分钟，会真起停几次
```

`lifecycle.py` 覆盖的三个坑（HANDOFF 坑 25~27）都做过**反向验证**：
把修复回退掉，测试必须变红（实测确实变红，并指出是哪个进程残留）。

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
| 9 | 修饰键释放 | Shift 松开后必须发 modifiers=0，否则 `Upper` → `UPPER` |

## ⚠️ 最重要的一条：`DISPLAY` 绝不能硬编码 `":0"`

`wstest.py` 原来写 `env["DISPLAY"] = ":0"`，注释是「沙箱内的 XWayland」。
**假设是错的**，而且这是本项目发生过的最严重事故：窗口开到了用户真实桌面上。

沙箱 Xwayland 的 display 号取决于宿主机**已占用几个** X display：

| 场景 | `:0` 实际是谁 |
|---|---|
| CI 裸容器（没跑桌面） | 沙箱的 ✅ |
| 用户桌面跑着 Hyprland/GNOME | **用户真实的桌面** ❌ |

实测：用户桌面 `:0` = 4000x2560 Hyprland，沙箱 Xwayland = `:1` (1280x720)。
用原来的写法起探针，`DISPLAY=:0 xdotool search` 真就查到了探针窗口 —— 在用户屏幕上。
之后每一次点击、每一次按键注入，都打进了用户的真实会话。

**为什么难发现**：断言表现为「点击没到达」「按键没到达」，看着像注入功能坏了。
而 CI 是裸容器、`:0` 恰好是沙箱的，**CI 全程是绿的**。

现在 `sandbox_display(env)` 三步探测：
1. `swaymsg exec 'sh -c "echo $DISPLAY > /tmp/f"'` —— 让沙箱自己回显，最准
2. 扫 `/proc/*/environ` 找 `XDG_RUNTIME_DIR == $ROOT/run` 的 Xwayland，读 cmdline
3. 都取不到 → **删掉 `DISPLAY`**，探针连不上、干净失败

**兜底方向是重点**：原来 `or ":0"` 是「探测失败就用默认」，而默认恰恰最危险。
失败要往「连不上」的方向失败，不能往「连上用户桌面」的方向失败。

## 怎么验证隔离真的生效

不能只看断言通过 —— 断言失败可能是功能坏了，也可能是测试打错了地方。
每次改测试，额外确认这两条：

```bash
# 真桌面（你正在用的）上不该有测试窗口
DISPLAY=:0 xdotool search --name "WSTREAM_X11_PROBE" | wc -l    # 期望 0
# 沙箱里应该有
DISPLAY=:1 xdotool search --name "WSTREAM_X11_PROBE" | wc -l    # 期望 1
```

同类的坑：任何碰 `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` / `DISPLAY` 的工具
（`grim`、`wlr-randr`、`wl-copy`、`wl-paste`、`xdotool`、`xwd`）都必须显式指向沙箱。
曾经还发生过：抓截图忘了设 `XDG_RUNTIME_DIR`，抓出了用户的真实桌面。

## 五个容易踩的测试环境问题

**① `SWAYSOCK` 必须显式设置。**
`swaymsg` 靠它找 IPC socket，没设就报 `Unable to retrieve socket path`，
查不到窗口坐标。测试会退化成「点屏幕中心」，然后一路失败。

**② 新窗口会让 sway 重新平铺。**
起一个终端之后，之前探针的位置就变了。每次点击前都要**重新查一次坐标**，
不能缓存（踩过：步骤 5 之后探针从 (1067,360) 挪到了 (800,360)）。

**③ `wl-copy` 会 fork 常驻，不能 `subprocess.run`。**
它要活着持有 selection source（数据源），`run()` 会一直等到超时。用 `Popen`
且不要 `wait()`。

**④ 测试要能在没有 Xwayland 的环境跑。**
CI 的 Arch 容器默认不带 Xwayland，sway 会报
`Cannot find Xwayland binary`，X11 探针就起不来。
探针只是**观测手段**，不是被测功能 —— 起不来就跳过依赖它的断言，
而不是让整个测试失败。真正验证「输入到达应用」的是第 5~7 节，
用的是原生 Wayland 终端（alacritty），不需要 X11。

本地装 `xorg-xwayland` 能跑满 22 项；CI 里装了会跑满，
万一镜像没装也能跑 18 项且不误报。

**⑤ 断言要验「真的落地」，不能只验「没报错」。**
最早的客户端库测试只检查「截图还能取」，结果是假阳性 —— 中文根本没进去
也照样通过。现在一律用「起一个 `cat > 文件` 的终端，读文件内容」来验。

## 中文为什么不对 X11 客户端断言

Unicode 注入的做法是把字符映射到 keycode 9 再按下去。这个 keycode 在
**原生 Wayland 客户端**那边是我们刚推的自定义 keymap（正确）；但
**Xwayland 有自己独立的一套 keymap**，keycode 9 在它那里永远是 Escape。

所以中文只对原生 Wayland 客户端有效（这也是 wstream 的设计前提），
测试改用 alacritty（原生 Wayland）写文件来断言。
