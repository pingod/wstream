#!/usr/bin/env bash
# start.sh — 起一个无头 sway 沙箱 + wstream 实时画面服务
#
# 用法：
#   ./scripts/start.sh              # 默认 1600x1000，端口 8765
#   ./scripts/start.sh 1920x1080    # 指定沙箱分辨率
#   ./scripts/start.sh stop         # 全部停止
#
# 你只需要：浏览器打开 http://127.0.0.1:8765
# 全程不建任何窗口到你的桌面，不抢焦点，不抓你的键鼠。

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
ROOT="${WSTREAM_ROOT:-/tmp/wstream}"
SIZE="${1:-1600x1000}"
PORT="${WSTREAM_PORT:-8765}"

log() { printf '\033[1;36m[wstream]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[wstream]\033[0m %s\n' "$*" >&2; exit 1; }

# 进程是不是僵尸（已退出但还没被父进程 reap）。
# ⚠️ 为什么需要：Docker/CI 容器里 PID 1 往往不 reap 子进程，被 kill 的进程
# 会一直以 <defunct> 状态存在，`pgrep`/`kill -0` **照样能匹配到它**。
# 踩过：CI 里 stop 之后下一步 start 说「wstream 已在运行」，但端口根本不响应，
# 于是回归测试全部 Connection refused。
is_zombie() {
  [ -r "/proc/$1/stat" ] || return 1
  # stat 的第 3 个字段是状态，Z = zombie
  st=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)
  [ "$st" = "Z" ]
}

# 真的活着吗：进程在、不是僵尸。
alive() {
  p=$1
  [ -n "${p:-}" ] || return 1
  kill -0 "$p" 2>/dev/null || return 1
  ! is_zombie "$p"
}

# 端口上有没有人在服务（比「进程存在」可靠得多）
port_serving() {
  command -v curl >/dev/null 2>&1 || return 1
  curl -fsS -o /dev/null --max-time 2 "http://127.0.0.1:$1/" 2>/dev/null
}

# 这个进程属于**本实例**吗（XDG_RUNTIME_DIR 指向 $ROOT/run）。
#
# ⚠️ 为什么必须有这个：支持多实例（每个 agent 一套独立虚拟桌面）。
# 但 pgrep -x wstream 是**全局**的，不分实例。踩过：起了 A(8801) 和 B(8802)，
# B 启动时发现「有 wstream 进程但我的端口没在服务」，就把 A 也 kill -9 了，
# 结果是「后起的踢掉先起的」。
# 判断依据用 XDG_RUNTIME_DIR 而不是命令行参数：它由 start.sh 显式设置，
# 且 /proc/PID/environ 随时可读，比解析 argv 可靠。
belongs_to_root() {
  p=$1
  [ -r "/proc/$p/environ" ] || return 1
  rt=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')
  [ "$rt" = "$ROOT/run" ]
}

# 列出本实例的 wstream/sway 进程 pid
own_pids() {
  for name in wstream sway-nocap sway; do
    for p in $(pgrep -x "$name" 2>/dev/null); do
      belongs_to_root "$p" && echo "$p"
    done
  done
}

stop_all() {
  for f in "$ROOT"/*.pid; do
    [ -f "$f" ] || continue
    p=$(cat "$f" 2>/dev/null)
    [ -n "${p:-}" ] && kill "$p" 2>/dev/null
    rm -f "$f"
  done
  # 兜底：只杀 XDG_RUNTIME_DIR 指向沙箱的进程，绝不误伤用户自己的桌面
  for p in $(own_pids); do
    is_zombie "$p" && continue          # 已经死了，不用管
    kill "$p" 2>/dev/null
  done
  # 等它们真的退出（最多 5 秒），别让下一步 start 撞上还没死透的进程
  i=0
  while [ $i -lt 25 ]; do
    busy=""
    for p in $(own_pids); do
      is_zombie "$p" || busy="$p"
    done
    [ -z "$busy" ] && break
    sleep 0.2; i=$((i+1))
  done
  log "已停止"
}

if [ "${1:-}" = "stop" ]; then stop_all; exit 0; fi

[ -x "$REPO/wstream" ] || die "先编译：$REPO/build.sh"

# ---------- 1. 无头沙箱 ----------
WL=""
if [ -n "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)" ]; then
  WL=$(basename "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)")
  log "沙箱已在运行（$WL），复用"
else
  rm -rf "$ROOT"
  mkdir -p "$ROOT/run" "$ROOT/cfg/sway" "$ROOT/out"
  chmod 700 "$ROOT/run"

  # 沙箱里放两个不停刷新的终端，方便肉眼确认「画面是实时的」
  cat > "$ROOT/desktop.sh" <<'XEOF'
#!/bin/sh
alacritty --class AgentTop -e sh -c 'c=1; while :; do printf "\033[1;36m窗口 1\033[0m  %s  第 %d 次刷新\n" "$(date +%H:%M:%S)" $c; c=$((c+1)); sleep 1; done' &
sleep 1
alacritty --class AgentSub -e sh -c 'n=1; while :; do echo "窗口 2  计数器 $n"; n=$((n+1)); sleep 1; done' &
sleep 36000
XEOF
  chmod +x "$ROOT/desktop.sh"

  # sway 没有 labwc 的 -S（启动钩子），必须写进配置的 exec
  {
    printf '%s\n' 'set $mod Mod4' 'default_border pixel 2' 'default_floating_border pixel 2'
    printf 'exec %s\n' "$ROOT/desktop.sh"
  } > "$ROOT/cfg/sway/config"

  log "启动无头沙箱 $SIZE …"

  # ── 容器 / CI 兼容：绕过 sway 的 cap_sys_nice ──────────────────────
  # Arch 打包的 /usr/bin/sway 带文件能力 cap_sys_nice=ep。在 Docker/CI 容器里
  # root 往往没有 CAP_SYS_NICE，此时 execve 直接返回 EPERM，报错长这样：
  #     env: 'sway': Operation not permitted
  # 这不是 sway 的问题，也不是我们的问题。处理办法：把二进制复制一份，
  # 复制会丢掉文件能力，然后用副本启动。
  SWAY_BIN="$(command -v sway || true)"
  [ -n "$SWAY_BIN" ] || die "找不到 sway，先装：sudo pacman -S --needed sway"
  if command -v getcap >/dev/null 2>&1 && [ -n "$(getcap "$SWAY_BIN" 2>/dev/null)" ]; then
    SWAY_COPY="$ROOT/sway-nocap"
    cp "$SWAY_BIN" "$SWAY_COPY" 2>/dev/null && chmod +x "$SWAY_COPY" 2>/dev/null \
      && SWAY_BIN="$SWAY_COPY"
  fi

  # -u DISPLAY 很关键：否则 GTK/Qt 程序会连着真实桌面的 Xwayland，
  # 窗口会跑到用户真实桌面上（踩过：firefox 泄漏到真实 workspace）。
  setsid env -u DISPLAY \
    XDG_RUNTIME_DIR="$ROOT/run" XDG_CONFIG_HOME="$ROOT/cfg" \
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
    "$SWAY_BIN" --unsupported-gpu > "$ROOT/sway.log" 2>&1 &
  echo $! > "$ROOT/sway.pid"

  # sway 的 socket 名不一定是 wayland-0（多实例时会是 wayland-1），动态找。
  # 超时给到 60 秒：容器 / 首次运行时 fontconfig 要建缓存，可能比裸机慢一个数量级
  # （踩过：CI 里 15 秒超时刚好不够，报「sway 没起来」但其实只是慢）。
  i=0
  while [ $i -lt 240 ]; do
    [ -n "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)" ] && break
    # sway 提前退出就不用再等了
    kill -0 "$(cat "$ROOT/sway.pid" 2>/dev/null)" 2>/dev/null || break
    sleep 0.25; i=$((i+1))
  done
  WL=$(basename "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)" 2>/dev/null)
  if [ -z "$WL" ]; then
    # 把日志打到 stderr，CI / 用户都能直接看到失败原因
    echo "──── sway.log ────" >&2
    cat "$ROOT/sway.log" >&2 2>/dev/null
    echo "──────────────────" >&2
    die "sway 没起来（日志见上）"
  fi
  echo "$WL" > "$ROOT/wl_name"

  # 改分辨率
  OUT=$(XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" wlr-randr 2>/dev/null | head -1 | awk '{print $1}')
  [ -n "${OUT:-}" ] && XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" \
      wlr-randr --output "$OUT" --custom-mode "$SIZE" >/dev/null 2>&1
  sleep 3
fi

# ---------- 2. wstream ----------
# ⚠️ 判据是「端口真的在服务」，不是「有个叫 wstream 的进程」。
# 容器里被 kill 的进程会变成僵尸、pgrep 仍能匹配（见 is_zombie 的说明）。
# ⚠️ 只看**本实例**的进程（$ROOT/run 下的），绝不碰别的实例。
# 别的 agent 可能正跑着自己的 wstream，全局 pgrep 会把它一起杀掉。
RUNNING=""
OWNSELF=""
for p in $(pgrep -x wstream 2>/dev/null); do
  belongs_to_root "$p" || continue
  OWNSELF="$OWNSELF $p"
  if alive "$p"; then RUNNING=1; fi
done
if [ -n "$RUNNING" ] && port_serving "$PORT"; then
  log "wstream 已在运行（端口 $PORT 可服务）"
else
  # 本实例有进程但端口不服务 → 僵尸或卡死，只清掉自己的
  for p in $OWNSELF; do
    kill -9 "$p" 2>/dev/null
  done
  log "启动 wstream（端口 $PORT）…"
  setsid env -u DISPLAY \
    XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" \
    "$REPO/wstream" -p "$PORT" -f 10 -v >> "$ROOT/wstream.log" 2>&1 < /dev/null &
  echo $! > "$ROOT/wstream.pid"

  # 等端口真的能服务（内部要连 Wayland、建 shmbuf、初始化虚拟设备，
  # 光 sleep 2 秒在慢机器/容器里不够 —— 踩过：返回后测试立刻连接被拒）
  i=0
  while [ $i -lt 100 ]; do
    if port_serving "$PORT"; then break; fi
    # 进程提前挂了就别再等
    W=$(cat "$ROOT/wstream.pid" 2>/dev/null)
    if [ -n "${W:-}" ] && ! alive "$W"; then
      echo "──── wstream.log ────" >&2
      tail -20 "$ROOT/wstream.log" >&2 2>/dev/null
      echo "─────────────────────" >&2
      die "wstream 启动后立即退出（日志见上）"
    fi
    sleep 0.2; i=$((i+1))
  done
  if ! port_serving "$PORT"; then
    echo "──── wstream.log ────" >&2
    tail -20 "$ROOT/wstream.log" >&2 2>/dev/null
    echo "─────────────────────" >&2
    die "wstream 已启动但端口 $PORT 一直不可服务"
  fi
fi

log "──────────────────────────────────────────"
log " 浏览器打开： http://127.0.0.1:$PORT"
log " 停止：       $0 stop"
log "──────────────────────────────────────────"
