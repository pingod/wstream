#!/usr/bin/env python3
"""wstest.py — wstream 键鼠注入回归测试（CI 用）

设计原则：**断言事件真的到达应用，而不是比对截图**。
截图会因终端每秒刷新而变化，断言不稳定；这里用 X11 探针
（tests/x11probe）把收到的按键打印出来，逐行精确比对。

覆盖：
  1. 来源校验（恶意 Origin/Host 必须 403，正常必须 101）
  2. 鼠标移动 + 点击到达
  3. ASCII 键盘输入到达
  4. 中文（非 ASCII）文本注入到达
  5. 中文之后再打 ASCII 仍然正常（keymap 恢复，这是 wtype 的老毛病）
  6. 多客户端所有权：非持有者的输入必须被丢弃

退出码 0 = 全通过，1 = 有失败。
"""
import base64
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import time

HOST = "127.0.0.1"
PORT = int(os.environ.get("WSTREAM_PORT", "8765"))
ROOT = os.environ.get("WSTREAM_ROOT", "/tmp/wstream")

FAILS = []
PASSES = []


def check(name, cond, detail=""):
    if cond:
        PASSES.append(name)
        print(f"  ✅ {name}")
    else:
        FAILS.append(f"{name} {detail}".strip())
        print(f"  ❌ {name} {detail}")


# ---------------------------------------------------------------- HTTP / WS

def http_req(headers, path="/ws", timeout=5):
    """返回状态行字符串。"""
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = f"GET {path} HTTP/1.1\r\n"
    for k, v in headers.items():
        req += f"{k}: {v}\r\n"
    req += "\r\n"
    s.sendall(req.encode())
    try:
        line = s.recv(200).split(b"\r\n")[0].decode(errors="replace")
    except OSError:
        line = ""
    s.close()
    return line


def ws_connect(origin="http://127.0.0.1:8765"):
    s = socket.create_connection((HOST, PORT), timeout=8)
    req = (f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: "
           + base64.b64encode(os.urandom(16)).decode()
           + "\r\nSec-WebSocket-Version: 13\r\n")
    if origin:
        req += f"Origin: {origin}\r\n"
    req += "\r\n"
    s.sendall(req.encode())
    time.sleep(0.25)
    resp = s.recv(200)
    if b"101" not in resp:
        raise RuntimeError(f"WebSocket 握手失败: {resp!r}")
    return s


def ws_send(s, t, a=0, b=0):
    payload = bytes([t, 0, 0, 0]) + struct.pack("<HH", a & 0xFFFF, b & 0xFFFF)
    _ws_frame(s, payload)


def _ws_frame(s, payload, opcode=0x2):
    n = len(payload)
    m = os.urandom(4)
    if n < 126:
        hdr = bytes([0x80 | opcode, 0x80 | n])
    elif n < 65536:
        hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
    else:
        hdr = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(hdr + m + bytes(c ^ m[i % 4] for i, c in enumerate(payload)))
    time.sleep(0.015)


def ws_text(s, text):
    payload = bytes([5, 0, 0, 0]) + struct.pack("<HH", 0, 0) + text.encode("utf-8")
    _ws_frame(s, payload)


KEY = dict(zip("abcdefghijklmnopqrstuvwxyz",
               [30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49,
                24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44]))
KEY[" "] = 57
KEY["\n"] = 28
KEY["-"] = 12


def type_ascii(s, text):
    for ch in text:
        c = KEY.get(ch)
        if c is None:
            continue
        ws_send(s, 4, c, 1)
        ws_send(s, 4, c, 0)


def move(s, x, y, w, h):
    ws_send(s, 1, int(x / w * 65535), int(y / h * 65535))


# ---------------------------------------------------------------- 环境准备

def sandbox_env():
    with open(os.path.join(ROOT, "wl_name")) as f:
        wl = f.read().strip()
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = os.path.join(ROOT, "run")
    env["WAYLAND_DISPLAY"] = wl
    env["DISPLAY"] = ":0"        # 沙箱内的 XWayland
    env.pop("WAYLAND_SOCKET", None)
    # ⚠️ 必须显式给 SWAYSOCK：sway 不会把它写进我们的环境，
    # 少了它 swaymsg 会报 "Unable to retrieve socket path"，
    # 从而查不到窗口坐标（踩过：测试因此退化成点屏幕中心，全部失败）。
    import glob
    socks = glob.glob(os.path.join(ROOT, "run", "sway-ipc.*"))
    if socks:
        env["SWAYSOCK"] = socks[0]
    return env


def get_geometry_by_appid(env, app_id):
    """按 app_id 查窗口中心坐标。找不到返回 None。"""
    try:
        out = subprocess.run(["swaymsg", "-t", "get_tree"], env=env,
                             capture_output=True, text=True, timeout=5).stdout
        import json
        tree = json.loads(out)

        def walk(node):
            if node.get("app_id") == app_id and "rect" in node:
                r = node["rect"]
                return (r["x"] + r["width"] // 2, r["y"] + r["height"] // 2)
            for c in node.get("nodes", []) + node.get("floating_nodes", []):
                got = walk(c)
                if got:
                    return got
            return None

        return walk(tree)
    except Exception:
        return None


def start_probe():
    """在沙箱里起 X11 探针，等它打印 READY。返回 (进程, 日志路径)。"""
    log = "/tmp/x11probe.log"
    if os.path.exists(log):
        os.unlink(log)
    env = sandbox_env()
    # setsid：脱离当前会话组，避免父 shell 收到信号时连带杀掉
    p = subprocess.Popen(["setsid", "./tests/x11probe"],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT,
                         env=env, start_new_session=True)
    for _ in range(100):
        time.sleep(0.1)
        if os.path.exists(log):
            with open(log) as f:
                if "READY" in f.read():
                    return p, log
        if p.poll() is not None:
            break

    # 起不来时给一个能直接看懂的原因，而不是干巴巴的 RuntimeError
    reason = ""
    try:
        with open(log) as f:
            reason = f.read()[:300]
    except OSError:
        pass
    hasext = os.path.exists("/tmp/wstream/run/.X11-unix/X0") or os.path.exists("/tmp/.X11-unix/X0")
    if not hasext:
        reason += ("\n  → 沙箱里没有 X socket，通常是镜像缺 Xwayland。"
                   "Debian/Arch 装 xorg-xwayland；wstream 不依赖它，"
                   "但这个探针需要。")
    raise RuntimeError(f"X11 探针没能启动：{reason}")


def ws_recv_frame(s, timeout=5):
    """收一个完整的 WS 帧，返回载荷（已去掩码/头）。超时返回 None。"""
    s.settimeout(timeout)
    try:
        hdr = s.recv(2)
        if len(hdr) < 2:
            return None
        n = hdr[1] & 0x7F
        if n == 126:
            ext = s.recv(2)
            n = struct.unpack(">H", ext)[0]
        elif n == 127:
            ext = s.recv(8)
            n = struct.unpack(">Q", ext)[0]
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf
    except (socket.timeout, OSError):
        return None


def probe_output(log):
    try:
        with open(log) as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_for(log, pattern, timeout=6.0):
    """等日志里出现 pattern。返回是否出现。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if re.search(pattern, probe_output(log)):
            return True
        time.sleep(0.1)
    return False


def get_geometry(env):
    """探针窗口在沙箱屏幕上的位置。

    sway 默认平铺，窗口不一定是我们要的坐标。用 swaymsg 查最准；
    查不到就退化成「点屏幕中心」——探针会占据一块可见区域。
    """
    try:
        out = subprocess.run(["swaymsg", "-t", "get_tree"], env=env,
                             capture_output=True, text=True, timeout=5).stdout
        import json
        tree = json.loads(out)

        def walk(node):
            if node.get("name") == "WSTREAM_X11_PROBE" and "rect" in node:
                r = node["rect"]
                return (r["x"] + r["width"] // 2, r["y"] + r["height"] // 2)
            for c in node.get("nodes", []) + node.get("floating_nodes", []):
                got = walk(c)
                if got:
                    return got
            return None

        got = walk(tree)
        if got:
            return got
    except Exception:
        pass
    return None


# ---------------------------------------------------------------- 测试主体

def wait_ready(timeout=30):
    """等 wstream 的 HTTP 端口真的能服务。

    start.sh 起完进程就返回了（内部只 sleep 2s），但 wstream 还要连 Wayland、
    建 shmbuf、初始化虚拟设备。直接跑测试会撞上「连接被拒」或半初始化状态 ——
    表现为随机失败（踩过：同一份代码一次全过、一次挂 4 项）。
    """
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            r = http_req({"Host": f"{HOST}:{PORT}"}, "/", timeout=2)
            if "200" in r:
                return True
            last = r
        except OSError as e:
            last = str(e)
        time.sleep(0.3)
    print(f"  ⚠️ 等 wstream 就绪超时（最后响应: {last}）")
    return False


def main():
    env = sandbox_env()
    W, H = 1280, 720

    print("── 0. 等 wstream 就绪 ──")
    check("wstream 可服务", wait_ready())

    print("── 1. 来源校验 ──")
    K = base64.b64encode(b"abcdefgh12345678").decode()
    FULL = {"Host": f"{HOST}:{PORT}", "Upgrade": "websocket",
            "Connection": "Upgrade", "Sec-WebSocket-Key": K,
            "Sec-WebSocket-Version": "13"}
    for name, hdr in [
        ("恶意 Origin", {**FULL, "Origin": "http://evil.example"}),
        ("恶意 Host", {**FULL, "Host": "evil.example"}),
        ("null Origin", {**FULL, "Origin": "null"}),
    ]:
        r = http_req(hdr)
        check(f"拒绝{name}", "403" in r, f"→ {r}")
    for name, hdr in [
        ("正常 Origin", {**FULL, "Origin": f"http://{HOST}:{PORT}"}),
        ("无 Origin", dict(FULL)),
    ]:
        r = http_req(hdr)
        check(f"放行{name}", "101" in r, f"→ {r}")
    r = http_req({"Host": f"{HOST}:{PORT}"}, "/")
    check("索引页可达", "200" in r, f"→ {r}")

    print("── 2. 启动 X11 探针 ──")
    # ⚠️ X11 探针依赖 Xwayland。CI 镜像里可能没装（踩过：Arch 容器需要
    # xorg-xwayland，否则 sway 报 Cannot find Xwayland binary）。
    # 探针只是「观测手段」，不是被测功能本身 —— 起不来就跳过依赖它的断言，
    # 不要因此把整个测试判失败。真正要看的是注入能不能到达应用，
    # 后面第 5 节用原生 Wayland 终端（alacritty）验证，那条不需要 X11。
    probe = None
    log = "/tmp/x11probe.log"
    try:
        probe, log = start_probe()
        check("探针启动", "READY" in probe_output(log))
        HAVE_PROBE = True
    except RuntimeError as e:
        HAVE_PROBE = False
        print(f"  ⚠️ 跳过 X11 探针相关断言：{e}")
        print("     （这不是 wstream 的问题；探针只是观测手段）")
    pos = get_geometry(env) if HAVE_PROBE else None
    if pos:
        print(f"     探针窗口中心: {pos}")
    else:
        pos = (W // 2, H // 2)
        print(f"     退化用屏幕中心 {pos}")

    try:
        print("── 3. 鼠标注入 ──")
        s = ws_connect()
        ws_send(s, 6, 1)             # take
        time.sleep(0.2)
        ws_send(s, 2, 1, 0)          # 清掉可能的残留按下状态
        move(s, pos[0], pos[1], W, H)
        time.sleep(0.6)
        ws_send(s, 2, 1, 1)          # 左键按下
        ws_send(s, 2, 1, 0)
        if HAVE_PROBE:
            check("鼠标点击到达探针", wait_for(log, r"BUTTON 1"), "(看 /tmp/x11probe.log)")
        else:
            print("     ⏭ 跳过（无 X11 探针）")

        print("── 4. ASCII 键盘 ──")
        type_ascii(s, "abc")
        ws_send(s, 4, 28, 1)
        ws_send(s, 4, 28, 0)
        time.sleep(0.5)
        if HAVE_PROBE:
            out = probe_output(log)
            got_a = "KEY a" in out
            got_b = "KEY b" in out
            got_c = "KEY c" in out
            check("ASCII 按键到达(a/b/c)", got_a and got_b and got_c,
                  f"(a={got_a} b={got_b} c={got_c})")
            check("Enter 到达", "KEY Return" in out)
        else:
            print("     ⏭ 跳过（无 X11 探针；第 5 节会用 Wayland 终端验证键盘）")

        print("── 5. 中文注入（用原生 Wayland 终端断言）──")
        # ⚠️ 为什么不用 X11 探针断言中文：
        #   Unicode 注入靠「把字符映射到 keycode 9 再按下去」。keycode 9 在
        #   Wayland 客户端那边是我们刚推的自定义 keymap（= Unicode keysym，
        #   正确）；但 **Xwayland 有自己独立的一套 keymap**，keycode 9 在它
        #   那里永远是 Escape。实测探针收到的就是 sym=0xff1b(Escape)。
        #   所以中文只对原生 Wayland 客户端有效，这是真实限制，已在
        #   docs 里记录。断言改用原生 Wayland 终端（真实使用场景也更接近）。
        typed = "/tmp/wstream_typed.txt"
        if os.path.exists(typed):
            os.unlink(typed)
        term = subprocess.Popen(
            ["setsid", "env", "-u", "DISPLAY", "alacritty", "--class", "WStreamTest",
             "-e", "sh", "-c", f"cat > {typed}"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        tpos = None
        for _ in range(60):
            time.sleep(0.2)
            tpos = get_geometry_by_appid(env, "WStreamTest")
            if tpos:
                break
        if tpos:
            print(f"     终端窗口中心: {tpos}")
            ws_send(s, 2, 1, 0)          # 先确保没有按下的键
            move(s, tpos[0], tpos[1], W, H)
            time.sleep(0.6)
            ws_send(s, 2, 1, 1)
            time.sleep(0.08)
            ws_send(s, 2, 1, 0)
            time.sleep(0.8)
            type_ascii(s, "ab")          # 先打 ASCII 确认焦点真的在这
            ws_text(s, "中文")
            time.sleep(2.5)
            ws_send(s, 4, 28, 1)         # Enter：让终端把整行交给 cat
            ws_send(s, 4, 28, 0)
            time.sleep(1.5)
            content = ""
            for _ in range(30):
                if os.path.exists(typed):
                    with open(typed, "rb") as f:
                        content = f.read().decode("utf-8", "replace")
                    if "中文" in content:
                        break
                time.sleep(0.2)
            check("中文注入到达终端", "中文" in content,
                  f"(终端文件内容={content!r})")
            check("中文前后 ASCII 都正常", "ab" in content,
                  f"(终端文件内容={content!r})")
        else:
            check("中文注入到达终端", False, "(终端窗口没起来)")
            check("中文前后 ASCII 都正常", False, "(终端窗口没起来)")
        try:
            term.terminate()
        except Exception:
            pass

        print("── 6/6b/7. 用 Wayland 终端验证（不依赖 X11）──")
        # ⚠️ 为什么改到这里用终端而不是 X11 探针：
        #   ① CI 镜像可能没装 Xwayland（踩过：sway 报 Cannot find Xwayland binary）
        #   ② 终端是真实使用场景，验证「文本真的落地」更有意义
        #   ③ 本地和 CI 行为一致，避免「本地测探针、CI 测终端」的偏差
        typed2 = "/tmp/wstream_typed2.txt"
        if os.path.exists(typed2):
            os.unlink(typed2)
        term2 = subprocess.Popen(
            ["setsid", "env", "-u", "DISPLAY", "alacritty", "--class", "WStreamTest2",
             "-e", "sh", "-c", f"cat > {typed2}"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        t2 = None
        for _ in range(60):
            time.sleep(0.2)
            t2 = get_geometry_by_appid(env, "WStreamTest2")
            if t2:
                break
        if t2 and HAVE_PROBE:
            # 有探针时只用它来确认焦点切换能力；文本结论一律看终端文件
            pass

        if t2:
            print(f"     终端窗口中心: {t2}")
            move(s, t2[0], t2[1], W, H)
            time.sleep(0.6)
            ws_send(s, 2, 1, 1)
            time.sleep(0.08)
            ws_send(s, 2, 1, 0)
            time.sleep(0.8)

            def t2_content():
                if not os.path.exists(typed2):
                    return ""
                return open(typed2, "rb").read().decode("utf-8", "replace")

            # 6. 中文之后 ASCII 仍可用（keymap 必须恢复）
            ws_text(s, "中文")
            time.sleep(2.0)
            type_ascii(s, "zz")
            ws_send(s, 4, 28, 1)
            ws_send(s, 4, 28, 0)
            time.sleep(1.5)
            c = t2_content()
            check("中文之后 ASCII 仍正常", "zz" in c, f"(终端={c!r})")
            check("中文之后中文也在", "中文" in c, f"(终端={c!r})")

            # 6b. 修饰键释放：打 "Upper" 必须是 Upper 不是 UPPER
            ws_send(s, 4, 28, 1)
            ws_send(s, 4, 28, 0)
            time.sleep(0.3)
            # U p p e r：U 用 Shift
            ws_send(s, 4, 42, 1); ws_send(s, 4, 22, 1); ws_send(s, 4, 22, 0); ws_send(s, 4, 42, 0)
            time.sleep(0.15)
            for code in (25, 25, 18, 19):   # p p e r
                ws_send(s, 4, code, 1)
                ws_send(s, 4, code, 0)
                time.sleep(0.05)
            ws_send(s, 4, 28, 1)
            ws_send(s, 4, 28, 0)
            time.sleep(1.5)
            c = t2_content()
            check("Shift 大写正确（Upper 不是 UPPER）", "Upper" in c, f"(终端={c!r})")
            check("Shift 释放后不粘连", "UPPER" not in c, f"(终端={c!r})")

            # 7. 多客户端所有权（非持有者的输入必须被丢弃）
            s2 = ws_connect()
            time.sleep(0.3)
            before = len(t2_content())
            type_ascii(s2, "dropped")
            ws_send(s2, 4, 28, 1)
            ws_send(s2, 4, 28, 0)
            time.sleep(1.5)
            after = len(t2_content())
            check("非持有者输入被丢弃", after == before,
                  f"(终端长度 {before} → {after})")

            # 持有者输入应生效
            before = len(t2_content())
            type_ascii(s, "owner")
            ws_send(s, 4, 28, 1)
            ws_send(s, 4, 28, 0)
            time.sleep(1.5)
            after = len(t2_content())
            check("持有者输入仍生效", after > before, f"(终端长度 {before} → {after})")

            # 转移后新持有者可操作
            ws_send(s2, 6, 1)
            time.sleep(0.4)
            before = len(t2_content())
            type_ascii(s2, "newowner")
            ws_send(s2, 4, 28, 1)
            ws_send(s2, 4, 28, 0)
            time.sleep(1.5)
            after = len(t2_content())
            check("转移后新持有者可操作", after > before, f"(终端长度 {before} → {after})")
            s2.close()
        else:
            for nm in ["中文之后 ASCII 仍正常", "中文之后中文也在",
                       "Shift 大写正确（Upper 不是 UPPER）", "Shift 释放后不粘连",
                       "非持有者输入被丢弃", "持有者输入仍生效",
                       "转移后新持有者可操作"]:
                print(f"     ⏭ 跳过 {nm}（终端窗口没起来）")
        try:
            term2.terminate()
        except Exception:
            pass

        print("── 8. 剪贴板双向同步 ──")
        # ⚠️ 这里必须新开一个连接：上面的 s2 在步骤 7 里已经 close 了。
        #    而且剪贴板帧**不受输入权限制**（围观者也能复制），
        #    所以用一个没取权的连接正好顺带验证这一点。
        s2 = ws_connect()
        time.sleep(0.2)
        # 8a. 沙箱 → 浏览器：先在沙箱里写剪贴板，再让页面读
        typed_cb = "/tmp/wstream_cb_src.txt"
        with open(typed_cb, "wb") as f:
            f.write("沙箱剪贴板内容 abc".encode())
        # ⚠️ 用 Popen 不用 run：wl-copy 默认 fork 到后台常驻来持有 selection
        # source，run() 会一直等它退出 → 撞 timeout（踩过）。
        # 必须让它活着，否则数据源消失、这边读到的就是空。
        cbproc = subprocess.Popen(["wl-copy"], stdin=open(typed_cb, "rb"),
                                  env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        # 剪贴板帧不受输入权限制（围观者也能复制），用 s2 验证这一点
        # 先发 type 8 请求
        ws_send(s2, 8)
        got = ws_recv_frame(s2, timeout=6)
        if got and len(got) >= 10 and got[0] == 0x81:
            text = got[8:].decode("utf-8", "replace")
            check("沙箱剪贴板能被页面读取", "沙箱剪贴板内容" in text, f"(读到={text!r})")
        else:
            check("沙箱剪贴板能被页面读取", False, f"(响应={got!r})")

        # 8b. 浏览器 → 沙箱：页面推文本，沙箱 wl-paste 应该读到
        payload = bytes([7, 0, 0, 0]) + struct.pack("<HH", 0, 0) + \
                  "页面推来的内容 123".encode()
        _ws_frame(s2, payload)
        time.sleep(1.5)
        pasted = subprocess.run(["wl-paste"], env=env, capture_output=True,
                                timeout=10).stdout.decode("utf-8", "replace")
        check("页面能把文本写进沙箱剪贴板", "页面推来的内容" in pasted,
              f"(wl-paste 读到={pasted!r})")

        s2.close()
        try:
            cbproc.terminate()
        except Exception:
            pass
    finally:
        try:
            probe.terminate()
        except Exception:
            pass

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
