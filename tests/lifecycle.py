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
START = os.path.join(REPO, "scripts", "start.sh")
STOP_ALL = os.path.join(REPO, "scripts", "stop-all.sh")
SANDBOX_APPS = ["wstream", "sway-nocap", "sway", "desktop.sh", "alacritty"]

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


def run_start(root, port, size="1024x600", timeout=120):
    env = dict(os.environ, WSTREAM_ROOT=root, WSTREAM_PORT=str(port))
    return subprocess.run([START, size], env=env, capture_output=True,
                          text=True, timeout=timeout)


def run_stop(root, timeout=60):
    env = dict(os.environ, WSTREAM_ROOT=root)
    return subprocess.run([START, "stop"], env=env, capture_output=True,
                          text=True, timeout=timeout)


def port_http(port):
    import urllib.request
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=5) as r:
            return r.status
    except Exception:
        return None


def main():
    if not os.access(os.path.join(REPO, "wstream"), os.X_OK):
        print("先编译：./build.sh")
        return 1

    root = "/tmp/wstream-lc-test"
    port = 8834
    print("── 0. 清理上次残留 ──")
    run_stop(root)
    shutil.rmtree(root, ignore_errors=True)

    try:
        # ---------- 1. 起停一轮，必须零残留 ----------
        print("── 1. 起停一轮后不应留下任何进程 ──")
        r = run_start(root, port)
        check("实例能起来", r.returncode == 0, r.stderr[-300:])
        check("端口可服务", port_http(port) == 200)

        during = pids_of(root)
        names = {n for n, _ in during}
        check("运行时能看到 wstream", "wstream" in names, f"(实际={names})")
        # 坑 25：desktop.sh 必须被 own_pids 认出来，否则 stop 清不掉
        check("运行时能看到 desktop.sh", "desktop.sh" in names, f"(实际={names})")

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
        print("── 4. stop-all.sh 能清掉本测试的实例 ──")
        env = dict(os.environ, WSTREAM_ROOT=root)
        subprocess.run([START, "1024x600"], env=env, capture_output=True, timeout=120)
        before = pids_of(root)
        check("stop-all 前有进程", len(before) > 0, f"(实际={before})")

        # 故意删掉实例目录，模拟「目录没了但进程还在」——坑 27
        shutil.rmtree(root, ignore_errors=True)
        r = subprocess.run([STOP_ALL, "--yes"], capture_output=True, text=True,
                           timeout=120)
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
