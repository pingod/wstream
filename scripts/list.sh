#!/usr/bin/env bash
# list.sh — 列出当前在本机运行的所有 wstream 实例
#
# 为什么需要：多 agent 场景下每个实例有自己的 $ROOT 和端口，
# 浏览器里要一个个记住地址很累。这个脚本扫描 /proc 里所有 wstream 进程，
# 从它们的 XDG_RUNTIME_DIR 反推实例根目录，再探测每个实例的端口。
#
# 用法：
#   ./scripts/list.sh          表格
#   ./scripts/list.sh --url    只输出 URL，方便 xargs / 脚本消费
set -u

PORT_ARG=""
URL_ONLY=0
for a in "$@"; do
  case "$a" in
    --url) URL_ONLY=1 ;;
    -p|--port) shift; PORT_ARG="${1:-}" ;;
  esac
done

say() { printf '%b\n' "$*"; }

# 找出所有 wstream 进程 → (pid, root, port)
found=0
rows=""
for p in $(pgrep -x wstream 2>/dev/null); do
  [ -r "/proc/$p/environ" ] || continue
  # 僵尸跳过
  st=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)
  [ "$st" = "Z" ] && continue

  rt=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')
  [ -n "${rt:-}" ] || continue
  root=$(dirname "$rt")
  # 端口在命令行里：-p 8801
  port=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | sed -n 's/.*-p \([0-9]\+\).*/\1/p')
  [ -n "${port:-}" ] || continue
  [ -n "$PORT_ARG" ] && [ "$port" != "$PORT_ARG" ] && continue

  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$port/" 2>/dev/null || echo "---")
  wl=$(cat "$root/wl_name" 2>/dev/null || echo "?")
  rows="$rows$port|$root|$p|$wl|$code
"
  found=$((found+1))
done

if [ "$URL_ONLY" = 1 ]; then
  echo "$rows" | sed '/^$/d' | cut -d'|' -f1 | while read -r port; do
    [ -n "$port" ] && echo "http://127.0.0.1:$port"
  done
  exit 0
fi

if [ "$found" -eq 0 ]; then
  say "\033[1;36m[wstream]\033[0m 没有正在运行的实例"
  say "  启动一个：  ./scripts/start.sh"
  say "  多实例：    WSTREAM_ROOT=/tmp/ws-a WSTREAM_PORT=8801 ./scripts/start.sh"
  exit 0
fi

say "\033[1;36m[wstream]\033[0m 共 $found 个实例\n"
# 中文占两字符宽，用固定字符串 + 手工对齐，别用 %-Ns（会算错宽度）
say "  PORT    状态    运行时目录               URL"
say "  ----    -----   ----------------------   ---"
echo "$rows" | sed '/^$/d' | while IFS='|' read -r port root pid wl code; do
  if [ "$code" = "200" ]; then st="在线  "; else st="离线  "; fi
  printf '  %-7s %s  %-22s  %s\n' "$port" "$st" "$root" "http://127.0.0.1:$port"
done
say ""
say "提示：每个实例是**完全独立的虚拟桌面**，各开一个浏览器标签页即可同时查看。"
