#!/usr/bin/env bash
# build.sh — 从协议 XML 生成 C 代码并编译 wstream
#
# 依赖（Arch / CachyOS）：
#   sudo pacman -S --needed gcc pkgconf wayland libjpeg-turbo libxkbcommon wlr-protocols
set -eu
cd "$(dirname "$0")"
mkdir -p proto

gen() {  # $1=XML 路径  $2=输出前缀
    [ -f "$1" ] || { echo "缺少协议 XML：$1" >&2; exit 1; }
    wayland-scanner client-header "$1" "proto/$2.h"
    wayland-scanner private-code  "$1" "proto/$2.c"
}

# wlroots 的协议从系统包拿（wlr-protocols）
PROTO="${WLR_PROTO_DIR:-/usr/share/wlr-protocols/unstable}"
gen "$PROTO/wlr-screencopy-unstable-v1.xml"      wlr-screencopy
gen "$PROTO/wlr-virtual-pointer-unstable-v1.xml" wlr-virtual-pointer

# virtual-keyboard 的 XML 不在任何发行包里，随源码带一份
gen protocols/virtual-keyboard-unstable-v1.xml   virtual-keyboard

gcc -O2 -g -Wall -Wextra -o wstream src/wstream.c \
    proto/wlr-screencopy.c proto/wlr-virtual-pointer.c proto/virtual-keyboard.c \
    -Iproto $(pkg-config --cflags --libs wayland-client libturbojpeg xkbcommon) -lpthread

echo "✅ 编译完成：$(pwd)/wstream"
