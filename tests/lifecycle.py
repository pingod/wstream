#!/usr/bin/env python3
"""lifecycle.py — 实例生命周期回归测试。

断言的是「起停之后有没有留下垃圾」，而不是功能本身。这一类 bug 特别阴险：
功能完全正常，只是每跑一次就漏一批进程，几小时后机器变慢、端口被占，
而没人知道是 wstream 干的。

覆盖的坑（见 docs/HANDOFF.md 第 3 节）：
  坑 25  own_pids 漏了 desktop.sh → 每轮 start/stop 漏一批进程
  坑 26  ls run/wayland-* 判「沙箱已在运行」→ 进程死了就永久卡死
  坑 27  实例目录被删后，孤儿进程永远清不掉

用法：
    ./build.sh
    python3 tests/lifecycle.py

⚠️ 这个测试会**真的起停实例**，但只用临时目录（/tmp/wstream-lc-*），
   不会碰你的默认实例（/tmp/wstream），也不会碰任何真实桌面进程。
   所有进程归属判据都是 XDG_RUNTIME_DIR，不按名字裸杀。
"""
import os
import shutil
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "wstream")
SANDBOX_APPS = ["wstream", "sway-nocap", "sway", "desktop.sh", "alacritty", "sh", "sleep"]

PASSES = []
FAILS = []


def check(name, cond, detail=""):
    (PASSES if cond else FAILS).append(name)
    print(f"  {'✅' if cond else '❌'} {name}{(' ' + detail) if detail and not cond else ''}")


def pids_of(root):
    """属于某个实例的进程 pid 列表（判据：XDG_RUNTIME_DIR == <root>/run）。"""
    want = os.path.join(root, "run")
    out = []
    for name in SANDBOX_APPS:
        try:
            raw = subprocess.run(["pgrep", "-x", name],
                                 capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        for tok in raw.split():
            if not tok.isdigit():
                continue
            pid = tok
            try:
                with open(f"/proc/{pid}/environ", "rb") as f:
                    envs = f.read().decode("utf-8", "replace").split("\0")
                rt = [e for e in envs if e.startswith("XDG_RUNTIME_DIR=")]
                if rt and rt[0].split("=", 1)[1] == want:
                    out.append((name, pid))
            except OSError:
                continue
    return out


def run_start(root, port, size="1024x600", timeout=120, app=None):
    env = dict(os.environ, WSTREAM_ROOT=root, WSTREAM_PORT=str(port))
    cmd = [BIN, "start", size]
    if app:
        cmd += ["--app", app]
    return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)


def run_stop(root, timeout=60):
    env = dict(os.environ, WSTREAM_ROOT=root)
    return subprocess.run([BIN, "stop"], env=env, capture_output=True,
                          text=True, timeout=timeout)


def runtime_pids(root):
    """XDG_RUNTIME_DIR == root/run 的所有非僵尸 pid（不限进程名）。
    比 pids_of 的白名单更全：能抓到 --app 启动的任意程序。"""
    want = os.path.join(root, "run")
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                envs = f.read().decode("utf-8", "replace").split("\0")
            rt = [e for e in envs if e.startswith("XDG_RUNTIME_DIR=")]
            if not rt or rt[0].split("=", 1)[1] != want:
                continue
            with open(f"/proc/{pid}/stat") as f:
                if f.read().split()[2] == "Z":
                    continue
            out.append(pid)
        except OSError:
            continue
    return out


def port_http(port):
    import urllib.request
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=5) as r:
            return r.status
    except Exception:
        return None


def _port_of_instance(root):
    """该实例（root）里 wstream 实际监听的端口。读不到返回 None。"""
    for pid in runtime_pids(root):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv = f.read().decode("utf-8", "replace").split("\0")
        except OSError:
            continue
        if not argv or "wstream" not in os.path.basename(argv[0]):
            continue
        for i, a in enumerate(argv):
            if a == "-p" and i + 1 < len(argv) and argv[i + 1].isdigit():
                return int(argv[i + 1])
    return None


def sandbox_windows(port):
    """沙箱窗口列表（GET /windows）。失败返回 None。"""
    import json
    import urllib.request
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/windows", timeout=5) as r:
            return json.loads(r.read().decode("utf-8")).get("windows", [])
    except Exception:
        return None


def main():
    if not os.access(os.path.join(REPO, "wstream"), os.X_OK):
        print("先编译：./build.sh")
        return 1

    root = "/tmp/wstream-lc-test"
    port = 8834
    # 进来第一件事：记下默认实例位是否已被占用（很可能是用户正在看的 8765）。
    # 必须在这里采样 —— 后面的用例会把 8765 停掉，那时再判断就晚了。
    default_busy_at_start = port_http(8765) == 200
    print("── 0. 清理上次残留 ──")
    run_stop(root)
    shutil.rmtree(root, ignore_errors=True)

    try:
        # ---------- 1. 起停一轮，必须零残留 ----------
        print("── 1. 起停一轮后不应留下任何进程 ──")
        r = run_start(root, port)
        check("实例能起来", r.returncode == 0, r.stderr[-300:])
        check("端口可服务", port_http(port) == 200)
        # CI 裸容器踩到的坑：sway 同时建 wayland-N 与 wayland-N.lock，readdir 无序
        # 时可能把 .lock 当 socket 写进 wl_name → 连不上。必须指向真 socket。
        import stat as _stat
        wl_name = open(os.path.join(root, "wl_name")).read().strip()
        wl_path = os.path.join(root, "run", wl_name)
        check("wl_name 指向真 socket（非 .lock）",
              not wl_name.endswith(".lock") and os.path.exists(wl_path)
              and _stat.S_ISSOCK(os.stat(wl_path).st_mode),
              f"(wl_name={wl_name})")

        during = pids_of(root)
        names = {n for n, _ in during}
        check("运行时能看到 wstream", "wstream" in names, f"(实际={names})")
        # 默认实例**不再自动开窗口**（命令行/时钟改由浏览器按钮触发），
        # 所以既不该有 desktop.sh，也不该有任何 alacritty。
        check("默认实例不起 desktop.sh", "desktop.sh" not in names, f"(实际={names})")
        check("默认实例没有自动窗口", "alacritty" not in names, f"(实际={names})")
        wins = sandbox_windows(port)
        check("默认沙箱启动后是空桌面", wins == [], f"(windows={wins})")

        r = run_stop(root)
        check("stop 成功返回", r.returncode == 0, r.stderr[-300:])
        time.sleep(2)
        left = pids_of(root)
        check("stop 后零残留", len(left) == 0,
              f"(残留={left})")

        # 逐个名字断言，失败时能直接看出是谁漏了
        for name in SANDBOX_APPS:
            n = sum(1 for x, _ in left if x == name)
            if n:
                check(f"  {name} 已清理", False, f"({n} 个残留)")

        # ---------- 2. 残留 socket 必须被识别并重建（坑 26）----------
        print("── 2. 死 socket 不应导致永久卡死 ──")
        os.makedirs(os.path.join(root, "run"), exist_ok=True)
        with open(os.path.join(root, "run", "wayland-99"), "w") as f:
            f.write("")                     # 一个没人监听的假 socket 文件
        with open(os.path.join(root, "wl_name"), "w") as f:
            f.write("wayland-99\n")
        r = run_start(root, port)
        check("遇到死 socket 仍能起来（不是卡死）", r.returncode == 0,
              r.stderr[-300:])
        check("死 socket 后端口可服务", port_http(port) == 200)
        check("日志说明了重建原因", "残留" in (r.stdout + r.stderr),
              f"(输出={(r.stdout + r.stderr)[-300:]!r})")
        run_stop(root)
        time.sleep(2)

        # ---------- 3. 正常复用路径没被改坏 ----------
        print("── 3. 活着时再 start 应复用而不是重建 ──")
        r = run_start(root, port)
        check("首次启动成功", r.returncode == 0, r.stderr[-300:])
        r2 = run_start(root, port)
        out2 = r2.stdout + r2.stderr
        check("二次 start 成功返回", r2.returncode == 0, out2[-300:])
        check("二次 start 判定为复用", "复用" in out2, f"(输出={out2[-300:]!r})")
        run_stop(root)
        time.sleep(2)

    finally:
        # ---------- 4. stop-all 必须能清理（坑 27）----------
        print("── 4. stop-all 能清掉本测试的实例 ──")
        env = dict(os.environ, WSTREAM_ROOT=root)
        subprocess.run([BIN, "start", "1024x600"], env=env, capture_output=True, timeout=120)
        before = pids_of(root)
        check("stop-all 前有进程", len(before) > 0, f"(实际={before})")

        # 故意删掉实例目录，模拟「目录没了但进程还在」——坑 27
        shutil.rmtree(root, ignore_errors=True)
        # ⚠️ 不能用 --all：那会连带停掉用户正在看的默认实例。
        # 目录虽被删，但进程的 XDG_RUNTIME_DIR 仍指向 root，按 root 过滤能找到。
        r = subprocess.run([BIN, "stop-all", "--yes"],
                           env=dict(os.environ, WSTREAM_ROOT=root),
                           capture_output=True, text=True, timeout=120)
        check("stop-all 成功返回", r.returncode == 0, r.stderr[-300:])
        time.sleep(2)

        still = []
        for name, pid in before:
            try:
                os.kill(int(pid), 0)
                # 僵尸不算活着
                with open(f"/proc/{pid}/stat") as f:
                    if f.read().split()[2] != "Z":
                        still.append((name, pid))
            except OSError:
                pass
        check("目录被删后孤儿仍被清掉", len(still) == 0, f"(残留={still})")

        # 兜底清理，别把垃圾留给用户
        shutil.rmtree(root, ignore_errors=True)

    # ---------- 5. --app 打开指定程序，stop 按归属清理 ----------
    print("── 5. --app 打开指定程序且 stop 按归属清理 ──")
    aroot = root + "-app"
    aport = port + 1
    run_stop(aroot)
    shutil.rmtree(aroot, ignore_errors=True)
    r = run_start(aroot, aport, app="tail -f /dev/null")
    check("--app 实例能起来", r.returncode == 0, r.stderr[-300:])

    def _comm(pid):
        try:
            return open(f"/proc/{pid}/comm").read().strip()
        except OSError:
            return ""
    # tail 不在任何白名单里，验证它确实在沙箱内被启动
    has_tail = any(_comm(p) == "tail" for p in runtime_pids(aroot))
    check("--app 程序在沙箱内启动", has_tail, f"(runtime_pids={runtime_pids(aroot)})")
    run_stop(aroot)
    time.sleep(1)
    left = runtime_pids(aroot)
    check("--app stop 零残留（按归属，不限名）", len(left) == 0, f"(残留={left})")
    shutil.rmtree(aroot, ignore_errors=True)

    # ---------- 5b. 浏览器按钮按需开窗口（WS type 12）----------
    # 沙箱启动后是空桌面，窗口由页面按钮触发。这里验证：能开、不重复开、
    # 进程在沙箱里（不带 DISPLAY）、以及 stop 能按归属把它们清干净。
    print("── 5b. 按需开窗口 + stop 按归属清理 ──")
    wroot = root + "-win"
    wport = port + 2
    run_stop(wroot)
    shutil.rmtree(wroot, ignore_errors=True)
    r = run_start(wroot, wport)
    check("按需窗口实例能起来", r.returncode == 0, r.stderr[-300:])
    check("起完是空桌面", sandbox_windows(wport) == [],
          f"(windows={sandbox_windows(wport)})")

    sys.path.insert(0, os.path.join(REPO, "clients"))
    launched = False
    try:
        import wstream_client
        with wstream_client.WStream(port=wport) as ws:
            ws.wait_ready()
            check("type 12 能拉起命令行窗口", ws.open_window(1),
                  f"(windows={sandbox_windows(wport)})")
            launched = bool(ws.window(app_id="AgentTop"))
            # 再点一次：不该出现第二扇
            ws.open_window(1, wait=False)
            time.sleep(3)
            n_top = sum(1 for w in (sandbox_windows(wport) or [])
                        if w.get("app_id") == "AgentTop")
            check("重复点按钮不出第二扇窗", n_top == 1, f"(AgentTop 数量={n_top})")
    except Exception as e:
        check("type 12 能拉起命令行窗口", False, f"(异常={e})")
        check("重复点按钮不出第二扇窗", False, f"(异常={e})")

    if launched:
        # 隔离红线（坑 5）：沙箱里起的窗口不能带 DISPLAY，否则会连用户真实 X11。
        leaks = []
        for pid in runtime_pids(wroot):
            try:
                with open(f"/proc/{pid}/environ", "rb") as f:
                    envs = f.read().decode("utf-8", "replace").split("\0")
            except OSError:
                continue
            if any(e.startswith("DISPLAY=") for e in envs):
                try:
                    comm = open(f"/proc/{pid}/comm").read().strip()
                except OSError:
                    comm = "?"
                leaks.append(f"{comm}:{pid}")
        check("按需窗口不继承 DISPLAY", not leaks, f"(泄漏={leaks})")

    run_stop(wroot)
    time.sleep(1)
    left = runtime_pids(wroot)
    check("按需窗口 stop 零残留", len(left) == 0, f"(残留={left})")
    shutil.rmtree(wroot, ignore_errors=True)

    # ---------- 5c. 端口冲突必须失败，不能静默挂到别人的实例上 ----------
    # 真实踩到过：两个 agent 各设一个 WSTREAM_ROOT、都没设端口，
    # 第二个实例的 wstream 因 EADDRINUSE 退出，而第一个实例照常应答 8765，
    # 于是 start 报告「成功」，两边共用同一个沙箱、互相抢输入权。
    print("── 5c. 端口被别的实例占着必须报错 ──")
    a_root = root + "-coll-a"
    b_root = root + "-coll-b"
    cport = port + 3
    for rr in (a_root, b_root):
        run_stop(rr)
        shutil.rmtree(rr, ignore_errors=True)
    r = run_start(a_root, cport)
    check("占用端口的实例能起来", r.returncode == 0, r.stderr[-300:])
    # 另一个 root 显式要同一个端口 → 必须失败
    r = run_start(b_root, cport)
    out = r.stdout + r.stderr
    check("端口冲突时 start 失败（非 0 退出）", r.returncode != 0, f"(rc={r.returncode})")
    check("端口冲突有明确提示", "已被另一个 wstream 实例占用" in out, f"(输出={out[-200:]!r})")
    check("端口冲突不留下沙箱进程", len(runtime_pids(b_root)) == 0,
          f"(残留={runtime_pids(b_root)})")
    check("冲突后原实例仍在线", port_http(cport) == 200)
    run_stop(a_root)
    time.sleep(1)
    shutil.rmtree(a_root, ignore_errors=True)
    shutil.rmtree(b_root, ignore_errors=True)

    # ---------- 5d. 只给 root 不给端口：不能撞别人的端口 ----------
    print("── 5d. 只给 WSTREAM_ROOT 时自动避开被占用的端口 ──")
    d_root = root + "-auto-a"
    e_root = root + "-auto-b"
    run_stop(d_root)
    run_stop(e_root)
    shutil.rmtree(d_root, ignore_errors=True)
    shutil.rmtree(e_root, ignore_errors=True)
    env = dict(os.environ, WSTREAM_ROOT=d_root)
    env.pop("WSTREAM_PORT", None)
    r1 = subprocess.run([BIN, "start", "1024x600"], env=env,
                        capture_output=True, text=True, timeout=120)
    check("首个实例（隐式端口）能起来", r1.returncode == 0, r1.stderr[-300:])
    port1 = _port_of_instance(d_root)
    env2 = dict(os.environ, WSTREAM_ROOT=e_root)
    env2.pop("WSTREAM_PORT", None)
    r2 = subprocess.run([BIN, "start", "1024x600"], env=env2,
                        capture_output=True, text=True, timeout=120)
    check("第二个实例（隐式端口）能起来", r2.returncode == 0, r2.stderr[-300:])
    port2 = _port_of_instance(e_root)
    check("两个实例端口不同", port1 and port2 and port1 != port2,
          f"(port1={port1} port2={port2})")
    check("两个实例都在线", port_http(port1) == 200 and port_http(port2) == 200,
          f"(port1={port1} port2={port2})")
    for rr in (d_root, e_root):
        run_stop(rr)
        shutil.rmtree(rr, ignore_errors=True)

    # ---------- 5e. 继承来的 SWAYSOCK 不能影响新沙箱 ----------
    # 多 agent 场景里，前一个实例的 shell 往往 export 过 SWAYSOCK。sway 会用它
    # 绑定自己的 IPC socket，于是新沙箱报 "Unable to bind IPC socket" 起不来，
    # 表现为「连不上 Wayland」。spawn_sway 现在显式 unsetenv。
    print("── 5e. 继承的 SWAYSOCK 不影响新沙箱 ──")
    f_root = root + "-swaysock"
    fport = port + 4
    run_stop(f_root)
    shutil.rmtree(f_root, ignore_errors=True)
    fenv = dict(os.environ, WSTREAM_ROOT=f_root, WSTREAM_PORT=str(fport),
                SWAYSOCK="/tmp/definitely-not-this-instance/sway-ipc.1.2.sock")
    r = subprocess.run([BIN, "start", "1024x600"], env=fenv,
                       capture_output=True, text=True, timeout=120)
    check("继承坏 SWAYSOCK 时实例仍能起来", r.returncode == 0,
          (r.stdout + r.stderr)[-300:])
    check("该实例端口可服务", port_http(fport) == 200)
    run_stop(f_root)
    time.sleep(1)
    check("该实例 stop 零残留", len(runtime_pids(f_root)) == 0,
          f"(残留={runtime_pids(f_root)})")
    shutil.rmtree(f_root, ignore_errors=True)

    # ---------- 6. 多实例：裸 start 重复运行自动新建 ----------
    # ⚠️ 自动新建只在「默认根 /tmp/wstream + 8765」上发生，无法用临时 root 隔离。
    #    若用户此刻正在看 8765（非测试进程占用），跳过本节 —— 不能为了测试把
    #    用户正在用的实例停掉。CI 里没有用户实例，本节照常跑。
    print("── 6. 多实例自动分配（裸 start 重复运行）──")
    skip6 = default_busy_at_start and not os.environ.get("WSTREAM_TEST_TAKE_8765")
    if skip6:
        print("     ⏭ 跳过（8765 上有实例在服务，可能是用户正在看的；"
              "要强制跑设 WSTREAM_TEST_TAKE_8765=1）")
    if not skip6:
        # 到这里 8765 是空闲的（否则上面已跳过），可以安全占用默认实例位。
        # 自动分配只在「默认根 + 8765」触发，所以本节必须用默认根才测得到。
        menv = {k: v for k, v in os.environ.items()
                if k not in ("WSTREAM_ROOT", "WSTREAM_PORT")}
        r1 = subprocess.run([BIN, "start", "1024x600"], env=menv,
                            capture_output=True, text=True, timeout=120)
        check("多实例: 第1个实例起来", r1.returncode == 0, r1.stderr[-200:])
        r2 = subprocess.run([BIN, "start", "1024x600"], env=menv,
                            capture_output=True, text=True, timeout=120)
        check("多实例: 第2个自动新建", r2.returncode == 0 and "自动新建" in (r2.stdout + r2.stderr),
              f"(输出={(r2.stdout + r2.stderr)[-200:]})")
        ports = [8765, 8766]
        online = [p for p in ports if port_http(p) == 200]
        check("多实例: 两个实例都在线", len(online) == 2, f"(在线={online})")
        subprocess.run([BIN, "stop-all", "--yes", "--all"], capture_output=True, timeout=120)
        time.sleep(1)
        check("多实例: stop-all 后本组清空",
              all(port_http(p) is None for p in ports), f"(仍在线={[p for p in ports if port_http(p)]})")
        shutil.rmtree("/tmp/wstream", ignore_errors=True)
        shutil.rmtree("/tmp/wstream-2", ignore_errors=True)

    print()
    print(f"通过 {len(PASSES)} 项，失败 {len(FAILS)} 项")
    if FAILS:
        for f in FAILS:
            print(f"  失败：{f}")
        return 1
    print("✅ 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
