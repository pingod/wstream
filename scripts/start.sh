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

stop_all() {
  for f in "$ROOT"/*.pid; do
    [ -f "$f" ] || continue
    p=$(cat "$f" 2>/dev/null)
    [ -n "${p:-}" ] && kill "$p" 2>/dev/null
    rm -f "$f"
  done
  # 兜底：只杀 XDG_RUNTIME_DIR 指向沙箱的进程，绝不误伤用户自己的桌面
  for p in $(pgrep -x sway 2>/dev/null) $(pgrep -x wstream 2>/dev/null); do
    [ -r "/proc/$p/environ" ] || continue
    rt=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')
    [ "$rt" = "$ROOT/run" ] && kill "$p" 2>/dev/null
  done
  sleep 0.5
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
  # -u DISPLAY 很关键：否则 GTK/Qt 程序会连着真实桌面的 Xwayland，
  # 窗口会跑到用户真实桌面上（踩过：firefox 泄漏到真实 workspace）。
  setsid env -u DISPLAY \
    XDG_RUNTIME_DIR="$ROOT/run" XDG_CONFIG_HOME="$ROOT/cfg" \
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
    sway --unsupported-gpu > "$ROOT/sway.log" 2>&1 &
  echo $! > "$ROOT/sway.pid"

  # sway 的 socket 名不一定是 wayland-0（多实例时会是 wayland-1），动态找
  i=0
  while [ $i -lt 60 ]; do
    [ -n "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)" ] && break
    sleep 0.25; i=$((i+1))
  done
  WL=$(basename "$(ls "$ROOT"/run/wayland-* 2>/dev/null | head -1)" 2>/dev/null)
  [ -n "$WL" ] || die "sway 没起来，看 $ROOT/sway.log"
  echo "$WL" > "$ROOT/wl_name"

  # 改分辨率
  OUT=$(XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" wlr-randr 2>/dev/null | head -1 | awk '{print $1}')
  [ -n "${OUT:-}" ] && XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" \
      wlr-randr --output "$OUT" --custom-mode "$SIZE" >/dev/null 2>&1
  sleep 3
fi

# ---------- 2. wstream ----------
if pgrep -x wstream >/dev/null 2>&1; then
  log "wstream 已在运行"
else
  log "启动 wstream（端口 $PORT）…"
  setsid env -u DISPLAY \
    XDG_RUNTIME_DIR="$ROOT/run" WAYLAND_DISPLAY="$WL" \
    "$REPO/wstream" -p "$PORT" -f 10 -v >> "$ROOT/wstream.log" 2>&1 < /dev/null &
  echo $! > "$ROOT/wstream.pid"
  sleep 2
fi

log "──────────────────────────────────────────"
log " 浏览器打开： http://127.0.0.1:$PORT"
log " 停止：       $0 stop"
log "──────────────────────────────────────────"
