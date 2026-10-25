#!/usr/bin/env bash
# dashboard.sh — 生成一个总览页面，同时看所有 wstream 实例的实时画面
#
# 为什么需要：多 agent 场景下每个实例是独立虚拟桌面、独立端口，
# 想「一眼看到全部」就得开一堆标签页。这个页面把所有实例的 MJPEG 流
# 并排嵌进一个网格里。
#
# 为什么只嵌画面、不带交互：
#   各实例的 Origin 校验只信任自己端口的页面，所以跨端口**不能**建 WebSocket。
#   但 <img> 加载 MJPEG 不发 Origin 头、Host 也是回环地址，能通过校验。
#   想操作某个实例，点它的「打开」用原生页面。
#
# 用法：
#   ./scripts/dashboard.sh               生成 /tmp/wstream-dashboard.html
#   ./scripts/dashboard.sh -o out.html   指定输出位置
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="/tmp/wstream-dashboard.html"
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) shift; OUT="${1:-$OUT}" ;;
  esac
  shift
done

URLS=$("$REPO/scripts/list.sh" --url 2>/dev/null)

{
  cat <<'HEAD'
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>wstream 实例总览</title>
<style>
  :root{color-scheme:dark}
  body{margin:0;background:#0b0f14;color:#cbd5e1;
       font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
  header{padding:14px 18px;border-bottom:1px solid #1e293b;display:flex;
         align-items:baseline;gap:12px;flex-wrap:wrap}
  h1{font-size:14px;margin:0;font-weight:600;letter-spacing:.06em;color:#f8fafc}
  .meta{color:#64748b;font-size:11px}
  #grid{display:grid;gap:14px;padding:16px;
        grid-template-columns:repeat(auto-fill,minmax(420px,1fr))}
  .card{background:#111827;border:1px solid #1e293b;border-radius:8px;overflow:hidden}
  .bar{display:flex;align-items:center;justify-content:space-between;
       padding:8px 11px;border-bottom:1px solid #1e293b;font-size:12px}
  .port{color:#93c5fd;font-weight:600}
  .st{font-size:11px;color:#64748b}
  .st.on::before{content:"\25CF ";color:#4ade80}
  a{color:#93c5fd;text-decoration:none;font-size:11px}
  a:hover{text-decoration:underline}
  .shot{display:block;width:100%;background:#020617;min-height:200px}
  .empty{padding:40px;text-align:center;color:#64748b}
</style></head><body>
<header>
  <h1>wstream 实例总览</h1>
  <span class="meta">每个实例都是完全独立的虚拟桌面 · 画面为实时 MJPEG · 点「打开」可交互</span>
</header>
<div id="grid">
HEAD

  n=0
  while read -r url; do
    [ -z "$url" ] && continue
    port="${url##*:}"
    n=$((n+1))
    cat <<CARD
  <div class="card">
    <div class="bar">
      <span class="port">:$port</span>
      <span class="st on">在线</span>
      <a href="$url" target="_blank">打开 ↗</a>
    </div>
    <img class="shot" src="$url/stream.mjpg" alt="instance $port"
         onerror="this.style.display='none';this.nextElementSibling.style.display='block'">
    <div class="empty" style="display:none">画面加载失败（实例可能已停止）</div>
  </div>
CARD
  done <<< "$URLS"

  if [ "$n" -eq 0 ]; then
    echo '  <div class="empty">没有正在运行的实例。启动：<code>./scripts/start.sh</code></div>'
  fi

  cat <<'TAIL'
</div></body></html>
TAIL
} > "$OUT"

echo "$OUT"
