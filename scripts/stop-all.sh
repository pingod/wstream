#!/usr/bin/env bash
# stop-all.sh — 一次停掉本机所有 wstream 实例
#
# 为什么需要：多 agent 场景下会有好几个实例各自跑着（A 在 8801、B 在 8802…），
# 收工时一个个 `WSTREAM_ROOT=... ./scripts/start.sh stop` 很容易漏。
# 更常见的是「忘了当初用了哪个 ROOT」，于是残留的 sway 一直吃内存。
#
# 安全设计（这个脚本会杀进程，必须保守）：
#   1. **只动 wstream 实例**：判据是进程的 XDG_RUNTIME_DIR 下确实有 wstream 的
#      运行时结构（run/ + wstream.pid 或 sway.pid）。用户自己的 sway / Hyprland
#      的 XDG_RUNTIME_DIR 是 /run/user/NNNN，不会匹配。
#   2. **先列再杀**：默认打印将要停止的实例并要求确认；`--yes` 跳过确认（CI 用）。
#   3. **不碰用户桌面**：绝不按进程名裸杀。`sway` 这个名字在你桌面也在用。
#
# 用法：
#   ./scripts/stop-all.sh            列出并确认
#   ./scripts/stop-all.sh --yes      不确认，直接停
#   ./scripts/stop-all.sh --dry-run  只列不停
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

ASSUME_YES=0
DRY_RUN=0
for a in "$@"; do
  case "$a" in
    -y|--yes) ASSUME_YES=1 ;;
    -n|--dry-run) DRY_RUN=1 ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
  esac
done

say() { printf '%b\n' "$*"; }
log() { printf '\033[1;36m[wstream]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[wstream]\033[0m %s\n' "$*"; }

# 一个 pid 是不是「wstream 实例的进程」。
#
# 判据（两条都要满足）：
#   a) XDG_RUNTIME_DIR 指向一个目录，该目录里同时有 run/ 和某个 .pid 文件
#      —— 这是 start.sh 创建的实例目录结构
#   b) 进程名在白名单里（wstream / sway-nocap / sway / Xwayland / alacritty 等）
#
# 为什么不满足于 (b)：用户的桌面会话也跑着 sway（Hyprland 的 Xwayland 等），
# 按名字杀会直接把用户的桌面干掉。
instance_root_of() {
  local p="$1" rt cmd
  [ -r "/proc/$p/environ" ] || return 1
  rt=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')
  [ -n "${rt:-}" ] || return 1
  case "$rt" in
    */run) ;;
    *) return 1 ;;
  esac
  local root="${rt%/run}"

  # ⚠️ 不能要求 $root 目录「存在」。踩过：实例目录被 rm -rf 之后，它的
  # desktop.sh 孤儿还在跑（while 循环 sleep 36000），却因为「目录没了」
  # 而永远匹配不上任何实例，stop-all 清不掉。
  #
  # 所以归属判据放宽成「环境变量 + 命令行里都指向同一个 <root>」：
  #   - XDG_RUNTIME_DIR = <root>/run
  #   - cmdline 里出现 <root>/ 开头的路径（desktop.sh 是 <root>/desktop.sh，
  #     wstream/sway 是工作目录或显式参数）
  # 这两个都带上实例路径，用户真实桌面的进程不可能同时满足。

  # 认作 wstream 实例的三种证据，任一即可：
  #   1) $root 下还有 start.sh 写的 pid 文件（目录还在）
  #   2) cmdline 里有 "<root>/xxx.sh" 这类脚本路径（目录删了也认）
  #   3) cmdline 里直接有 "<root>/" 前缀的路径
  local has=0 f
  for f in "$root"/*.pid; do
    [ -f "$f" ] && has=1 && break
  done
  if [ "$has" = 0 ]; then
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
    case "$cmd" in
      *"$root/"*) has=1 ;;
    esac
  fi
  [ "$has" = 1 ] || return 1
  echo "$root"
}

# 收集：root → 一组 pid
declare -A ROOT_PIDS=()
for name in wstream sway-nocap sway Xwayland alacritty desktop.sh; do
  for p in $(pgrep -x "$name" 2>/dev/null); do
    # 僵尸不算
    st=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)
    [ "$st" = "Z" ] && continue
    root=$(instance_root_of "$p") || continue
    ROOT_PIDS["$root"]="${ROOT_PIDS[$root]:-} $p"
  done
done

if [ "${#ROOT_PIDS[@]}" -eq 0 ]; then
  log "没有正在运行的 wstream 实例"
  exit 0
fi

# 排序后输出，稳定一点
ROOTS=$(printf '%s\n' "${!ROOT_PIDS[@]}" | sort)

log "发现 ${#ROOT_PIDS[@]} 个 wstream 实例："
echo
printf '  %-24s %s\n' "运行时目录" "进程数"
printf '  %-24s %s\n' "----------" "------"
for root in $ROOTS; do
  n=$(echo ${ROOT_PIDS[$root]} | wc -w)
  port="?"
  # 端口从 wstream 的 cmdline 里读
  for p in ${ROOT_PIDS[$root]}; do
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
    case "$cmd" in
      *wstream*) port=$(echo "$cmd" | sed -n 's/.*-p \([0-9]\+\).*/\1/p'); break ;;
    esac
  done
  printf '  %-24s %s 个  端口 %s\n' "$root" "$n" "$port"
done
echo

# 明确告诉用户我们**不会**碰什么
self_rt="${XDG_RUNTIME_DIR:-}"
if [ -n "$self_rt" ] && [ "$self_rt" != "/run/user/$(id -u)" ]; then
  warn "注意：你当前 shell 的 XDG_RUNTIME_DIR 是 $self_rt（不是 /run/user/$(id -u)）"
  warn "      它不会被停，除非它同时是上面列出的 wstream 实例目录"
  echo
fi

if [ "$DRY_RUN" = 1 ]; then
  log "--dry-run，不做任何操作"
  exit 0
fi

if [ "$ASSUME_YES" != 1 ]; then
  if [ ! -t 0 ]; then
    warn "非交互环境，未带 --yes 就不动手。确认要停请加 --yes"
    exit 1
  fi
  printf '确认停止上面这些实例？[y/N] '
  read -r ans
  case "$ans" in
    y|Y|yes|YES) ;;
    *) log "已取消"; exit 0 ;;
  esac
fi

# 逐个停：优先用 start.sh stop（它会按 XDG_RUNTIME_DIR 精确清理 + 等真退出）
fail=0
for root in $ROOTS; do
  log "停止 $root …"
  if [ -x "$REPO/scripts/start.sh" ]; then
    WSTREAM_ROOT="$root" "$REPO/scripts/start.sh" stop >/dev/null 2>&1 || true
  fi
  # 兜底：start.sh stop 没能清掉的，自己按归属杀掉
  for p in ${ROOT_PIDS[$root]}; do
    if kill -0 "$p" 2>/dev/null; then
      st=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)
      [ "$st" = "Z" ] && continue
      kill "$p" 2>/dev/null
      sleep 1
      if kill -0 "$p" 2>/dev/null; then
        kill -9 "$p" 2>/dev/null
      fi
    fi
  done
done

# 等真退出
i=0
while [ $i -lt 25 ]; do
  left=0
  for root in $ROOTS; do
    for p in ${ROOT_PIDS[$root]}; do
      if kill -0 "$p" 2>/dev/null; then
        st=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)
        [ "$st" = "Z" ] || left=$((left+1))
      fi
    done
  done
  [ "$left" -eq 0 ] && break
  sleep 0.2; i=$((i+1))
done

echo
if [ "$left" -eq 0 ]; then
  log "全部已停止（${#ROOT_PIDS[@]} 个实例）"
  # 残留 socket 文件也清掉，避免下次 start 误判（start.sh 现在会自检，双保险）
  for root in $ROOTS; do
    [ -d "$root" ] && rm -rf "$root"/run/wayland-* 2>/dev/null
  done
else
  warn "仍有 $left 个进程没退出，可能是卡在 D 状态（IO 等待）"
  warn "可以用 ./scripts/list.sh 复查，或 kill -9 手动处理"
  fail=1
fi

exit "$fail"
