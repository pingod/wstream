#!/usr/bin/env python3
"""wstream_client — wstream 的 Python 客户端（零依赖，只用标准库）

给 AI agent 用：不用再手工复制 SKILL.md 里的代码片段。

    from wstream_client import WStream
    with WStream() as ws:
        ws.take()                        # 取得操作权
        print(ws.windows())              # 有哪些窗口、各在哪
        ws.click_window("AgentTop")      # 点窗口中心
        ws.type_text("echo hello")       # 打字（ASCII）
        ws.paste("中文也能打")            # 非 ASCII 走剪贴板
        ws.snapshot("shot.jpg")          # 存一张实时截图

设计要点（都是踩过的坑，改动前先看 docs/HANDOFF.md）：
  * 上行帧固定 8 字节头，坐标是 uint16 归一化（0..65535）
  * 按钮码服务端会翻译 1/2/3 → BTN_LEFT 等，客户端发习惯编号即可
  * 必须 take() 才有输入权，否则服务端丢弃所有输入
  * 非 ASCII 逐键到不了 XWayland，必须走剪贴板（paste() 已处理）
"""

import base64
import json
import os
import socket
import struct
import time
import urllib.request

__all__ = ["WStream", "WStreamError"]

# 上行 type（与 src/wstream.c 的 handle_msg 对应）
T_MOVE, T_BUTTON, T_SCROLL, T_KEY, T_TEXT, T_CTRL, T_CLIP_SET, T_CLIP_GET = 1, 2, 3, 4, 5, 6, 7, 8
# 下行 type
D_CLIPBOARD = 0x81

# Linux evdev 扫描码（服务端直接收这个，**不要 +8**）
KEY = dict(zip("abcdefghijklmnopqrstuvwxyz",
               [30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49,
                24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44]))
KEY.update(dict(zip("1234567890", [2, 3, 4, 5, 6, 7, 8, 9, 10, 11])))
KEY.update({
    " ": 57, "\n": 28, "\t": 15, "-": 12, "=": 13, "[": 26, "]": 27,
    "\\": 43, ";": 39, "'": 40, ",": 51, ".": 52, "/": 53, "`": 41,
})
SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
           "&": "7", "*": "8", "(": "9", ")": "0", "_": "-", "+": "=",
           "{": "[", "}": "]", "|": "\\", ":": ";", '"': "'", "<": ",",
           ">": ".", "?": "/", "~": "`"}
MOD = {"shift": 42, "ctrl": 29, "alt": 56, "super": 125}


class WStreamError(RuntimeError):
    pass


class WStream:
    """一个 wstream 会话。

    参数：
      host/port   wstream 监听地址（默认 127.0.0.1:8765）
      width/height 沙箱分辨率，用于坐标换算；不确定就用默认值，
                  move_px() 会自动从 /windows 推断
    """

    def __init__(self, host="127.0.0.1", port=8765, width=1280, height=720,
                 timeout=8.0):
        self.host = host
        self.port = port
        self.width = width
        self.height = height
        self.timeout = timeout
        self._sock = None
        self._recvbuf = b""
        self._owns = False

    # ---------------------------------------------------------------- 连接

    def connect(self):
        """建立 WebSocket 连接（含 Origin 校验所需的头部）。"""
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /ws HTTP/1.1\r\nHost: {self.host}:{self.port}\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Origin: http://{self.host}:{self.port}\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        s.sendall(req.encode())
        resp = s.recv(1024)
        if b"101" not in resp:
            s.close()
            raise WStreamError(f"WebSocket 握手失败：{resp[:120]!r}")
        self._sock = s
        self._owns = True
        return self

    def close(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        self._owns = False

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # ---------------------------------------------------------------- 底层帧

    def _require(self):
        if not self._sock:
            raise WStreamError("还没连接，先调用 connect() 或用 with 语句")

    def _send(self, payload):
        """发一个二进制帧（客户端必须加掩码，RFC 6455）。"""
        self._require()
        n = len(payload)
        mask = os.urandom(4)
        if n < 126:
            hdr = bytes([0x82, 0x80 | n])
        elif n < 65536:
            hdr = bytes([0x82, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x82, 0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        self._sock.sendall(hdr + mask + masked)

    def _cmd(self, t, a=0, b=0):
        """8 字节定长帧。A/B 都用无符号打包 —— 坐标 > 32767 时用 'h' 会变负数。"""
        self._send(bytes([t, 0, 0, 0]) + struct.pack("<HH", a & 0xFFFF, b & 0xFFFF))

    def _recv_frame(self, timeout=None):
        """收一帧，返回载荷（bytes）或 None（超时）。"""
        self._require()
        self._sock.settimeout(timeout if timeout is not None else self.timeout)
        try:
            while True:
                # 先看缓冲区里有没有完整帧
                if len(self._recvbuf) >= 2:
                    b0, b1 = self._recvbuf[0], self._recvbuf[1]
                    n = b1 & 0x7F
                    off = 2
                    if n == 126:
                        if len(self._recvbuf) < 4:
                            pass
                        else:
                            n = struct.unpack(">H", self._recvbuf[2:4])[0]
                            off = 4
                    elif n == 127:
                        if len(self._recvbuf) < 10:
                            pass
                        else:
                            n = struct.unpack(">Q", self._recvbuf[2:10])[0]
                            off = 10
                    if off == 2 or len(self._recvbuf) >= off:
                        if len(self._recvbuf) >= off + n:
                            payload = self._recvbuf[off:off + n]
                            self._recvbuf = self._recvbuf[off + n:]
                            return payload
                chunk = self._sock.recv(65536)
                if not chunk:
                    return None
                self._recvbuf += chunk
        except (socket.timeout, OSError):
            return None

    # ---------------------------------------------------------------- HTTP

    def _http(self, path, timeout=8):
        url = f"http://{self.host}:{self.port}{path}"
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()

    def snapshot(self, path=None):
        """取一张实时截图（JPEG）。给 path 就存盘并返回路径，否则返回 bytes。

        不需要订阅 MJPEG、也不需要重启 wstream。
        """
        data = self._http("/snapshot.jpg")
        if path:
            with open(path, "wb") as f:
                f.write(data)
            return path
        return data

    def windows(self):
        """窗口列表：[{app_id, title, x, y, width, height, cx, cy}, ...]

        服务端从 sway IPC 读真实坐标，比截图猜位置可靠得多。
        """
        data = self._http("/windows")
        return json.loads(data.decode("utf-8")).get("windows", [])

    def window(self, app_id=None, title_contains=None):
        """按 app_id 或标题片段找窗口；找不到返回 None。"""
        for w in self.windows():
            if app_id is not None and w.get("app_id") == app_id:
                return w
            if title_contains and title_contains in (w.get("title") or ""):
                return w
        return None

    # ---------------------------------------------------------------- 控制

    def take(self):
        """取得输入权。**不调这个，后面所有输入都会被服务端丢弃。**"""
        self._cmd(T_CTRL, 1, 0)
        time.sleep(0.15)

    def release(self):
        """释放输入权，让别人也能操作。"""
        self._cmd(T_CTRL, 0, 0)

    # ---------------------------------------------------------------- 鼠标

    def move(self, x, y):
        """按像素坐标移动（用当前 width/height 换算成归一化值）。"""
        nx = int(max(0, min(1, x / max(1, self.width))) * 65535)
        ny = int(max(0, min(1, y / max(1, self.height))) * 65535)
        self._cmd(T_MOVE, nx, ny)
        time.sleep(0.05)

    def click(self, x=None, y=None, button=1, double=False):
        """点击。给坐标就点到该处，否则点当前位置。button: 1左 2中 3右。"""
        if x is not None and y is not None:
            self.move(x, y)
            time.sleep(0.12)
        for _ in range(2 if double else 1):
            self._cmd(T_BUTTON, button, 1)
            time.sleep(0.05)
            self._cmd(T_BUTTON, button, 0)
            time.sleep(0.08)

    def click_window(self, app_id=None, title_contains=None, button=1):
        """点某个窗口的中心。找不到窗口返回 False。"""
        w = self.window(app_id, title_contains)
        if not w:
            return False
        self.click(w["cx"], w["cy"], button)
        return True

    def scroll(self, steps):
        """滚轮。正数向上，负数向下。"""
        self._cmd(T_SCROLL, steps & 0xFFFF, 0)

    # ---------------------------------------------------------------- 键盘

    def key(self, code, down=True):
        """按 evdev 扫描码发按键状态（底层接口）。"""
        self._cmd(T_KEY, code, 1 if down else 0)

    def tap(self, ch, shift=False):
        """敲一个 ASCII 字符。"""
        c = KEY.get(ch.lower() if ch.isalpha() else ch)
        if c is None:
            raise WStreamError(f"字符 {ch!r} 没有对应的键码，非 ASCII 请用 paste()")
        if shift or ch.isupper():
            self.key(MOD["shift"], True)
            time.sleep(0.01)
        self.key(c, True)
        self.key(c, False)
        if shift or ch.isupper():
            time.sleep(0.01)
            self.key(MOD["shift"], False)
        time.sleep(0.012)

    def type_text(self, text):
        """逐键输入 ASCII 文本。含非 ASCII 请改用 paste()。"""
        for ch in text:
            if ch in SHIFTED:
                self.tap(SHIFTED[ch], shift=True)
            elif ch.isupper():
                self.tap(ch.lower(), shift=True)
            else:
                self.tap(ch)

    def combo(self, *mods, key=None):
        """组合键，如 combo('ctrl', key='l')、combo('ctrl','shift',key='v')。

        注意间隔不要太短：合成器要时间处理 modifiers 事件，太快会丢修饰位。
        """
        codes = [MOD[m] for m in mods]
        for c in codes:
            self.key(c, True)
            time.sleep(0.05)
        if key:
            self.tap(key)
        for c in reversed(codes):
            self.key(c, False)
            time.sleep(0.05)

    def press(self, name):
        """常用功能键：enter / tab / esc / backspace / up / down / left / right。"""
        table = {"enter": 28, "tab": 15, "esc": 1, "backspace": 14,
                 "up": 103, "down": 108, "left": 105, "right": 106,
                 "home": 102, "end": 107, "pageup": 104, "pagedown": 109,
                 "delete": 111, "space": 57}
        c = table.get(name.lower())
        if c is None:
            raise WStreamError(f"不认识的键名 {name!r}")
        self.key(c, True)
        self.key(c, False)
        time.sleep(0.03)

    # ---------------------------------------------------------------- 文本 / 剪贴板

    def type_any(self, text, paste_keys="ctrl+shift+v"):
        """输入任意文本（含中文）。

        策略：纯 ASCII 逐键（快、稳）；含非 ASCII 走剪贴板粘贴
        （逐键的中文到不了 XWayland，只有剪贴板能通吃）。

        paste_keys 默认 ctrl+shift+v（终端习惯）；往 GUI 程序里贴用 "ctrl+v"。
        """
        if all(ord(c) < 128 for c in text):
            self.type_text(text)
        else:
            self.paste(text, keys=paste_keys)

    def paste(self, text, keys="ctrl+shift+v"):
        """把文本写进沙箱剪贴板，再发粘贴快捷键。

        ⚠️ 默认用 **Ctrl+Shift+V**，不是 Ctrl+V。实测：
          * 终端（alacritty/xterm）里粘贴是 Ctrl+Shift+V，Ctrl+V 是「引号转义」
          * GUI 程序（浏览器、编辑器）一般用 Ctrl+V
        所以在终端里贴东西要用默认值；往 GUI 程序里贴可以传 keys="ctrl+v"。
        """
        self.set_clipboard(text)
        time.sleep(0.25)
        self._send_combo(keys)
        time.sleep(0.15)

    def _send_combo(self, spec):
        """解析 "ctrl+shift+v" 这种字符串并发送组合键。"""
        parts = [p.strip().lower() for p in spec.split("+") if p.strip()]
        if not parts:
            return
        mods = [MOD[p] for p in parts[:-1] if p in MOD]
        key = parts[-1]
        for c in mods:
            self.key(c, True)
            time.sleep(0.05)
        self.tap(key)
        for c in reversed(mods):
            self.key(c, False)
            time.sleep(0.05)

    def set_clipboard(self, text):
        """type 7：把文本设成沙箱剪贴板。"""
        self._send(bytes([T_CLIP_SET, 0, 0, 0]) + struct.pack("<HH", 0, 0)
                   + text.encode("utf-8"))
        time.sleep(0.1)

    def get_clipboard(self, timeout=5.0):
        """type 8：读沙箱剪贴板。超时返回 None。"""
        self._cmd(T_CLIP_GET)
        payload = self._recv_frame(timeout=timeout)
        if payload and len(payload) >= 8 and payload[0] == D_CLIPBOARD:
            return payload[8:].decode("utf-8", "replace")
        return None

    # ---------------------------------------------------------------- 组合操作

    def wait_ready(self, timeout=30):
        """等 wstream 能服务（start.sh 刚返回时它可能还没初始化完）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                self._http("/", timeout=2)
                return True
            except Exception:
                time.sleep(0.3)
        return False

    def run_in_window(self, command, app_id=None, title_contains=None):
        """在指定窗口里输入命令并回车。找不到窗口返回 False。"""
        if not self.click_window(app_id, title_contains):
            return False
        time.sleep(0.3)
        self.type_any(command)
        self.press("enter")
        return True
