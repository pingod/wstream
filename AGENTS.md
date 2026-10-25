# 给 AI agent 的工作约定

这个仓库是我（用户）和 AI agent 一起维护的。下面几条是踩过之后定下来的，
请照做，别重新发明。

## 测试只跑一遍

**不要「本地跑一遍 → 推送 → 等 CI 再跑一遍」。** 同一份代码、同一套断言跑两次，
多花的时间什么都买不到。

| 情况 | 谁跑 |
|---|---|
| 改完代码要提交 | **CI 跑**。推上去，读 CI 结果 |
| 需要断点 / ASan / 看中间状态 | 本地跑 —— CI 做不到这些 |
| CI 红了要复现 | 本地跑 —— 为了迭代快 |
| 真没网 / CI 挂了 | 本地跑，提交信息里注明「CI 未验证」 |

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
