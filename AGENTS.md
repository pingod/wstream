# 给 AI agent 的工作约定

这个仓库是我（用户）和 AI agent 一起维护的。下面几条是踩过之后定下来的，
请照做，别重新发明。

## 本地是主关口，远程 CI 只用来发版

这个项目很轻，本地全套测试两分钟跑完。**以本地为准**：

| 时机 | 谁跑 |
|---|---|
| 改完代码 | **本地跑全套**（`tests/wstest.py` + `tests/lifecycle.py`），绿了再提交 |
| 日常提交 / 推送 | 不用管远程 CI |
| **准备发版**（打 tag / 建 release） | 推送后看**远程 CI** 结果，绿了才发 |

远程 CI 的唯一价值是验证**另一套环境**（裸容器、没有用户桌面、依赖自己装）——
那个差异只在发版时才值得确认。平时为个文档改动去等 CI 是浪费。

但**发版前必须看远程 CI**：1.3.x 六个版本里 CI 一直红着，就是因为只看本地。
**发版是唯一「必须两边都绿」的时刻。**

不要做的事：本地跑完 → 推送 → 等 CI 也跑完。同一份断言跑两次，第二次没有新信息。

参考 `tests/README.md`。CI 定义在 `.github/workflows/build.yml`。

## 推送要带代理

直连 GitHub 会超时：

```bash
git -c http.proxy=http://127.0.0.1:9565 push origin main
git -c http.proxy=http://127.0.0.1:9565 push origin vX.Y.Z
```

`-c` 必须放在子命令**前面**（`git push -c ...` 是参数错误）。
不要改全局 gitconfig —— 会影响用户其它仓库的路由。

`gh` 需要缓存目录可写，否则报错：

```bash
XDG_CACHE_HOME=$PWD/.ghcache gh release create ...
```

## 别杀掉自己

`pkill -f <关键字>` 会匹配到**你自己的 shell 命令行**，把自己 SIGTERM 掉。
这个坑我已经犯过好几次了（见 README「已知坑」、SKILL.md）。

要用 `pkill` 就加 `-x`（精确匹配进程名，不匹配整条 cmdline）。
清 wstream 实例用 `./scripts/stop-all.sh`，不要自己拼 `pkill`。

## 绝对不要碰用户真实桌面

这是本项目发生过的最严重事故（`tests/README.md` 有完整复盘）：
测试把 `DISPLAY` 硬编码成 `":0"`，结果窗口开到了用户屏幕上，后续所有点击和
按键都打进了用户的真实会话。

- 任何 `DISPLAY` / `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` 都不能硬编码，要探测
- 探测失败要往「连不上」的方向失败，**不能往「连上用户桌面」的方向失败**
- 别按进程名裸杀 `sway`（用户桌面也在用），只认 `XDG_RUNTIME_DIR` 归属

## 提交前

- `./build.sh` 零警告（`-Wall -Wextra` 全开）
- 文档要和代码一起改：`CHANGELOG.md`、`docs/HANDOFF.md`（踩过的坑都记在
  第 3 节，编号连续）、必要时 `README.md` 和 `skills/wstream/SKILL.md`
- 改了 `skills/wstream/SKILL.md` 要同步到 `~/.agents/skills/wstream/SKILL.md`

## 这个项目的核心约束

wstream 的价值是**与用户真实桌面物理隔离、不抢焦点**。任何改动如果让
「沙箱里的操作可能泄漏到用户桌面」，那它再方便也是错的。

参考 `docs/ARCHITECTURE.md` 的设计取舍，尤其「为什么不用 X11」「为什么
用 `zwlr_data_control` 而不是 `wl_data_device`」这类决策的原因。
