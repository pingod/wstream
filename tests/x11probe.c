/*
 * x11probe.c — CI 测试用的 X11 客户端「探针」
 *
 * 为什么需要它：wstream 的键鼠注入有没有真正到达应用，光看截图不可靠
 * （终端每秒刷新，画面一直在变）。这个探针创建一个 X11 窗口，把收到的
 * 按键 / 鼠标事件**按行打印到 stdout**，CI 里用 grep 精确断言即可。
 *
 * 顺带它还是「XWayland 可用性」的证据：这个窗口能被 wstream 抓到、
 * 也能收到注入的按键，说明 XWayland 不是文档早期说的那样「不可用」。
 *
 * 输出格式（一行一条，便于 grep）：
 *   READY                           窗口已创建并映射
 *   KEY <keysym-name> <code> sym=<hex>  收到了按键（sym 是 keysym 原始值）
 *   BUTTON <n>                      收到了鼠标点击
 *
 * 为什么要打印 sym 原始值：非 ASCII 字符（中文等）注入时用的是 xkb 的
 * Unicode keysym（0x01000000 + codepoint）。XKeysymToString 对这种键
 * 往往退化成 "Escape"，只看名字会误判成「中文没到」。用原始值 + 区间判断
 * 才能断言准确。
 *
 * 用法：DISPLAY=:0 ./x11probe
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "x11probe: 连不上 X server（DISPLAY=%s）\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(未设)");
        return 1;
    }
    int s = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), 40, 40, 520, 280, 2,
                                   BlackPixel(d, s), WhitePixel(d, s));
    XStoreName(d, w, "WSTREAM_X11_PROBE");
    XSelectInput(d, w, KeyPressMask | ExposureMask | ButtonPressMask
                       | StructureNotifyMask);
    XMapWindow(d, w);
    XFlush(d);

    GC gc = XCreateGC(d, w, 0, NULL);
    XSetForeground(d, gc, BlackPixel(d, s));
    int y = 40;
    int ready = 0;

    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) {
            XDrawString(d, w, gc, 20, 30, "X11 PROBE", 9);
            XDrawString(d, w, gc, 20, 55, "waiting for injected input...", 29);
            XFlush(d);
            if (!ready) { printf("READY\n"); fflush(stdout); ready = 1; }
        } else if (e.type == KeyPress) {
            char buf[32];
            KeySym ks = NoSymbol;
            int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, NULL);
            buf[n] = 0;
            const char *name = XKeysymToString(ks);
            /* sym=0x... 是 keysym 原始值；Unicode keysym 落在 0x01000000..0x0110FFFF */
            printf("KEY %s %u sym=0x%lx\n", name ? name : "?", e.xkey.keycode,
                   (unsigned long)ks);
            fflush(stdout);
            char line[128];
            snprintf(line, sizeof line, "key: %s (code %u)", name ? name : "?", e.xkey.keycode);
            if (y < 260) { XDrawString(d, w, gc, 20, y += 26, line, (int)strlen(line)); XFlush(d); }
        } else if (e.type == ButtonPress) {
            printf("BUTTON %u\n", e.xbutton.button);
            fflush(stdout);
            char line[64];
            snprintf(line, sizeof line, "button %u", e.xbutton.button);
            if (y < 260) { XDrawString(d, w, gc, 20, y += 26, line, (int)strlen(line)); XFlush(d); }
        }
    }
}
