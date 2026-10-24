/*
 * wstream — 把 Wayland 合成器画面实时推到浏览器，并把浏览器里的鼠标键盘
 *           注入回沙箱。画面 + 交互，全程在浏览器里完成，不碰真实桌面的任何窗口。
 *
 * 架构：
 *   抓屏   zwlr_screencopy_manager_v1 + wl_shm（wlroots 系：labwc / sway）
 *   编码   libjpeg-turbo → JPEG
 *   下行   HTTP multipart/x-mixed-replace（MJPEG，浏览器 <img> 原生支持）
 *   上行   WebSocket（自己实现 RFC6455 握手 + 解帧，无外部依赖）
 *   注入   zwp_virtual_keyboard_v1 + zwlr_virtual_pointer_v1
 *
 * 安全边界：只监听 127.0.0.1，永远不建真实窗口 ⇒ 物理上不可能抢用户焦点。
 *
 * 上行消息格式（小端，8 字节头 + 可选负载）：
 *   byte0    = type
 *   byte1    = flags（未用）
 *   byte2-3  = 保留
 *   byte4-7  = 参数 A（16 位）/ 参数 B（byte6-7）
 *
 *   type 1  move      : A=虚拟x(0..65535) B=虚拟y   → 绝对定位
 *   type 2  button    : A=button(1左2中3右)  B=state(0/1)
 *   type 3  scroll    : A=离散步数（有符号 16 位）
 *   type 4  key       : A=evdev keycode      B=state(0/1)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <turbojpeg.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-client.h>

#include "wlr-screencopy.h"
#include "wlr-virtual-pointer.h"
#include "virtual-keyboard.h"
#include "wlr-data-control.h"

/* ============================================================ 基础工具 */

static int g_verbose;
static int g_quality = 70;
static int g_fps = 10;
/* keymap 新鲜度：
 *   0 = 需要重推（刚被 Unicode 注入换掉、或初次）
 *   1 = 我们推的完整 evdev 布局仍是当前生效的那张
 *
 * ⚠️ 为什么不能每键都推 keymap（2026-10-24 实测坐实）：
 *   合成器收到新 keymap 会**重建键盘状态，修饰位被清零**。
 *   每键都推的后果是 Ctrl+V 退化成傻按一个 v、Shift+v 出小写 v ——
 *   1.1.0 为了「盖掉别的客户端留下的残缺 keymap」加了每键推送，
 *   结果把组合键全弄坏了。这是个真实的回归，只有打大写/粘贴才会暴露。
 *
 * 现在的策略：正常只推一次，Unicode 注入后标脏、下次按键前重推。 */
static int g_keymap_fresh;
/* 上一次发给合成器的修饰位。用来判断「变了没」——
 * 必须能发 0（全部释放），否则 Shift/Ctrl 会卡住（踩过）。 */
static uint32_t g_mods_sent;

static int g_port = 8765;   /* 用于 Origin 校验：只信任发往本端口的页面 */
static volatile sig_atomic_t g_net_stop;   /* main 退出时通知网络线程收工 */

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("wstream: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ============================================================ JPEG */

struct jpeg_enc {
    tjhandle tj;
    uint8_t *buf;
    unsigned long cap;
};

static int jpeg_open(struct jpeg_enc *e) {
    e->tj = tjInitCompress();
    if (!e->tj) return -1;
    e->cap = 512 * 1024;
    e->buf = malloc(e->cap);
    return e->buf ? 0 : -1;
}

/* BGRX（ARGB8888 的内存序）直接压 JPEG，stride 交给 turbojpeg，省掉一次全图转换 */
static unsigned long jpeg_encode(struct jpeg_enc *e, const uint8_t *bgrx, int w, int h) {
    unsigned char *out = e->buf;
    unsigned long outlen = e->cap;
    if (tjCompress2(e->tj, bgrx, w, w * 4, h, TJPF_BGRX,
                    &out, &outlen, TJSAMP_420, g_quality, TJFLAG_FASTDCT) == 0) {
        e->buf = out;
        return outlen;
    }
    if (outlen > e->cap) {
        free(e->buf);
        e->cap = outlen + 65536;
        e->buf = malloc(e->cap);
        if (!e->buf) return 0;
        out = e->buf;
        unsigned long l2 = e->cap;
        if (tjCompress2(e->tj, bgrx, w, w * 4, h, TJPF_BGRX,
                        &out, &l2, TJSAMP_420, g_quality, TJFLAG_FASTDCT) == 0) {
            e->buf = out;
            return l2;
        }
    }
    return 0;
}

/* ============================================================ Wayland 状态 */

struct capture {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm     *shm;
    struct wl_output  *output;
    struct wl_seat    *seat;

    struct zwlr_screencopy_manager_v1     *mgr;
    struct zwlr_virtual_pointer_manager_v1 *ptr_mgr;
    struct zwp_virtual_keyboard_manager_v1 *kbd_mgr;

    struct zwlr_virtual_pointer_v1 *ptr;
    struct zwp_virtual_keyboard_v1 *kbd;

    /* 剪贴板（data-control 协议，不需要焦点，专给剪贴板管理器用） */
    struct zwlr_data_control_manager_v1 *dc_mgr;
    struct zwlr_data_control_device_v1  *dc_dev;

    int frame_ready, frame_failed;
    int probed;      /* 是否已经做过 shm 格式 roundtrip */
    int fb_w, fb_h;
    uint32_t fb_format;
};

static struct capture cap;   /* 全局，注入函数里要用 */

static void shm_format(void *d, struct wl_shm *s, uint32_t f) { (void)d; (void)s; (void)f; }
static const struct wl_shm_listener shm_listener = { shm_format };

static void seat_caps(void *d, struct wl_seat *s, uint32_t c) { (void)d; (void)s; (void)c; }
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version) {
    struct capture *c = data;
    if (!strcmp(iface, "wl_shm")) {
        c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        wl_shm_add_listener(c->shm, &shm_listener, c);
    } else if (!strcmp(iface, "wl_output") && !c->output) {
        c->output = wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4);
    } else if (!strcmp(iface, "wl_seat") && !c->seat) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(c->seat, &seat_listener, c);
    } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        c->mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface,
                                  version < 3 ? version : 3);
    } else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) {
        c->ptr_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface,
                                      version < 2 ? version : 2);
    } else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        c->kbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name)) {
        c->dc_mgr = wl_registry_bind(reg, name, &zwlr_data_control_manager_v1_interface,
                                     version < 1 ? version : 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = { on_global, on_global_remove };

static void ev_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    struct capture *c = data;
    (void)f; (void)stride;
    if (!c->probed) {
        /* 合成器枚举 shm 格式（wl_shm.format 事件）会排在我们发起的请求之前。
         * 这里补一次 roundtrip，保证 wl_shm 侧也把格式表读完，
         * 否则后续给合成器的 buffer 可能因为格式校验未就绪被直接销毁
         * （踩过：虚拟输入全部静默失效，抓帧却看起来正常）。 */
        wl_display_roundtrip(c->display);
        c->probed = 1;
    }
    c->fb_w = (int)w; c->fb_h = (int)h; c->fb_format = format;
}
static void ev_ready(void *data, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t a, uint32_t b, uint32_t c) {
    (void)f; (void)a; (void)b; (void)c;
    ((struct capture *)data)->frame_ready = 1;
}
static void ev_failed(void *data, struct zwlr_screencopy_frame_v1 *f) {
    (void)f;
    struct capture *c = data;
    c->frame_failed = 1; c->frame_ready = 1;
}
/* 未实现的事件必须给空函数：写 NULL 会被 libwayland abort */
static void n1(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a) { (void)d;(void)f;(void)a; }
static void n3(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{ (void)d;(void)f;(void)a;(void)b;(void)c; }
static void n4(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c, uint32_t e)
{ (void)d;(void)f;(void)a;(void)b;(void)c;(void)e; }
static void n0(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d;(void)f; }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = ev_buffer, .flags = n1, .ready = ev_ready, .failed = ev_failed,
    .damage = n4, .linux_dmabuf = n3, .buffer_done = n0,
};

/* ============================================================ 帧缓冲 */

struct shmbuf {
    int fd;
    uint8_t *addr;
    size_t size;
    struct wl_buffer *wl_buf;
};

static int create_shmbuf(struct capture *c, struct shmbuf *b, int w, int h, uint32_t fmt) {
    size_t stride = (size_t)w * 4;
    b->size = stride * (size_t)h;
    b->fd = memfd_create("wstream", MFD_CLOEXEC);
    if (b->fd < 0) return -1;
    if (ftruncate(b->fd, (off_t)b->size) < 0) return -1;
    b->addr = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->addr == MAP_FAILED) return -1;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, b->fd, (int32_t)b->size);
    b->wl_buf = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride, fmt);
    wl_shm_pool_destroy(pool);
    return b->wl_buf ? 0 : -1;
}

/* ============================================================ 输入注入 */

/* 最小但完整的键位表：声明 pc+us+evdev，让合成器接受 evdev keycode 直传 */
/* 存下来的 keymap 文本（-v 时会落盘方便排查） */
static char g_keymap_text[8192];
static int  g_keymap_len;

static void input_init(struct capture *c) {
    if (!c->ptr_mgr || !c->kbd_mgr || !c->seat) {
        logf_("⚠️ 合成器缺虚拟输入协议（ptr=%p kbd=%p seat=%p）→ 只读模式",
              (void *)c->ptr_mgr, (void *)c->kbd_mgr, (void *)c->seat);
        return;
    }
    c->kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c->kbd_mgr, c->seat);
    if (!c->kbd) { logf_("⚠️ 创建虚拟键盘失败"); return; }

    /* ⚠️ 这里必须真的创建虚拟指针，否则 cap.ptr 一直是 NULL，
     * inject_move() / inject_button() 第一行 `if (!cap.ptr) return;`
     * 就把所有鼠标事件静默丢掉了 —— 表现为「画面能看、键盘能打、
     * 但鼠标点了没反应」，而日志却还骗人地说「虚拟指针已就绪」。
     * seat 为 NULL 时传 NULL，协议允许（合成器自己选默认 seat）。 */
    c->ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(c->ptr_mgr, c->seat);
    if (!c->ptr) logf_("⚠️ 创建虚拟指针失败 → 只能键盘操作，鼠标不可用");

    static char km[8192];
    /* 用完整的 evdev 布局：手搓的「每个 keycode 只有一个 keysym」表虽然能过
     * xkbcommon 校验，但会给终端发一串怪异的控制序列，还会丢掉后面的按键。
     * include 形式由 libxkbcommon 自己展开，稳定。 */
    int len = snprintf(km, sizeof km,
        "xkb_keymap {\n"
        "xkb_keycodes  \"x\" { include \"evdev+aliases(qwerty)\" };\n"
        "xkb_types     \"x\" { include \"complete\" };\n"
        "xkb_compatibility \"x\" { include \"complete\" };\n"
        "xkb_symbols   \"x\" { include \"pc+us+inet(evdev)\" };\n"
        "};\n");
    if (len > 0 && (size_t)len < sizeof km) {
        memcpy(g_keymap_text, km, (size_t)len);
        g_keymap_len = len;
    }
    if (len <= 0 || (size_t)len >= sizeof km) { logf_("⚠️ 生成 keymap 失败"); return; }
    if (g_verbose) { FILE *dbg = fopen("/tmp/wstream-keymap.txt", "w"); if (dbg) { fwrite(km, 1, (size_t)len, dbg); fclose(dbg); } }

    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)len) == 0) {
            void *m = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) { memcpy(m, km, (size_t)len); munmap(m, (size_t)len); }
            zwp_virtual_keyboard_v1_keymap(c->kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                           fd, (uint32_t)len);
            /* 关键：keymap 必须先送达合成器，按键事件才有意义。
             * 一个 roundtrip 保证 keymap 已被处理，再发 key。 */
            wl_display_roundtrip(c->display);
        }
        close(fd);
    }
    wl_display_flush(c->display);
    g_keymap_fresh = 1;   /* input_init 里已经推过完整 evdev keymap */
    logf_("✅ 虚拟键盘%s已就绪（可交互）", c->ptr ? " + 虚拟指针" : "（鼠标不可用）");
}

/* 虚拟指针的绝对定位。
 *
 * ⚠️ 为什么用「相对位移累积」而不是 motion_absolute：
 *   zwlr_virtual_pointer_v1 虽然有 motion_absolute 请求，但 wlroots 只把它
 *   转成事件抛给合成器，**要不要实现取决于合成器**。labwc 只接了
 *   wlr_seat_pointer_notify_motion（相对位移），没有实现 absolute —— 所以
 *   motion_absolute 发出去会被静默丢弃：日志显示请求正常发送、合成器也收到了，
 *   但光标纹丝不动，点击因此全部落在初始位置。这是「键盘能用、鼠标点不动」的根因。
 *   实测（WAYLAND_DEBUG）确认请求格式无误，所以只能改用 motion(dx,dy) 累积定位。
 *
 * 坐标约定：vx/vy 是 0..65535 的归一化值，W/H 是输出尺寸。 */
/* 光标定位。
 *
 * ⚠️ 这里从「相对位移累积」改成了 motion_absolute，原因是一个实测出来的 bug：
 *   相对位移的实现是 `dx = 目标 - 我们以为的当前位置`，而「当前位置」的初值
 *   被假定为 (0,0)。**合成器重启后光标并不在 (0,0)** —— 实测初始在 (99,99)，
 *   于是每次点击都偏移 (99,99)：点 (1067,360) 实际落在 (1166,459)。
 *   这种偏移还会随操作累积，表现为「点不准、越点越偏」。
 *
 *   改成 absolute 后不再依赖任何「当前位置」的假设，直接把归一化坐标和
 *   输出尺寸交给合成器。sway 完整实现了 motion_absolute（labwc 不支持，
 *   真要在 labwc 上跑，见 docs/PROTOCOLS.md 的降级说明）。
 *
 * ⚠️ 协议里的 x/y 是**像素坐标**，不是 0..65535 归一化值。协议原文：
 *      "Value of x can range from 0 to x_extent, value of y can range from 0 to y_extent."
 *    一开始想当然按 0..65535 传，结果坐标全部超出 extent 被钳到边缘，
 *    表现为「光标只会在角落、点击位置全错」。实测对照：
 *      motion_absolute(640,360, 1280,720) → 探针收到 (638,358)  ✅ 差 2px 是窗口边框
 *      motion_absolute(32000,32000, 1280,720) → 无任何移动        ❌ 超范围
 *
 * 所以前端仍发 0..65535（与画面尺寸无关，缩放窗口也不会错），
 * 在这里换算成像素。 */
static void inject_move(int vx, int vy, int eW, int eH) {
    if (!cap.ptr || eW <= 0 || eH <= 0) return;
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > 65535) vx = 65535;
    if (vy > 65535) vy = 65535;
    /* 归一化 → 像素，并夹到 [0, extent-1] */
    int tx = (int)((long long)vx * eW / 65535);
    int ty = (int)((long long)vy * eH / 65535);
    if (tx >= eW) tx = eW - 1;
    if (ty >= eH) ty = eH - 1;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    zwlr_virtual_pointer_v1_motion_absolute(cap.ptr, now_ms(),
        (uint32_t)tx, (uint32_t)ty, (uint32_t)eW, (uint32_t)eH);
    zwlr_virtual_pointer_v1_frame(cap.ptr);
    wl_display_flush(cap.display);
}

/* 按钮码映射。
 *
 * ⚠️ 协议要的是 **Linux input-event-codes 里的 BTN_***（0x110 起），
 *   不是前端那种「1=左 2=中 3=右」的习惯编号。直接把 1/2/3 发过去，
 *   合成器会当成 BTN_LEFT 之外的怪键 —— 实测 X11 客户端收到的是
 *   button=104 / 96 / 197 这种垃圾值，点击自然没反应。
 *
 * 前端仍发 1/2/3（网页习惯），在这里翻译：
 *   1 → BTN_LEFT(0x110)  2 → BTN_MIDDLE(0x112)  3 → BTN_RIGHT(0x111)
 * 也接受直接给 0x110 这类标准码，方便脚本直接用。
 */
static uint32_t map_button(uint32_t b) {
    switch (b) {
    case 1: return 0x110;   /* BTN_LEFT */
    case 2: return 0x112;   /* BTN_MIDDLE */
    case 3: return 0x111;   /* BTN_RIGHT */
    case 4: return 0x113;   /* BTN_SIDE */
    case 5: return 0x114;   /* BTN_EXTRA */
    default: return b;      /* 已经是标准码 */
    }
}

static void inject_button(uint32_t btn, uint32_t down) {
    if (!cap.ptr) return;
    uint32_t code = map_button(btn);
    zwlr_virtual_pointer_v1_button(cap.ptr, now_ms(), code, down ? 1 : 0);
    zwlr_virtual_pointer_v1_frame(cap.ptr);
    wl_display_flush(cap.display);
}

static void inject_scroll(int steps) {
    if (!cap.ptr) return;
    zwlr_virtual_pointer_v1_axis_source(cap.ptr, 0 /* wheel */);
    zwlr_virtual_pointer_v1_axis_discrete(cap.ptr, now_ms(), 0 /* vertical */,
        wl_fixed_from_int(steps * 10), steps);
    zwlr_virtual_pointer_v1_frame(cap.ptr);
    wl_display_flush(cap.display);
}

static uint32_t g_mods;
static volatile int g_input_dirty;   /* 有输入事件待 roundtrip（只在主线程清） */



/* 与 wtype 相同的做法：为「单个 evdev keycode」生成一张只含它的 keymap 并通过
 * memfd 传给合成器，紧贴在 key 事件之前。返回时 memfd 已关闭（合成器已 mmap）。 */
/* 推送「完整的 evdev 布局」keymap。要点全是踩出来的：
 *   1) 不能只列单个键位（wtype 那样）：合成器按这张表重编 xkb，其余键位会变成
 *      NoSymbol，打一个键之后整个键盘就没反应了；
 *   2) 多个虚拟键盘并存时，生效的是"最后一次推送"的那张表；所以每一键都推一次，
 *      盖掉别人（wtype/wshowkeys）留下的残缺表；
 *   3) 顺序必须 keymap → key：合成器一收到 keymap 就标记可用，随后的 key 才被处理。
 * 传 memfd 后立即 close，合成器已 mmap。 */
int push_keymap_snapshot(struct capture *cap, uint32_t evdev_code) {
    (void)evdev_code;
    if (!cap->kbd) return -1;
    static const char KM[] =
        "xkb_keymap {\n"
        "xkb_keycodes \"x\" { include \"evdev+aliases(qwerty)\" };\n"
        "xkb_types \"x\" { include \"complete\" };\n"
        "xkb_compatibility \"x\" { include \"complete\" };\n"
        "xkb_symbols \"x\" { include \"pc+us+inet(evdev)\" };\n"
        "};\n";
    int len = (int)sizeof KM - 1;
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, len) != 0) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    memcpy(m, KM, (size_t)len);
    munmap(m, (size_t)len);
    zwp_virtual_keyboard_v1_keymap(cap->kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
    return 0;
}

static void inject_key(uint32_t code, uint32_t down) {
    if (!cap.kbd) return;
    /* ⚠️ 不要加 8。zvp_virtual_keyboard_v1.key 的 code 参数是「evdev 扫描码」，
     * 由合成器自己 +8 转成 xkb 键码。加了 8 等于让合成器再 +8 一次，
     * 结果就是你敲 a 出来的是 8 个键位之后那个键（实测：abc → "l]c"，b → "m"）。
     * 详见 inject_key 末尾的说明。 */

    /* mods: 低 8 位是「当前按住」的修饰键，高 8 位是「有效」的修饰键。
     * 这两个值不对的话，Ctrl+C / Shift+Tab 这类组合键到不了应用。 */
    uint32_t mods = 0;
    if (code == 29 || code == 97) mods = 4;        /* 左 Ctrl / 右 Ctrl → bit2 */
    else if (code == 42 || code == 54) mods = 1;   /* 左 Shift / 右 Shift → bit0 */
    else if (code == 56 || code == 100) mods = 8;  /* 左 Alt / 右 Alt → bit3 */
    else if (code == 125 || code == 126) mods = 64;/* 左 Meta / 右 Meta → bit6 */

    /* 先算出新的修饰位状态，但**不要马上发** —— 见下面 push_keymap 之后的说明。 */
    if (mods) {
        if (down) g_mods |= mods; else g_mods &= ~mods;
    }
    /* 只在 keymap 不新鲜时重推（初次 / Unicode 注入之后）。
     * ⚠️ 不要每键都推：推 keymap 会清零修饰位，组合键直接失效。
     * 详见 g_keymap_fresh 的说明。 */
    if (!g_keymap_fresh) {
        push_keymap_snapshot(&cap, code);
        g_keymap_fresh = 1;
        g_mods_sent = 0;   /* 新 keymap 会让合成器清掉修饰位，我们也要同步 */
    }
    /* 🔑 这里是整个键盘问题真正的根因（2026-10-23 实测坐实）：
     *   zvp_virtual_keyboard_v1.key(code) 里的 code 是 **evdev 扫描码**，
     *   wlroots 收到后自己 +8 得到 xkb keycode（evdev 30 = 'a' → xkb 38）。
     *   如果这里再 +8（历史上误以为协议要求和 wl_keyboard 一样要键码），
     *   合成器就会 +16：a(30)→46('l')、b(48)→56、c(46)→54。
     *   屏幕上的现象正好是 abc 打成 "l]c"，b 打成 "m"，与实测逐字节吻合。
     *   另一路证据：最小复现程序不 +8 时永远正确，一 +8 立刻错乱。
     *   所以：直接发 code，绝不加 8。 */
    /* ⚠️ modifiers 必须在 push_keymap **之后**发（2026-10-24 实测坐实）。
     * 推新 keymap 会让合成器重建键盘状态，修饰位被清零。如果在推 keymap
     * 之前发 modifiers，组合键就失效：Ctrl+V 变成傻按一个 v ——
     * 实测现象是「剪贴板里明明是中文，终端里却多出一个字母 v」。
     * 顺序必须是：keymap → modifiers → key。 */
    /* ⚠️ 必须「变了就发」，不能只在 g_mods 非零时发。
     * 踩过：`if (g_mods)` 导致修饰键全部释放时（g_mods 归 0）**不发** modifiers，
     * 合成器就一直以为 Shift 是按下的 —— 现象是打 "Upper" 得到 "UPPER"。
     * 所以比较「上次发出去的值」，不同才发（避免每键都发，但该发的一定发）。 */
    if (g_mods != g_mods_sent) {
        zwp_virtual_keyboard_v1_modifiers(cap.kbd, g_mods, 0, 0, 0);
        g_mods_sent = g_mods;
    }
    zwp_virtual_keyboard_v1_key(cap.kbd, now_ms(), code, down ? 1 : 0);
    /* 注入发生在网络线程，不能在这里 roundtrip（主线程才 dispatch）。
     * 只 flush，主线程看到 g_input_dirty 会补一次 roundtrip。 */
    wl_display_flush(cap.display);
    g_input_dirty = 1;
}

/* ---------- ASCII → evdev 键码（服务端自己的表，供文本注入用） ---------- */

/* 返回 evdev 扫描码；0 = 该字符没有对应键 */
static uint32_t ascii_to_evdev(int c) {
    static const uint8_t lower[26] = {
        30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44
    };
    static const uint8_t digits[10] = { 11,2,3,4,5,6,7,8,9,10 };
    if (c >= 'a' && c <= 'z') return lower[c - 'a'];
    if (c >= 'A' && c <= 'Z') return lower[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case ' ':  return 57;
    case '\n': return 28;
    case '\t': return 15;
    case '-':  return 12;  case '_': return 12;
    case '=':  return 13;  case '+': return 13;
    case '[':  return 26;  case '{': return 26;
    case ']':  return 27;  case '}': return 27;
    case '\\': return 43;  case '|': return 43;
    case ';':  return 39;  case ':': return 39;
    case '\'': return 40;  case '"': return 40;
    case '`':  return 41;  case '~': return 41;
    case ',':  return 51;  case '<': return 51;
    case '.':  return 52;  case '>': return 52;
    case '/':  return 53;  case '?': return 53;
    case '!':  return 2;   case '@': return 3;   case '#': return 4;
    case '$':  return 5;   case '%': return 6;   case '^': return 7;
    case '&':  return 8;   case '*': return 9;   case '(': return 10;
    case ')':  return 11;
    default:   return 0;
    }
}

/* 该 ASCII 字符是否需要 Shift（大写字母 / 上档符号） */
static int ascii_needs_shift(int c) {
    if (c >= 'A' && c <= 'Z') return 1;
    switch (c) {
    case '!': case '@': case '#': case '$': case '%': case '^':
    case '&': case '*': case '(': case ')':
    case '_': case '+': case '{': case '}': case '|': case ':':
    case '"': case '~': case '<': case '>': case '?':
        return 1;
    default:
        return 0;
    }
}

/* ============================================================ 剪贴板同步
 *
 * 用 zwlr_data_control（不是 wl_data_device）：
 *   wl_data_device 要求 surface 有焦点才能 set_selection，而 wstream 根本没有
 *   窗口（这是项目的核心设计）。data-control 是专门给「剪贴板管理器」用的协议，
 *   不需要焦点 —— 实测在无头 sway 里 `wl-copy`/`wl-paste` 完全正常。
 *
 * 两个方向：
 *   沙箱 → 浏览器：合成器发 selection 事件 → 我们 receive 数据 → 通过 WS 发给页面
 *   浏览器 → 沙箱：页面发 type 7 → 我们 create_data_source + set_selection
 *
 * ⚠️ 沙箱剪贴板和宿主机剪贴板是**真隔离**的（实测：沙箱里写的内容，
 *    宿主机 wl-paste 读不到，读到的是用户自己的内容）。所以这不涉及
 *    往用户桌面泄漏任何东西 —— 桥接的是「浏览器页面」和「沙箱」。
 *
 * 数据大小上限：超过 CLIP_MAX 就截断。剪贴板里可能是几个 G 的文件，
 * 不能无脑往内存里灌。
 */
#define CLIP_MAX (256 * 1024)

/* 沙箱当前剪贴板内容（我们读到之后缓存一份，页面连上时直接给） */
static char   g_clip_text[CLIP_MAX];
static size_t g_clip_len;
static int    g_clip_ver;          /* 每次变化 +1，页面据此判断要不要拉 */
static pthread_mutex_t g_clip_mu = PTHREAD_MUTEX_INITIALIZER;

/* 浏览器最近一次推过来的文本，作为「沙箱剪贴板的来源」 */
static char   g_clip_out[CLIP_MAX];
static size_t g_clip_out_len;
static struct zwlr_data_control_source_v1 *g_clip_source;   /* 当前持有者 */

/* 正在 receive 的 fd：读完后把数据塞进 g_clip_text */
static void clip_read_fd(int fd) {
    char buf[8192];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        if (total + (size_t)n >= CLIP_MAX) {   /* 超限截断 */
            size_t room = CLIP_MAX - 1 - total;
            if (room > 0) { memcpy(g_clip_text + total, buf, room); total += room; }
            break;
        }
        memcpy(g_clip_text + total, buf, (size_t)n);
        total += (size_t)n;
    }
    close(fd);
    pthread_mutex_lock(&g_clip_mu);
    g_clip_text[total] = 0;
    g_clip_len = total;
    g_clip_ver++;
    pthread_mutex_unlock(&g_clip_mu);
    if (g_verbose) logf_("📋 沙箱剪贴板更新：%zu 字节", total);
}

/* ---------- offer 监听：收集 mime，等 selection 之后再读 ----------
 *
 * ⚠️ 关键顺序（踩过，日志里表现为一堆 `discarded ...offer("text/plain")`）：
 *   合成器发事件的顺序是
 *       data_offer(offer对象)  →  offer("text/plain") × N  →  selection(offer)
 *   所以**监听器必须在 data_offer 里就加上**。如果等到 selection 里才 add_listener，
 *   前面那些 offer 事件已经到达、没有监听者，被 libwayland 直接 discard，
 *   我们就永远不知道有哪些 mime 可用，也就读不到数据。
 *
 * 每个 offer 对象配一个 clip_offer_ctx 记录它支持哪些 mime。 */

#define OFFER_MAX_MIME 8
#define OFFER_MIME_LEN 64

struct clip_offer_ctx {
    char mime[OFFER_MAX_MIME][OFFER_MIME_LEN];
    int  n;
    struct clip_offer_ctx *next;   /* 单向链表，方便按对象查找 */
};
static struct clip_offer_ctx *g_offers;

static void offer_mime(void *data, struct zwlr_data_control_offer_v1 *offer,
                       const char *mime) {
    struct clip_offer_ctx *ctx = data;
    (void)offer;
    if (!ctx || ctx->n >= OFFER_MAX_MIME) return;
    snprintf(ctx->mime[ctx->n], OFFER_MIME_LEN, "%s", mime);
    ctx->n++;
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    .offer = offer_mime,
};

/* 从收集到的 mime 里挑一个能读文本的 */
static const char *pick_text_mime(struct clip_offer_ctx *ctx) {
    if (!ctx) return NULL;
    /* 优先级：明确 UTF-8 的最可靠 */
    static const char *pref[] = {
        "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"
    };
    for (size_t p = 0; p < sizeof pref / sizeof pref[0]; p++)
        for (int i = 0; i < ctx->n; i++)
            if (!strcmp(ctx->mime[i], pref[p])) return ctx->mime[i];
    return NULL;
}

/* ---------- device 监听：selection 变化 ---------- */
static void dc_data_offer(void *data, struct zwlr_data_control_device_v1 *dev,
                          struct zwlr_data_control_offer_v1 *offer) {
    (void)dev;
    struct capture *c = data;
    /* 立刻挂监听器并记下上下文（对象在 selection 里还会用到） */
    struct clip_offer_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return;
    ctx->next = g_offers;
    g_offers = ctx;
    zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, ctx);
    /* 把 ctx 关联到 offer 对象，selection 时反查 */
    zwlr_data_control_offer_v1_set_user_data(offer, ctx);
    (void)c;
}

static void dc_selection(void *data, struct zwlr_data_control_device_v1 *dev,
                         struct zwlr_data_control_offer_v1 *offer) {
    (void)data; (void)dev;
    if (!offer) {   /* NULL = 剪贴板被清空 */
        pthread_mutex_lock(&g_clip_mu);
        g_clip_len = 0; g_clip_text[0] = 0; g_clip_ver++;
        pthread_mutex_unlock(&g_clip_mu);
        return;
    }
    struct clip_offer_ctx *ctx = zwlr_data_control_offer_v1_get_user_data(offer);
    /* offer 事件可能还没处理完，先做一次 roundtrip 让它们到齐 */
    wl_display_roundtrip(cap.display);
    const char *mime = pick_text_mime(ctx);
    if (!mime) {
        if (g_verbose) logf_("📋 剪贴板里没有文本类型（%d 种 mime）", ctx ? ctx->n : 0);
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) return;
    zwlr_data_control_offer_v1_receive(offer, mime, fds[1]);
    close(fds[1]);
    /* receive 之后要 roundtrip，合成器才会把数据写进管道 */
    wl_display_roundtrip(cap.display);
    clip_read_fd(fds[0]);
}
static void dc_finished(void *data, struct zwlr_data_control_device_v1 *dev) {
    (void)data; (void)dev;
    logf_("⚠️ 剪贴板 device 失效（合成器不再支持 data-control）");
}
static void dc_primary(void *data, struct zwlr_data_control_device_v1 *dev,
                       struct zwlr_data_control_offer_v1 *offer) {
    (void)data; (void)dev; (void)offer;   /* 主选区暂不处理 */
}
static const struct zwlr_data_control_device_v1_listener dc_dev_listener = {
    .data_offer = dc_data_offer,
    .selection = dc_selection,
    .finished = dc_finished,
    .primary_selection = dc_primary,
};

/* ---------- source 监听：浏览器推来的文本，合成器来取 ---------- */
static void src_send(void *data, struct zwlr_data_control_source_v1 *src,
                     const char *mime, int32_t fd) {
    (void)data; (void)src;
    if (g_verbose) logf_("📤 有人来取剪贴板：mime=%s", mime);
    /* 不管它要什么 mime，都给同一份 UTF-8 文本 */
    if (!strncmp(mime, "text/", 5) || !strcmp(mime, "UTF8_STRING") ||
        !strcmp(mime, "STRING") || !strcmp(mime, "TEXT")) {
        size_t off = 0;
        while (off < g_clip_out_len) {
            ssize_t n = write(fd, g_clip_out + off, g_clip_out_len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
    }
    close(fd);
}
static void src_cancelled(void *data, struct zwlr_data_control_source_v1 *src) {
    (void)data;
    if (src == g_clip_source) g_clip_source = NULL;
    zwlr_data_control_source_v1_destroy(src);
}
static const struct zwlr_data_control_source_v1_listener src_listener = {
    .send = src_send,
    .cancelled = src_cancelled,
};

/* 初始化剪贴板桥。失败不影响其它功能。 */
static void clipboard_init(struct capture *c) {
    if (!c->dc_mgr || !c->seat) {
        logf_("ℹ️ 合成器不支持 data-control → 剪贴板同步关闭");
        return;
    }
    c->dc_dev = zwlr_data_control_manager_v1_get_data_device(c->dc_mgr, c->seat);
    if (!c->dc_dev) { logf_("⚠️ 创建剪贴板 device 失败"); return; }
    zwlr_data_control_device_v1_add_listener(c->dc_dev, &dc_dev_listener, c);
    wl_display_roundtrip(c->display);   /* 拿一次当前剪贴板 */
    logf_("✅ 剪贴板同步已就绪（data-control）");
}

/* 浏览器 → 沙箱：把文本设成沙箱剪贴板。
 * **只在主线程调用**（要 roundtrip）。返回 0 成功。 */
static int clipboard_set(const char *text, size_t n) {
    if (!cap.dc_mgr || !cap.dc_dev) return -1;
    if (n >= CLIP_MAX) n = CLIP_MAX - 1;
    memcpy(g_clip_out, text, n);
    g_clip_out[n] = 0;
    g_clip_out_len = n;

    struct zwlr_data_control_source_v1 *src =
        zwlr_data_control_manager_v1_create_data_source(cap.dc_mgr);
    if (!src) return -1;
    zwlr_data_control_source_v1_add_listener(src, &src_listener, NULL);
    /* 声明多种 mime，兼容 GTK / Qt / 终端 */
    zwlr_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
    zwlr_data_control_source_v1_offer(src, "text/plain");
    zwlr_data_control_source_v1_offer(src, "UTF8_STRING");
    zwlr_data_control_source_v1_offer(src, "STRING");
    zwlr_data_control_device_v1_set_selection(cap.dc_dev, src);
    if (g_clip_source && g_clip_source != src)
        zwlr_data_control_source_v1_destroy(g_clip_source);
    g_clip_source = src;
    wl_display_roundtrip(cap.display);
    if (g_verbose) logf_("📋 浏览器 → 沙箱剪贴板：%zu 字节", n);
    return 0;
}

/* 取当前沙箱剪贴板快照（给 WS 用）。返回长度，out 已 NUL 结尾。 */
static size_t clipboard_get(char *out, size_t cap, int *ver) {
    size_t n;
    pthread_mutex_lock(&g_clip_mu);
    n = g_clip_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, g_clip_text, n);
    out[n] = 0;
    if (ver) *ver = g_clip_ver;
    pthread_mutex_unlock(&g_clip_mu);
    return n;
}

/* ---------- 任意 Unicode 字符注入（中文 / emoji / 任意符号） ----------
 *
 * 为什么不能靠 evdev 键码：键盘布局里根本没有「中」这个键。要打非 ASCII，
 * 唯一可行的路子是**临时换一张只含该字符的 keymap**，再按对应的键码。
 * 这也是 wtype 的做法（实测抓到的 keymap 原文）：
 *
 *     xkb_keycodes "(unnamed)" { minimum = 8; maximum = 10; <K1> = 9; };
 *     xkb_symbols  "(unnamed)" { key <K1> {[U4E2D]}; };      // U4E2D = 「中」
 *
 * `UXXXX` 是 xkb 的 Unicode keysym 写法，合成器会把它交给客户端当作文本。
 *
 * ⚠️ 顺序仍然是 keymap → key，且必须一个 roundtrip 让 keymap 先落地。
 * ⚠️ 用完必须把完整的 evdev keymap 推回去（push_keymap_snapshot），
 *    否则后续普通按键会落到这张「只有一个键」的表上，全部失效。
 *    这是 wtype 的已知毛病，我们不能重犯。
 *
 * 返回 0 成功，-1 失败。调用方在**主线程**（需要 roundtrip）。 */
static int inject_unicode_codepoint(uint32_t cp) {
    if (!cap.kbd || cp == 0 || cp > 0x10FFFF) return -1;
    /* 代理区不是合法码点 */
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;

    /* 键码固定用 evdev 1（xkb keycode 9），与 wtype 一致。
     * keycodes 段声明 minimum/maximum 只覆盖这一个键，避免残留其它键位。 */
    char km[512];
    int len = snprintf(km, sizeof km,
        "xkb_keymap {\n"
        "xkb_keycodes \"(unnamed)\" { minimum = 8; maximum = 10; <K1> = 9; };\n"
        "xkb_types \"(unnamed)\" { include \"complete\" };\n"
        "xkb_compatibility \"(unnamed)\" { include \"complete\" };\n"
        "xkb_symbols \"(unnamed)\" { key <K1> {[U%04X]}; };\n"
        "};\n", cp);
    if (len <= 0 || (size_t)len >= sizeof km) return -1;

    int fd = memfd_create("wstream-uni", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) != 0) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    memcpy(m, km, (size_t)len);
    munmap(m, (size_t)len);
    zwp_virtual_keyboard_v1_keymap(cap.kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   fd, (uint32_t)len);
    close(fd);
    /* keymap 必须先被合成器处理完，key 才有意义 */
    wl_display_roundtrip(cap.display);

    /* 按一下这个键。code=1 是 evdev 扫描码，合成器内部 +8 → xkb 9。 */
    zwp_virtual_keyboard_v1_key(cap.kbd, now_ms(), 1, 1);
    zwp_virtual_keyboard_v1_key(cap.kbd, now_ms(), 1, 0);
    wl_display_flush(cap.display);
    return 0;
}

/* 解一个 UTF-8 字符，返回码点并推进 *pp；非法序列返回 0xFFFD 并只前进 1 字节 */
static uint32_t utf8_next(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    if (p >= end) return 0;
    uint8_t c = *p;
    if (c < 0x80) { *pp = p + 1; return c; }

    int extra; uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { *pp = p + 1; return 0xFFFD; }

    if (p + extra >= end) { *pp = p + 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p + 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + extra + 1;
    return cp;
}

/* 注入一整段 UTF-8 文本。**只在主线程调用**（内部会 roundtrip）。
 * 返回实际注入的字符数。 */
static int inject_text(const char *s, size_t n) {
    if (!cap.kbd) return 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + n;
    int count = 0;
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        if (cp == 0 || cp == 0xFFFD) continue;
        /* 换行/制表符用普通键更可靠（很多应用对 U000A 不响应） */
        if (cp == '\n') { inject_key(28, 1); inject_key(28, 0); count++; continue; }
        if (cp == '\t') { inject_key(15, 1); inject_key(15, 0); count++; continue; }
        /* 纯 ASCII 走原来的键码路径：更快，且不折腾 keymap */
        if (cp < 0x80) {
            uint32_t code = ascii_to_evdev((int)cp);
            if (code) {
                int shift = ascii_needs_shift((int)cp);
                if (shift) inject_key(42, 1);
                inject_key(code, 1);
                inject_key(code, 0);
                if (shift) inject_key(42, 0);
                count++;
                continue;
            }
            /* 没有键码的 ASCII（少见）也走 Unicode 路径 */
        }
        if (inject_unicode_codepoint(cp) == 0) count++;
        /* 每个字符之间留一点时间，否则终端会丢字 */
        struct timespec ts = {0, 12 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    /* ⚠️ 关键收尾：把完整 evdev keymap 推回去，否则后续普通按键全失效 */
    if (count > 0) {
        push_keymap_snapshot(&cap, 0);
        g_keymap_fresh = 1;
        g_mods_sent = 0;
        wl_display_roundtrip(cap.display);
    }
    return count;
}

/* 把一个 BGRX 帧存成 PPM：没有浏览器时也能核对画面正确性 */
static int save_ppm(const uint8_t *bgrx, int w, int h, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t *s2 = bgrx + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x*3+0] = s2[x*4+2];
            row[x*3+1] = s2[x*4+1];
            row[x*3+2] = s2[x*4+0];
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* ============================================================ SHA1 + Base64（WS 握手） */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_t *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    sha1_t s = {{0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0}, 0, {0}, 0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(&s, msg + i);
    uint8_t tail[128]; size_t r = len - i;
    memcpy(tail, msg + i, r);
    tail[r] = 0x80;
    size_t pad = (r + 9 <= 64) ? 64 : 128;
    memset(tail + r + 1, 0, pad - r - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[pad - 1 - k] = (uint8_t)(bits >> (k * 8));
    for (size_t o = 0; o < pad; o += 64) sha1_block(&s, tail + o);
    for (int k = 0; k < 5; k++) {
        out[k*4]   = (uint8_t)(s.h[k] >> 24);
        out[k*4+1] = (uint8_t)(s.h[k] >> 16);
        out[k*4+2] = (uint8_t)(s.h[k] >> 8);
        out[k*4+3] = (uint8_t)s.h[k];
    }
}

static void ws_accept_key(const char *key, char *out /* >=29 */) {
    static const char *MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s%s", key, MAGIC);
    uint8_t d[20];
    sha1((const uint8_t *)tmp, strlen(tmp), d);
    static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 20; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < 20) v |= (uint32_t)d[i+1] << 8;
        if (i + 2 < 20) v |= d[i+2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < 20) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < 20) ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

/* ============================================================ HTTP / WS 客户端管理 */

#define MAX_CLIENTS 8
#define WS_BIN 2

struct client {
    int fd;                 /* -1 = 空槽 / 已死，等回收 */
    int is_stream;
    int is_ws;
    unsigned long id;       /* 槽位会被复用，id 单调递增永不重用 */
    uint8_t in[16384];
    size_t in_len;
};

static struct client clients[MAX_CLIENTS];
static int nclients;
static unsigned long g_client_id;   /* 只在持锁时递增 */
static pthread_mutex_t g_cl_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- 输入所有权 ----------
 *
 * 多客户端同时连的时候，如果谁都能注入，鼠标会互相拉扯：A 把光标移到左边、
 * B 同时移到右边，结果两边都觉得「点了没反应」。所以同一时刻**只允许一个
 * 客户端持有输入权**。
 *
 * 规则（类似 VNC 的单控制器模型）：
 *   - 第一个发来 type 6 do_take 的客户端成为持有者
 *   - 持有者断开 → 自动释放，谁先抢谁得
 *   - 显式 type 6 do_release → 释放
 *   - **非持有者的输入事件一律丢弃**（这是关键：在前端隐藏按钮不够，
 *     必须服务端强制，否则随便一个脚本都能绕过）
 *
 * g_owner 是 client id（0 = 无人持有）。读写都要持 g_cl_mu。 */
static unsigned long g_owner;

/* 当前调用者是不是输入持有者。ws_read 在持锁状态下调用。 */
static int is_owner(unsigned long id) { return id != 0 && id == g_owner; }


/* 按 id 反查槽位。-1 = 已不存在。**调用方必须持有 g_cl_mu。**
 *
 * 为什么要 id 而不是下标：poll() 前记下标、poll() 回来后按下标访问，
 * 中间主线程的 push_frame() 可能 drop_client()（把末尾元素搬进被删的槽），
 * 于是那个下标指向了另一个客户端，甚至 >= nclients 越界读。
 * 拿 fd 也有同样的问题（fd 会被内核复用）。id 单调递增，不会和任何旧对象混淆。 */
static int client_by_id(unsigned long id) {
    if (!id) return -1;
    for (int i = 0; i < nclients; i++)
        if (clients[i].id == id) return i;
    return -1;
}

/* 注册一个新客户端；返回 id，0 = 满了。**调用方必须持有 g_cl_mu。** */
static unsigned long client_add(int fd, int is_stream, int is_ws) {
    if (nclients >= MAX_CLIENTS) return 0;
    struct client *c = &clients[nclients++];
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->is_stream = is_stream;
    c->is_ws = is_ws;
    c->id = ++g_client_id;
    if (!c->id) c->id = ++g_client_id;   /* 回绕保护（实际不可能） */
    return c->id;
}

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* **调用方必须持有 g_cl_mu。** */
static void drop_client(int i) {
    if (g_verbose) logf_("[-] 客户端断开 (stream=%d ws=%d)", clients[i].is_stream, clients[i].is_ws);
    /* 持有者走了就把输入权释放掉，否则沙箱会一直没人能操作 */
    if (clients[i].id && clients[i].id == g_owner) {
        g_owner = 0;
        logf_("🔓 输入持有者断开，已释放控制权");
    }
    if (clients[i].fd >= 0) close(clients[i].fd);
    clients[i] = clients[nclients - 1];
    nclients--;
    if (nclients > 0) memset(&clients[nclients], 0, sizeof clients[0]);
}

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>agent 沙箱 · 实时交互</title><style>\n"
    "html,body{margin:0;height:100%;background:#141414;overflow:hidden}\n"
    "body{display:flex;flex-direction:column;align-items:center;justify-content:center;"
    "font:12px system-ui;color:#888;user-select:none;-webkit-user-select:none}\n"
    "#wrap{position:relative;max-width:100%;max-height:calc(100% - 48px)}\n"
    "#v{display:block;max-width:100%;max-height:100%;box-shadow:0 4px 30px #000a;cursor:crosshair}\n"
    "#cur{position:absolute;width:16px;height:16px;margin:-8px 0 0 -8px;pointer-events:none;"
    "border:1px solid #fff;border-radius:50%;box-shadow:0 0 4px #000,inset 0 0 3px #000;display:none}\n"
    "#bar{margin:11px;letter-spacing:.06em;font-size:11px;text-transform:uppercase}\n"
    "#bar b{color:#4ade80;font-weight:600}\n"
    "#clipbar{margin:0 11px 8px;font-size:10px;color:#666;letter-spacing:.04em}\n"
    "#clipbar b{color:#93c5fd;font-weight:500}\n"
    "#k{position:fixed;left:0;bottom:0;width:1px;height:1px;opacity:0;"
    "border:0;padding:0;background:transparent;color:transparent;pointer-events:none}\n"
    "#hint{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;"
    "background:#0009;color:#fff;font-size:14px;letter-spacing:.1em;cursor:pointer;"
    "text-transform:uppercase;backdrop-filter:blur(2px)}\n"
    "</style></head><body>\n"
    "<div id=wrap><img id=v src=\"/stream.mjpg\"><div id=cur></div>"
    "<div id=hint>点击画面开始操作</div></div>\n"
    "<div id=bar>agent sandbox · <b id=st>连接中…</b> · 点画面接管（同一时刻只有一人能操作）· Ctrl+Esc 释放</div>\n"
    "<div id=clipbar>📋 <b id=st2>剪贴板未同步</b> · Ctrl+Shift+V 粘贴到我电脑</div>\n"
    "<input id=k autocomplete=off spellcheck=false>\n"
    "<script>\n"
    "const v=document.getElementById('v'),cur=document.getElementById('cur'),st=document.getElementById('st'),\n"
    "      k=document.getElementById('k'),hint=document.getElementById('hint');\n"
    "let ws=null,active=false;\n"
    "/* ---------- WebSocket ---------- */\n"
    "function conn(){\n"
    "  ws=new WebSocket((location.protocol==='https:'?'wss':'ws')+'://'+location.host+'/ws');\n"
    "  ws.binaryType='arraybuffer';\n"
    "  ws.onopen=()=>{st.textContent='画面 + 输入已连接';st.style.color='#4ade80';};\n"
    "  ws.onmessage=ev=>{\n"
    "    const b=new Uint8Array(ev.data);\n"
    "    if(b.length<8)return;\n"
    "    if(b[0]===0x81){            /* 0x81 = 下行剪贴板内容 */\n"
    "      const txt=new TextDecoder().decode(b.subarray(8));\n"
    "      sandboxClip=txt;\n"
    "      st2.textContent = txt ? ('沙箱剪贴板: '+txt.slice(0,24)+(txt.length>24?'…':'')) : '沙箱剪贴板为空';\n"
    "    }\n"
    "  };\n"
    "  ws.onclose=()=>{st.textContent='已断开，重连中…';st.style.color='#f87171';release();setTimeout(conn,1200);};\n"
    "  ws.onerror=()=>ws.close();\n"
    "}\n"
    "conn();\n"
    "/* 8 字节帧，布局必须与服务端 handle_msg 逐字节一致：\n"
    " *   [0]=type  [1]=保留  [2..3]=保留  [4..5]=A(int16 LE)  [6..7]=B(int16 LE)\n"
    " * ⚠️ 这里曾经写成 type|flag|rsv|A|B（type 后跟两个 1 字节字段），服务端却按\n"
    " * 4 字节头读 A，于是 A 读到的永远是 0；而且整个帧只有 5 字节，直接触发\n"
    " * handle_msg 里 `if (n < 8) return` 被静默丢弃 —— 表现为「点得动、打不了字」。\n"
    " * 改这里务必同步 admin/handler，8 字节一个都不能少。 */\n"
    "function send(type,a,b){\n"
    "  if(!ws||ws.readyState!==1)return;\n"
    "  const bf=new ArrayBuffer(8),dv=new DataView(bf);\n"
    "  dv.setUint8(0,type);dv.setUint8(1,0);dv.setUint16(2,0,true);\n"
    "  dv.setInt16(4,a|0,true);dv.setInt16(6,b|0,true);\n"
    "  ws.send(bf);\n"
    "}\n"
    "/* type 5：8 字节头 + UTF-8 负载。中文/emoji 等非 ASCII 走这条。\n"
    " * 服务端收到后换 keymap 注入，见 inject_text()。 */\n"
    "function sendText(t){\n"
    "  if(!ws||ws.readyState!==1||!t)return;\n"
    "  const enc=new TextEncoder().encode(t);\n"
    "  const bf=new Uint8Array(8+enc.length);\n"
    "  bf[0]=5;\n"
    "  bf.set(enc,8);\n"
    "  ws.send(bf);\n"
    "}\n"
    "/* ---------- 画面坐标 → 虚拟坐标 ---------- */\n"
    "function pos(e){\n"
    "  const r=v.getBoundingClientRect();\n"
    "  const x=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width));\n"
    "  const y=Math.max(0,Math.min(1,(e.clientY-r.top)/r.height));\n"
    "  return [Math.round(x*65535),Math.round(y*65535),e.clientX-r.left,e.clientY-r.top];\n"
    "}\n"
    "/* ---------- 鼠标 ---------- */\n"
    "v.addEventListener('mousemove',e=>{\n"
    "  if(!active)return;\n"
    "  const p=pos(e);cur.style.left=p[2]+'px';cur.style.top=p[3]+'px';\n"
    "  send(1,p[0],p[1]);\n"
    "});\n"
    "v.addEventListener('mousedown',e=>{if(!active)return;e.preventDefault();send(2,e.button+1,1);});\n"
    "window.addEventListener('mouseup',e=>{if(!active)return;send(2,e.button+1,0);});\n"
    "v.addEventListener('contextmenu',e=>e.preventDefault());\n"
    "v.addEventListener('wheel',e=>{if(!active)return;e.preventDefault();\n"
    "  send(3,(e.deltaY>0?-1:1)*3,0);},{passive:false});\n"
    "/* ---------- 键盘：DOM keyCode → evdev ---------- */\n"
    "const M={8:14,9:15,13:28,16:42,17:29,18:56,19:119,20:58,27:1,32:57,33:104,34:109,35:107,\n"
    "36:102,37:105,38:103,39:106,40:108,44:99,45:110,46:111,48:11,49:2,50:3,51:4,52:5,53:6,\n"
    "54:7,55:8,56:9,57:10,59:39,61:13,63:98,65:30,66:48,67:46,68:32,69:18,70:33,71:34,72:35,\n"
    "73:23,74:36,75:37,76:38,77:50,78:49,79:24,80:25,81:16,82:19,83:31,84:20,85:22,86:47,\n"
    "87:17,88:45,89:21,90:44,91:125,92:126,93:127,96:82,97:79,98:80,99:81,100:75,101:76,\n"
    "102:77,103:71,104:72,105:73,106:55,107:78,109:74,110:83,111:98,112:59,113:60,114:61,\n"
    "115:62,116:63,117:64,118:65,119:66,120:67,121:68,122:87,123:88,144:69,145:70,186:47,\n"
    "187:13,188:51,189:12,190:52,191:53,192:41,219:26,220:43,221:27,222:40,226:86};\n"
    "function evdev(e){\n"
    "  if(M[e.keyCode])return M[e.keyCode];\n"
    "  const c=(e.key||'').toUpperCase().charCodeAt(0);\n"
    "  return M[c]||0;\n"
    "}\n"
    "/* IME 组合中：此时按键是「拼音输入过程」，绝不能当普通键发出去，\n"
    " * 否则会把 pinyin 字母也打进沙箱。等 compositionend 拿到最终文本再发。 */\n"
    "let composing=false;\n"
    "k.addEventListener('compositionstart',()=>{composing=true;});\n"
    "k.addEventListener('compositionend',e=>{\n"
    "  composing=false;\n"
    "  if(!active)return;\n"
    "  if(e.data)sendText(e.data);   /* 中文/日文等由 IME 产出的最终文本 */\n"
    "  k.value='';\n"
    "});\n"
    "k.addEventListener('keydown',e=>{\n"
    "  if(!active)return;\n"
    "  if(e.key==='Escape'&&e.ctrlKey){e.preventDefault();release();return;}\n"
    "  if(composing)return;              /* 交给 IME，不要抢 */\n"
    "  /* 粘贴：Ctrl+V / Cmd+V 由 paste 事件处理，这里吞掉避免重复 */\n"
    "  if((e.ctrlKey||e.metaKey)&&(e.key==='v'||e.key==='V')){e.preventDefault();return;}\n"
    "  e.preventDefault();\n"
    "  const c=evdev(e);if(!c)return;\n"
    "  if(!e.repeat)send(4,c,1);\n"
    "});\n"
    "k.addEventListener('keyup',e=>{\n"
    "  if(!active||composing)return;e.preventDefault();\n"
    "  const c=evdev(e);if(c)send(4,c,0);\n"
    "});\n"
    "/* ---------- 剪贴板 ---------- */\n"
    "let sandboxClip='';\n"
    "/* 请求沙箱当前剪贴板（type 8），响应会走下行 0x81 */\n"
    "function clipPull(){ if(ws&&ws.readyState===1) send(8,0,0); }\n"
    "/* 把文本设成沙箱剪贴板（type 7）*/\n"
    "function clipPush(t){\n"
    "  if(!ws||ws.readyState!==1)return;\n"
    "  const enc=new TextEncoder().encode(t);\n"
    "  const bf=new Uint8Array(8+enc.length);\n"
    "  bf[0]=7; bf.set(enc,8); ws.send(bf);\n"
    "}\n"
    "setInterval(clipPull, 2000);   /* 定期同步沙箱剪贴板过来 */\n"
    "/* 粘贴。两条路：\n"
    " *   普通粘贴（Ctrl+V）：把文本**写进沙箱剪贴板**再发 Ctrl+V。\n"
    " *     为什么绕这一圈？因为逐键只能解决原生 Wayland 客户端；Xwayland\n"
    " *     有独立 keymap，打不了中文。走剪贴板粘贴则所有程序都能用。\n"
    " *   Ctrl+Shift+V：反过来，把**沙箱的**剪贴板内容贴到我自己的电脑上。\n"
    " */\n"
    "k.addEventListener('paste',e=>{\n"
    "  if(!active)return;\n"
    "  e.preventDefault();\n"
    "  const raw=(e.clipboardData||window.clipboardData).getData('text');\n"
    "  if(!raw)return;\n"
    "  const asciiOnly=/^[\\x00-\\x7F]*$/.test(raw);\n"
    "  if(asciiOnly){\n"
    "    sendText(raw);              /* 纯 ASCII 直接逐键打，最省事 */\n"
    "  }else{\n"
    "    /* 含非 ASCII：走剪贴板，保证 Xwayland 程序也能收到 */\n"
    "    clipPush(raw);\n"
    "    setTimeout(()=>{ send(4,29,1); send(4,47,1); send(4,47,0); send(4,29,0); }, 120);\n"
    "  }\n"
    "  k.value='';\n"
    "});\n"
    "/* Ctrl+Shift+V：把沙箱剪贴板内容复制到宿主机剪贴板（用浏览器 API） */\n"
    "k.addEventListener('keydown', e=>{\n"
    "  if(!active)return;\n"
    "  if((e.ctrlKey||e.metaKey)&&e.shiftKey&&(e.key==='V'||e.key==='v')){\n"
    "    e.preventDefault();\n"
    "    clipPull();\n"
    "    setTimeout(async()=>{\n"
    "      if(!sandboxClip)return;\n"
    "      try{ await navigator.clipboard.writeText(sandboxClip);\n"
    "           st2.textContent='已复制到你的剪贴板: '+sandboxClip.slice(0,20); }\n"
    "      catch(err){ st2.textContent='复制失败（浏览器限制）'; }\n"
    "    }, 200);\n"
    "  }\n"
    "}, true);\n"
    "/* 兜底：万一 input 事件拿到了非 ASCII（部分浏览器 IME 行为不一致），\n"
    " * 也送过去；纯 ASCII 忽略，避免和 keydown 重复。 */\n"
    "k.addEventListener('input',e=>{\n"
    "  if(!active||composing)return;\n"
    "  const t=e.target.value||'';\n"
    "  if(!t)return;\n"
    "  if(/[^\\x00-\\x7F]/.test(t))sendText(t);\n"
    "  k.value='';\n"
    "});\n"
    "/* ---------- 接管 / 释放 ---------- */\n"
    "function take(){active=true;hint.style.display='none';cur.style.display='block';\n"
    "  v.style.cursor='none';st.textContent='已接管 · 可操作沙箱';k.focus();\n"
    "  send(6,1,0);   /* type 6 = 输入权控制，action 1 = take */\n"
    "  const p=pos({clientX:v.getBoundingClientRect().left+50,clientY:v.getBoundingClientRect().top+50});\n"
    "  send(1,p[0],p[1]);}\n"
    "function release(){active=false;hint.style.display='flex';cur.style.display='none';\n"
    "  v.style.cursor='crosshair';st.textContent='已释放 · 但你仍可正常使用自己电脑';k.blur();\n"
    "  send(6,0,0);   /* action 0 = release */}\n"
    "hint.addEventListener('click',take);\n"
    "v.addEventListener('click',()=>{if(!active)take();});\n"
    "img_ready();\n"
    "function img_ready(){setTimeout(()=>{hint.style.display=active?'none':'flex';},300);}\n"
    "</script></body></html>";

static void serve_index(int fd) {
    char hdr[384];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        sizeof INDEX_HTML - 1);
    if (send_all(fd, hdr, (size_t)n) == 0)
        send_all(fd, INDEX_HTML, sizeof INDEX_HTML - 1);
    close(fd);
}

static void serve_forbidden(int fd) {
    static const char body[] = "403 Forbidden: untrusted Origin/Host\n";
    char hdr[192];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", sizeof body - 1);
    if (send_all(fd, hdr, (size_t)n) == 0) send_all(fd, body, sizeof body - 1);
    close(fd);
}

static void serve_stream(int fd) {
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\nConnection: close\r\n\r\n";
    if (send_all(fd, hdr, sizeof hdr - 1) < 0) close(fd);
}

static void push_frame(const uint8_t *data, size_t len) {
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);
    pthread_mutex_lock(&g_cl_mu);
    for (int i = nclients - 1; i >= 0; i--) {
        if (!clients[i].is_stream) continue;
        if (send_all(clients[i].fd, hdr, (size_t)hn) < 0 ||
            send_all(clients[i].fd, data, len) < 0 ||
            send_all(clients[i].fd, "\r\n", 2) < 0)
            drop_client(i);
    }
    pthread_mutex_unlock(&g_cl_mu);
}

/* 把一个 WS 帧推给**单个**客户端（按 id）。用于剪贴板响应。
 * **调用方必须持有 g_cl_mu。** */
static void ws_send_frame_locked(struct client *c, int opcode,
                                 const void *payload, size_t n) {
    if (!c || !c->is_ws || c->fd < 0) return;
    uint8_t hdr[10];
    size_t hn;
    if (n < 126) {
        hdr[0] = (uint8_t)(0x80 | opcode);
        hdr[1] = (uint8_t)n;
        hn = 2;
    } else if (n < 65536) {
        hdr[0] = (uint8_t)(0x80 | opcode);
        hdr[1] = 126;
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)(n & 0xFF);
        hn = 4;
    } else {
        hdr[0] = (uint8_t)(0x80 | opcode);
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(n >> (56 - i * 8));
        hn = 10;
    }
    /* 服务端发出的帧**不加掩码**（RFC 6455） */
    if (send_all(c->fd, hdr, hn) < 0 || send_all(c->fd, payload, n) < 0) {
        c->is_ws = 0;   /* 标记掉线，外层回收 */
    }
}

/* 把剪贴板内容发给请求者。
 * 协议（下行，opcode=2 二进制）：
 *   [0]=0x81  [4..5]=A=版本号  [8..]=UTF-8 文本
 * 复用 8 字节头，让前端解析逻辑和上行一致。 */
static void push_clipboard(unsigned long req_id) {
    char text[CLIP_MAX];
    int ver = 0;
    size_t n = clipboard_get(text, CLIP_MAX - 8, &ver);

    uint8_t *msg = malloc(8 + n);
    if (!msg) return;
    memset(msg, 0, 8);
    msg[0] = 0x81;                       /* 0x80 = 服务端→客户端，1 = 剪贴板 */
    msg[4] = (uint8_t)(ver & 0xFF);
    msg[5] = (uint8_t)((ver >> 8) & 0xFF);
    if (n) memcpy(msg + 8, text, n);

    pthread_mutex_lock(&g_cl_mu);
    int idx = client_by_id(req_id);
    if (idx >= 0) ws_send_frame_locked(&clients[idx], 2, msg, 8 + n);
    pthread_mutex_unlock(&g_cl_mu);
    free(msg);
}

/* 有没有人在看画面。主线程用它决定这一帧要不要编码 —— 必须走锁，
 * 否则读到的是网络线程正在改的 nclients（数据竞争，UB）。 */
/* ---------- 按需快照（GET /snapshot.jpg） ----------
 *
 * 场景：agent 想知道「现在屏幕长什么样」，只想拿一张图。
 * 之前只能 ① 用 `-s` 重启一个 wstream（会中断推流）② 装 grim。
 * 都不合适，加个 HTTP 端点。
 *
 * 线程问题：抓帧在主线程，HTTP 在网络线程。所以：
 *   网络线程：置 want 标志 → 条件变量等 ← 主线程编码完唤醒
 *   主线程：发现 want 就额外存一份 JPEG → 唤醒网络线程
 *
 * 编码只在「有人看流」或「有人要快照」时才做，闲时仍不耗 CPU。
 * 等待上限 3 秒：合成器卡住时不能让 HTTP 线程永久阻塞。 */
static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_snap_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_snap_want;      /* 网络线程置 1，表示有请求等着 */
static unsigned     g_snap_gen;       /* 每次 publish +1 */
static uint8_t     *g_snap_buf;
static size_t       g_snap_len;
static size_t       g_snap_cap;
/* 最近一次编码出来的 JPEG（不管有没有人在看）。用于让连续/并发快照请求
 * 直接命中缓存、不阻塞 accept 循环。
 *
 * 为什么需要：HTTP 的 accept 是**串行**的 —— 每个连接都要先 recv 请求、
 * 再处理。如果每个 snapshot 都阻塞等下一帧（~100ms @10fps），20 个并发请求
 * 就是 2 秒，listen backlog 直接溢出。有了新鲜缓存，第二个请求起就是零等待。 */
static uint8_t     *g_last_jpg;
static size_t       g_last_len;
static size_t       g_last_cap;
static uint32_t     g_last_ms;        /* 最近一次编码的时间戳 */

/* 主线程调用：如果有人在等快照，把这一帧的 JPEG 存下来。 */
static void snap_publish(const uint8_t *jpg, size_t len) {
    if (!g_snap_want) return;
    pthread_mutex_lock(&g_snap_mu);
    if (g_snap_want) {
        if (len > g_snap_cap) {
            uint8_t *nb = realloc(g_snap_buf, len);
            if (nb) { g_snap_buf = nb; g_snap_cap = len; }
        }
        if (len <= g_snap_cap) {
            memcpy(g_snap_buf, jpg, len);
            g_snap_len = len;
            g_snap_gen++;
            g_snap_want = 0;
            pthread_cond_broadcast(&g_snap_cv);
        }
    }
    pthread_mutex_unlock(&g_snap_mu);
}

/* 主线程在每次编码成功后调用：更新「最近一帧」缓存。 */
static void snap_cache_last(const uint8_t *jpg, size_t len) {
    pthread_mutex_lock(&g_snap_mu);
    if (len > g_last_cap) {
        uint8_t *nb = realloc(g_last_jpg, len);
        if (nb) { g_last_jpg = nb; g_last_cap = len; }
    }
    if (len <= g_last_cap) {
        memcpy(g_last_jpg, jpg, len);
        g_last_len = len;
        g_last_ms = now_ms();
    }
    pthread_mutex_unlock(&g_snap_mu);
}

/* 网络线程调用：请求一张快照并等它。
 * 成功返回 1，并把数据**拷贝**到调用方给的缓冲（*out 由调用方 free）。
 *
 * ⚠️ 必须拷贝，不能直接返回内部指针：多个 HTTP 线程可能同时等，
 * 而主线程下一次 publish 会覆盖同一块内存 —— 那就是「A 刚拿到指针、
 * B 触发新快照把它覆盖了」的数据竞争。踩过：并发取快照偶尔拿到半张图。 */
static int snap_wait(uint8_t **out, size_t *out_len) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 3;

    *out = NULL;
    *out_len = 0;

    pthread_mutex_lock(&g_snap_mu);
    /* 快速路径：最近 800ms 内编码过一帧就直接给它，完全不用等。
     * 800ms 的取值：@10fps 相当于最多 8 帧的容差，画面变化不会太旧；
     * 同时足够覆盖「agent 连续取几张图」的间隔。 */
    if (g_last_len > 0 && (uint32_t)(now_ms() - g_last_ms) < 800) {
        uint8_t *copy = malloc(g_last_len);
        if (copy) {
            memcpy(copy, g_last_jpg, g_last_len);
            *out = copy;
            *out_len = g_last_len;
            pthread_mutex_unlock(&g_snap_mu);
            return 1;
        }
    }
    if (!g_snap_buf) {
        g_snap_buf = malloc(512 * 1024);
        g_snap_cap = g_snap_buf ? 512 * 1024 : 0;
    }
    /* 用「版本号变了」判断，而不是布尔 ready。
     * 为什么：多个 HTTP 线程可能同时等，用布尔标志时后来者会把 ready 清零，
     * 先来的 waiter 就被漏掉、白等一轮。版本号只增不减，每个 waiter 记下
     * 自己等待的起点，谁都不会被别人的重置影响。 */
    unsigned start = g_snap_gen;
    g_snap_want = 1;
    while (g_snap_gen == start) {
        if (pthread_cond_timedwait(&g_snap_cv, &g_snap_mu, &dl) != 0) {
            g_snap_want = 0;
            break;
        }
    }
    int ok = 0;
    if (g_snap_gen != start && g_snap_len > 0) {
        uint8_t *copy = malloc(g_snap_len);
        if (copy) {
            memcpy(copy, g_snap_buf, g_snap_len);
            *out = copy;
            *out_len = g_snap_len;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_snap_mu);
    return ok;
}

static int has_viewers(void) {
    pthread_mutex_lock(&g_cl_mu);
    int n = nclients;
    pthread_mutex_unlock(&g_cl_mu);
    return n > 0;
}


/* ---------- 窗口列表（GET /windows） ----------
 *
 * 为什么 agent 需要这个：知道「现在有哪些窗口、各在屏幕的哪里」，
 * 才知道该往哪个坐标点击。只靠截图猜位置既慢又不可靠。
 *
 * 为什么不用 zwlr_foreign_toplevel_management：
 *   它只给 title / app_id，**不给坐标**（set_rectangle 是给任务栏的提示，
 *   不是真实位置）。而坐标恰恰是我们要的。
 *
 * 所以直接读 sway 的 IPC（沙箱里本来就跑着 sway）。
 * sway 的 i3-ipc 协议很简单：magic "i3-ipc" + len + type，GET_TREE=4。
 * 返回的 JSON 里每个容器带 rect 和 app_id/title。
 *
 * 这样做的前提：沙箱用的是 sway（start.sh 就是）。换成别的合成器时
 * 这个端点返回 501，不影响其它功能。 */
#define SWAY_IPC_GET_TREE 4

/* 在 dir 里找 sway-ipc.*.sock。找不到返回 0。 */
static int find_sway_sock(char *out, size_t cap, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (!strncmp(de->d_name, "sway-ipc.", 9)) {
            snprintf(out, cap, "%s/%s", dir, de->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 通过 sway IPC 拿窗口树 JSON。成功返回 malloc 的字符串（调用方 free），失败 NULL。 */
static char *sway_get_tree(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[512];
    if (!rt || !find_sway_sock(path, sizeof path, rt)) {
        /* 退化：试 wstream 的默认根目录 */
        if (!find_sway_sock(path, sizeof path, "/tmp/wstream/run")) return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    /* 显式长度检查：sun_path 只有 108 字节，超长会被 snprintf 静默截断，
     * 那样 connect 会连到错的路径。宁可直接失败。 */
    size_t plen_path = strlen(path);
    if (plen_path >= sizeof sa.sun_path) { close(fd); return NULL; }
    memcpy(sa.sun_path, path, plen_path + 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return NULL; }

    const char *cmd = "GET_TREE";
    uint32_t len = 7, type = SWAY_IPC_GET_TREE;
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    memcpy(hdr + 6, &len, 4);
    memcpy(hdr + 10, &type, 4);
    if (send_all(fd, hdr, 14) < 0 || send_all(fd, cmd, len) < 0) { close(fd); return NULL; }

    /* 响应：14 字节头 + payload */
    char rh[14];
    size_t got = 0;
    while (got < 14) {
        ssize_t n = recv(fd, rh + got, 14 - got, 0);
        if (n <= 0) { close(fd); return NULL; }
        got += (size_t)n;
    }
    if (memcmp(rh, "i3-ipc", 6) != 0) { close(fd); return NULL; }
    uint32_t plen;
    memcpy(&plen, rh + 6, 4);
    if (plen > 4 * 1024 * 1024) { close(fd); return NULL; }   /* 异常，别信 */
    char *buf = malloc(plen + 1);
    if (!buf) { close(fd); return NULL; }
    got = 0;
    while (got < plen) {
        ssize_t n = recv(fd, buf + got, plen - got, 0);
        if (n <= 0) { free(buf); close(fd); return NULL; }
        got += (size_t)n;
    }
    buf[plen] = 0;
    close(fd);
    return buf;
}

/* ---------- 极简 JSON 扫描器 ----------
 *
 * 项目刻意零依赖，所以不引 cJSON。但字符串 strstr 的做法不可行 ——
 * sway 的 JSON 里 rect 在 app_id **之前**，而且节点是嵌套的，
 * 按「app_id 之后找 rect」会错位（踩过：两个窗口只认出 1 个，坐标还串了）。
 *
 * 这里写一个最小但正确的递归下降扫描器：只需要能遍历对象/数组、
 * 读字符串和数字，找到「同时有 app_id 和 rect 的对象」。
 * 不构建完整 DOM，边扫边判断，省内存。 */

struct json_window {
    char app_id[128];
    char title[192];
    int  x, y, w, h;
    int  has_app, has_rect;
};

struct json_scan {
    const char *p;
    const char *end;
    struct json_window *out;
    int out_cap;
    int out_n;
    /* 当前对象收集到的字段 */
    struct json_window cur;
    int depth;
};

static void js_ws(struct json_scan *sc) {
    while (sc->p < sc->end &&
           (*sc->p == ' ' || *sc->p == '\t' || *sc->p == '\n' || *sc->p == '\r'))
        sc->p++;
}

/* 读一个 JSON 字符串到 buf，处理常见转义。返回 0 成功。 */
static int js_string(struct json_scan *sc, char *buf, size_t cap) {
    js_ws(sc);
    if (sc->p >= sc->end || *sc->p != '"') return -1;
    sc->p++;
    size_t n = 0;
    while (sc->p < sc->end && *sc->p != '"') {
        char c = *sc->p++;
        if (c == '\\' && sc->p < sc->end) {
            char e = *sc->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':   /* \uXXXX：这里不展开，简单跳过 4 位 */
                for (int i = 0; i < 4 && sc->p < sc->end; i++) sc->p++;
                c = '?';
                break;
            default: c = e; break;
            }
        }
        if (n + 1 < cap) buf[n++] = c;
    }
    if (sc->p < sc->end) sc->p++;   /* 跳过结尾引号 */
    buf[n] = 0;
    return 0;
}

/* 读一个数字（也接受 null / true / false，返回 0 并给默认值） */
static int js_number(struct json_scan *sc, int *out) {
    js_ws(sc);
    int neg = 0;
    if (sc->p < sc->end && *sc->p == '-') { neg = 1; sc->p++; }
    if (sc->p >= sc->end || *sc->p < '0' || *sc->p > '9') {
        /* 不是数字：跳过 token */
        while (sc->p < sc->end && *sc->p != ',' && *sc->p != '}' && *sc->p != ']')
            sc->p++;
        return -1;
    }
    long v = 0;
    while (sc->p < sc->end && *sc->p >= '0' && *sc->p <= '9') {
        v = v * 10 + (*sc->p - '0');
        if (v > 100000000) v = 100000000;   /* 防溢出 */
        sc->p++;
    }
    /* 跳过小数/指数部分 */
    while (sc->p < sc->end && ( (*sc->p >= '0' && *sc->p <= '9') ||
            *sc->p == '.' || *sc->p == 'e' || *sc->p == 'E' ||
            *sc->p == '+' || *sc->p == '-'))
        sc->p++;
    *out = (int)(neg ? -v : v);
    return 0;
}

static void js_skip_value(struct json_scan *sc);

/* 扫描一个对象。depth_limit 防止病态输入爆栈。 */
static void js_object(struct json_scan *sc, int depth) {
    if (depth > 64) return;
    js_ws(sc);
    if (sc->p >= sc->end || *sc->p != '{') return;
    sc->p++;

    struct json_window save = sc->cur;
    sc->cur = (struct json_window){0};

    for (;;) {
        js_ws(sc);
        if (sc->p >= sc->end) break;
        if (*sc->p == '}') { sc->p++; break; }
        if (*sc->p == ',') { sc->p++; continue; }

        char key[64];
        if (js_string(sc, key, sizeof key) != 0) { sc->p++; continue; }
        js_ws(sc);
        if (sc->p < sc->end && *sc->p == ':') sc->p++;
        js_ws(sc);

        if (!strcmp(key, "app_id")) {
            if (sc->p < sc->end && *sc->p == '"') {
                js_string(sc, sc->cur.app_id, sizeof sc->cur.app_id);
                sc->cur.has_app = 1;
            } else {
                js_skip_value(sc);
            }
        } else if (!strcmp(key, "name")) {
            if (sc->p < sc->end && *sc->p == '"')
                js_string(sc, sc->cur.title, sizeof sc->cur.title);
            else
                js_skip_value(sc);
        } else if (!strcmp(key, "rect")) {
            js_ws(sc);
            if (sc->p < sc->end && *sc->p == '{') {
                sc->p++;
                int x = 0, y = 0, w = 0, h = 0;
                for (;;) {
                    js_ws(sc);
                    if (sc->p >= sc->end) break;
                    if (*sc->p == '}') { sc->p++; break; }
                    if (*sc->p == ',') { sc->p++; continue; }
                    char rk[32];
                    if (js_string(sc, rk, sizeof rk) != 0) { sc->p++; continue; }
                    js_ws(sc);
                    if (sc->p < sc->end && *sc->p == ':') sc->p++;
                    int v = 0;
                    if (js_number(sc, &v) != 0) continue;
                    if (!strcmp(rk, "x")) x = v;
                    else if (!strcmp(rk, "y")) y = v;
                    else if (!strcmp(rk, "width")) w = v;
                    else if (!strcmp(rk, "height")) h = v;
                }
                sc->cur.x = x; sc->cur.y = y;
                sc->cur.w = w; sc->cur.h = h;
                sc->cur.has_rect = 1;
            } else {
                js_skip_value(sc);
            }
        } else if (!strcmp(key, "nodes") || !strcmp(key, "floating_nodes")) {
            /* 递归进子节点数组 */
            js_ws(sc);
            if (sc->p < sc->end && *sc->p == '[') {
                sc->p++;
                for (;;) {
                    js_ws(sc);
                    if (sc->p >= sc->end || *sc->p == ']') { if (sc->p < sc->end) sc->p++; break; }
                    if (*sc->p == ',') { sc->p++; continue; }
                    if (*sc->p == '{') js_object(sc, depth + 1);
                    else js_skip_value(sc);
                }
            }
        } else {
            js_skip_value(sc);
        }
    }

    /* 出对象：如果这个对象本身是个窗口，收下 */
    if (sc->cur.has_app && sc->cur.has_rect && sc->cur.w > 0 && sc->cur.h > 0 &&
        sc->out_n < sc->out_cap) {
        sc->out[sc->out_n++] = sc->cur;
    }
    sc->cur = save;
}

static void js_skip_value(struct json_scan *sc) {
    js_ws(sc);
    if (sc->p >= sc->end) return;
    char c = *sc->p;
    if (c == '"') { char t[8]; js_string(sc, t, sizeof t); return; }
    if (c == '{') { int d = 0; while (sc->p < sc->end) {
            if (*sc->p == '{') d++; else if (*sc->p == '}') { d--; if (!d) { sc->p++; return; } }
            sc->p++; } return; }
    if (c == '[') { int d = 0; while (sc->p < sc->end) {
            if (*sc->p == '[') d++; else if (*sc->p == ']') { d--; if (!d) { sc->p++; return; } }
            sc->p++; } return; }
    while (sc->p < sc->end && *sc->p != ',' && *sc->p != '}' && *sc->p != ']')
        sc->p++;
}

/* 把窗口列表转成 JSON 输出 */
static void sway_extract_windows(const char *json, char *out, size_t cap) {
    struct json_window wins[32];
    struct json_scan sc;
    memset(&sc, 0, sizeof sc);
    sc.p = json;
    sc.end = json + strlen(json);
    sc.out = wins;
    sc.out_cap = 32;

    js_ws(&sc);
    if (sc.p < sc.end && *sc.p == '{') {
        /* 顶层对象会递归进 nodes，最终把所有窗口收进 wins */
        js_object(&sc, 0);
    }

    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "{\"windows\":[");
    for (int i = 0; i < sc.out_n && o + 400 < cap; i++) {
        /* 转义 title */
        char esc[512];
        size_t eo = 0;
        for (const char *q = wins[i].title; *q && eo + 3 < sizeof esc; q++) {
            if (*q == '"' || *q == '\\') esc[eo++] = '\\';
            if ((unsigned char)*q >= 0x20) esc[eo++] = *q;
        }
        esc[eo] = 0;
        o += (size_t)snprintf(out + o, cap - o,
            "%s{\"app_id\":\"%s\",\"title\":\"%s\",\"x\":%d,\"y\":%d,"
            "\"width\":%d,\"height\":%d,\"cx\":%d,\"cy\":%d}",
            i ? "," : "", wins[i].app_id, esc,
            wins[i].x, wins[i].y, wins[i].w, wins[i].h,
            wins[i].x + wins[i].w / 2, wins[i].y + wins[i].h / 2);
    }
    snprintf(out + o, cap - o, "]}");
}

/* ---------- 请求头工具 ---------- */

/* 按行取头（大小写不敏感），value 去掉前导空格。找不到返回 -1，否则返回长度。
 * 逐行比对而不是在整个请求里 strstr：否则 `X-Origin:` 之类也会被误命中。 */
static int hdr_get(const char *req, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = req;
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        const char *line_end = e ? e : p + strlen(p);
        if ((size_t)(line_end - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;   /* 跳过冒号，否则值里会带上 ": " */
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            size_t n = 0;
            while (v + n < line_end && n + 1 < cap) { out[n] = v[n]; n++; }
            out[n] = 0;
            return (int)n;
        }
        if (!e) break;
        p = e + 2;
    }
    return -1;
}

/* Host 必须是回环地址。挡的是 DNS rebinding：攻击者把 evil.com 解析到 127.0.0.1，
 * 浏览器就会带着 `Host: evil.com` 打到我们这儿。只绑 127.0.0.1 并不能挡这一手，
 * 因为发起连接的正是用户自己的浏览器。 */
static int host_is_loopback(const char *host) {
    if (!host || !*host) return 0;
    if (!strncasecmp(host, "127.0.0.1", 9))   return host[9] == 0 || host[9] == ':';
    if (!strncasecmp(host, "localhost", 9))   return host[9] == 0 || host[9] == ':';
    if (!strncasecmp(host, "[::1]", 5))       return host[5] == 0 || host[5] == ':';
    return 0;
}

/* Origin 必须是本页自己。空 Origin = 非浏览器客户端（curl / skill 里的 Python），
 * 放行——它本来就是本机进程，能直连就已经有等价权限了。
 *
 * ⚠️ 不校验 Origin 的后果（实测过）：任意网页的 JS 都能连上 ws://127.0.0.1:8765/ws，
 * 往沙箱里点任意坐标、敲任意键 ⇒ 用户只是打开一个恶意页面，agent 沙箱就被接管了。
 * 「只监听 127.0.0.1」挡不住这个，因为握手请求是浏览器发的。
 * 浏览器对 WebSocket 一定带 Origin，所以这条校验对正常使用零影响。 */
static int origin_trusted(const char *origin, int port) {
    if (!origin || !*origin) return 1;
    char want[64];
    static const char *hosts[] = { "http://127.0.0.1", "http://localhost", "http://[::1]" };
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        snprintf(want, sizeof want, "%s:%d", hosts[i], port);
        if (!strcasecmp(origin, want)) return 1;
        if (!strcasecmp(origin, hosts[i])) return 1;   /* 省略默认端口的情况 */
    }
    return 0;
}

/* ---------- WebSocket 服务端 ---------- */

static int ws_wants(const char *req) {
    const char *u = strstr(req, "GET ");
    if (!u) return 0;
    const char *e = strstr(u, "\r\n");
    if (!e) return 0;
    /* 路径必须以 /ws 结尾（允许 ?query） */
    const char *p = u + 4;
    if (p + 3 > e) return 0;
    if (p[0] != '/' || p[1] != 'w' || p[2] != 's') return 0;
    return 1;
}

/* 从请求头里抠 Sec-WebSocket-Key（去掉前导空格） */
static int ws_get_key(const char *req, char *out, size_t cap) {
    return hdr_get(req, "Sec-WebSocket-Key", out, cap);
}

/* 校验来源。返回 0 = 允许；-1 = 缺 key；-2 = 来源不可信（回 403） */
static int ws_authorize(const char *req, int port) {
    /* 必须初始化：缺 Host 时 hdr_get 返回 -1，下面的日志要读它 */
    char host[128] = "", origin[256] = "";
    if (hdr_get(req, "Host", host, sizeof host) < 0 || !host_is_loopback(host)) {
        logf_("⛔ 拒绝 WebSocket：Host 不可信（%s）", host[0] ? host : "缺失");
        return -2;
    }
    if (hdr_get(req, "Origin", origin, sizeof origin) >= 0 && !origin_trusted(origin, port)) {
        logf_("⛔ 拒绝 WebSocket：Origin 不可信（%s）", origin);
        return -2;
    }
    char key[128];
    if (ws_get_key(req, key, sizeof key) <= 0) return -1;
    return 0;
}

static int ws_handshake(int fd, const char *req) {
    char key[128];
    if (ws_get_key(req, key, sizeof key) <= 0) return -1;
    char acc[64];
    ws_accept_key(key, acc);
    char resp[512];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (send_all(fd, resp, (size_t)n) < 0) return -1;
    return 0;
}

/* ---------- 文本注入队列 ----------
 *
 * type 5（文本）不能像鼠标/键盘那样在网络线程里直接注入：inject_text() 内部要
 * 换 keymap + roundtrip，而 Wayland 只允许主线程 dispatch。
 * 所以网络线程把文本塞进队列，主线程在抓帧循环里取出来执行。
 *
 * 用固定大小的环形缓冲 + 互斥锁。单条上限 4KB；满了就丢弃并记日志
 * （宁可丢，也不要让网络线程阻塞住 —— 那会把整个 HTTP 服务拖死）。 */
#define TEXT_Q_SLOTS 8
#define TEXT_Q_MAX   4096

static struct {
    char buf[TEXT_Q_SLOTS][TEXT_Q_MAX];
    size_t len[TEXT_Q_SLOTS];
    int head, tail, count;
} g_tq;
static pthread_mutex_t g_tq_mu = PTHREAD_MUTEX_INITIALIZER;

/* 网络线程调用。返回 0 = 入队成功，-1 = 队列满/超长 */
static int text_enqueue(const char *s, size_t n) {
    if (n == 0 || n >= TEXT_Q_MAX) return -1;
    int ok = -1;
    pthread_mutex_lock(&g_tq_mu);
    if (g_tq.count < TEXT_Q_SLOTS) {
        memcpy(g_tq.buf[g_tq.tail], s, n);
        g_tq.len[g_tq.tail] = n;
        g_tq.tail = (g_tq.tail + 1) % TEXT_Q_SLOTS;
        g_tq.count++;
        ok = 0;
    }
    pthread_mutex_unlock(&g_tq_mu);
    return ok;
}

/* 主线程调用：取一条出来（拷贝走），返回长度，0 = 队列空 */
static size_t text_dequeue(char *out, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&g_tq_mu);
    if (g_tq.count > 0) {
        n = g_tq.len[g_tq.head];
        if (n >= cap) n = cap - 1;
        memcpy(out, g_tq.buf[g_tq.head], n);
        out[n] = 0;
        g_tq.head = (g_tq.head + 1) % TEXT_Q_SLOTS;
        g_tq.count--;
    }
    pthread_mutex_unlock(&g_tq_mu);
    return n;
}

/* ---------- 剪贴板设置队列（浏览器 → 沙箱） ----------
 * 和文本队列同样的理由：clipboard_set() 要 roundtrip，只能主线程做。 */
static struct {
    char buf[TEXT_Q_SLOTS][CLIP_MAX];
    size_t len[TEXT_Q_SLOTS];
    int head, tail, count;
} g_cq;
static pthread_mutex_t g_cq_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_clip_dirty;    /* 主线程要处理剪贴板请求 */
static volatile int g_clip_req;      /* 哪个客户端要读剪贴板（0 = 无） */

static int clip_enqueue(const char *s, size_t n) {
    if (n >= CLIP_MAX) return -1;
    int ok = -1;
    pthread_mutex_lock(&g_cq_mu);
    if (g_cq.count < TEXT_Q_SLOTS) {
        /* 空内容也允许（= 清空剪贴板） */
        memcpy(g_cq.buf[g_cq.tail], s, n);
        g_cq.len[g_cq.tail] = n;
        g_cq.tail = (g_cq.tail + 1) % TEXT_Q_SLOTS;
        g_cq.count++;
        ok = 0;
    }
    pthread_mutex_unlock(&g_cq_mu);
    return ok;
}

/* 取一条。返回 0 = 队列空；否则把长度写进 *out_len（可以是 0，表示清空剪贴板）。
 * 用「返回 0/1 + 单独给长度」而不是「返回长度」：否则「队列空」和
 * 「要清空剪贴板（长度 0）」两种情况分不开。 */
static int clip_dequeue(char *out, size_t cap, size_t *out_len) {
    int got = 0;
    pthread_mutex_lock(&g_cq_mu);
    if (g_cq.count > 0) {
        size_t n = g_cq.len[g_cq.head];
        if (n >= cap) n = cap - 1;
        memcpy(out, g_cq.buf[g_cq.head], n);
        out[n] = 0;
        *out_len = n;
        g_cq.head = (g_cq.head + 1) % TEXT_Q_SLOTS;
        g_cq.count--;
        got = 1;
    }
    pthread_mutex_unlock(&g_cq_mu);
    return got;
}

/* 把 WS 帧负载交给注入器。
 * caller_id = 发送者 client id，**调用方必须持有 g_cl_mu**（要读 g_owner）。 */
static void handle_msg(unsigned long caller_id, const uint8_t *m, size_t n) {
    if (n < 8) return;
    uint8_t type = m[0];

    /* ---------- 控制帧（不受所有权限制） ---------- */
    if (type == 6) {
        /* action 取 A 字段（byte 4-5），与前端 send(6, action, 0) 对齐。
         * ⚠️ 不能读 m[1]：那是 flags 位，前端恒为 0，会把 take 当成 release。 */
        uint8_t action = m[4];   /* 0 = release, 1 = take */
        if (action == 1) {
            if (g_owner && g_owner != caller_id) {
                /* 抢占：把控制权转给新来的人。前排用户可能已经关页面了，
                 * 硬锁着不放会导致「谁都操作不了」。 */
                logf_("🔁 输入权转移：%lu → %lu", g_owner, caller_id);
            }
            g_owner = caller_id;
            logf_("🔒 客户端 %lu 取得输入权", caller_id);
        } else if (g_owner == caller_id) {
            g_owner = 0;
            logf_("🔓 客户端 %lu 释放输入权", caller_id);
        }
        return;
    }

    /* ---------- 剪贴板帧：不受输入权限制 ----------
     * 围观者也能复制沙箱里的文字、也能贴东西进来，这不应该要求接管。
     * ⚠️ 必须放在下面的 is_owner 检查**之前**，否则会被当成输入事件丢弃 ——
     * 踩过：页面读剪贴板一直超时，查了半天才发现是被所有权拦掉的。 */
    if (type == 7) {
        if (n > 8) {
            if (clip_enqueue((const char *)m + 8, n - 8) == 0) g_clip_dirty = 1;
            else logf_("⚠️ 剪贴板队列满或超长，丢弃 %zu 字节", n - 8);
        }
        return;
    }
    if (type == 8) {
        g_clip_req = (int)caller_id;
        g_clip_dirty = 1;
        return;
    }

    /* ---------- 输入帧：必须持有输入权 ---------- */
    /* ⚠️ 在前端隐藏按钮是不够的：任何脚本都能直接连 /ws 发帧。
     * 服务端强制「非持有者的输入一律丢弃」才是真的隔离。 */
    if (!is_owner(caller_id)) {
        static uint32_t last_warn;
        uint32_t t = now_ms();
        if (t - last_warn > 3000) {   /* 限流，避免刷屏 */
            last_warn = t;
            logf_("⛔ 忽略无输入权的客户端 %lu 的输入事件（当前持有者=%lu）",
                  caller_id, g_owner);
        }
        return;
    }

    /* 两种解释都要有，按 type 分开用 —— 这是之前「鼠标只有左半边能点」的根因：
     *   鼠标坐标 (type 1) 是 **无符号** 16 位，范围 0..65535；
     *   滚动步数 (type 3) 是 **有符号** 的（负数 = 向下滚）。
     * 早期统一按 int16_t 读，导致 x > 32767（屏幕右半）被解成负数，转成 uint32
     * 后变成天文数字，合成器直接忽略 → 右半边点不动。 */
    int ua = (int)((m[4]) | (m[5] << 8));            /* 0..65535 */
    int ub = (int)((m[6]) | (m[7] << 8));            /* 0..65535 */
    int sa = (int)(int16_t)((m[4]) | (m[5] << 8));   /* -32768..32767 */
    int sb = (int)(int16_t)((m[6]) | (m[7] << 8));
    switch (type) {
    case 1: inject_move(ua, ub, cap.fb_w, cap.fb_h); break;
    case 2: inject_button((uint32_t)ua, (uint32_t)(sb != 0)); break;
    case 3: inject_scroll(sa); break;
    case 4: inject_key((uint32_t)ua, (uint32_t)(sb != 0)); break;
    case 5:
        /* type 5 文本：8 字节头 + UTF-8 负载（byte 8 起）。
         * 交给主线程注入（见 text_enqueue 的说明）。 */
        if (n > 8) {
            if (text_enqueue((const char *)m + 8, n - 8) == 0) g_input_dirty = 1;
            else logf_("⚠️ 文本队列满或超长，丢弃 %zu 字节", n - 8);
        }
        break;
    default: break;
    }
}

/* 解析客户端缓冲里的 WS 帧。返回消耗的字节数；数据不全返回 0 */
static size_t ws_parse_one(struct client *cl) {
    uint8_t *p = cl->in;
    size_t avail = cl->in_len;
    if (avail < 2) return 0;

    int fin = p[0] & 0x80;
    int opcode = p[0] & 0x0F;
    int masked = p[1] & 0x80;
    uint64_t plen = p[1] & 0x7F;
    size_t off = 2;

    if (plen == 126) {
        if (avail < 4) return 0;
        plen = ((uint64_t)p[2] << 8) | p[3];
        off = 4;
    } else if (plen == 127) {
        if (avail < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
        off = 10;
    }
    if (plen > 1024 * 1024) return (size_t)-1;    /* 异常，断开 */
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (avail < off + 4) return 0;
        memcpy(mask, p + off, 4);
        off += 4;
    }
    if (avail < off + plen) return 0;

    uint8_t *payload = p + off;
    if (masked) for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];

    (void)fin;
    if (opcode == 2) {                                   /* 二进制：输入事件 */
        handle_msg(cl->id, payload, (size_t)plen);
    } else if (opcode == 8) {                            /* close */
        return (size_t)-2;
    }
    return off + (size_t)plen;
}

static void ws_read(struct client *cl) {
    uint8_t tmp[4096];
    ssize_t rn = recv(cl->fd, tmp, sizeof tmp, MSG_DONTWAIT);
    if (rn <= 0) {
        if (rn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        cl->is_ws = 0;   /* 标记掉线，外层回收 */
        return;
    }
    if (cl->in_len + (size_t)rn > sizeof cl->in) { cl->in_len = 0; return; }
    memcpy(cl->in + cl->in_len, tmp, (size_t)rn);
    cl->in_len += (size_t)rn;

    for (;;) {
        size_t used = ws_parse_one(cl);
        if (used == 0) break;
        if (used == (size_t)-2 || used == (size_t)-1) { cl->is_ws = 0; cl->in_len = 0; return; }
        memmove(cl->in, cl->in + used, cl->in_len - used);
        cl->in_len -= used;
    }
}


/* ---------- HTTP / WS 服务线程 ----------
 * 抓屏在主线程里跑（Wayland 事件必须单线程 dispatch），
 * 网络读写全部放到这个线程，两边靠 clients[] 和全局 srv 通信。
 * 之前把 accept 塞进抓帧等待循环里，新连接一进来主线程就卡死在 poll 上，
 * 一次都出不了帧 —— 那个 bug 就是用这个拆分修掉的。 */
static void *http_thread(void *arg) {
    int srv = (int)(intptr_t)arg;
    while (!g_net_stop) {
        struct pollfd pfd[1 + MAX_CLIENTS];
        unsigned long ws_id[1 + MAX_CLIENTS];   /* 与 pfd 一一对应，存 id 不存下标 */
        int nfd = 0;
        pfd[nfd].fd = srv; pfd[nfd].events = POLLIN; ws_id[nfd] = 0; nfd++;

        pthread_mutex_lock(&g_cl_mu);
        for (int i = 0; i < nclients; i++) {
            if (!clients[i].is_ws) continue;
            pfd[nfd].fd = clients[i].fd; pfd[nfd].events = POLLIN;
            ws_id[nfd] = clients[i].id;
            nfd++;
        }
        pthread_mutex_unlock(&g_cl_mu);

        int pr = poll(pfd, (nfds_t)nfd, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        /* 1) 新连接 */
        if (pfd[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept4(srv, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) break;

                struct timeval tv = {5, 0};
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                int fl = fcntl(cfd, F_GETFL);
                fcntl(cfd, F_SETFL, fl & ~O_NONBLOCK);

                char req[2048];
                ssize_t rn = recv(cfd, req, sizeof req - 1, 0);
                if (rn <= 0) { close(cfd); continue; }
                req[rn] = 0;

                fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

                int nd = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
                struct timeval stv = {2, 0};
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);

                /* 所有端点都先过来源校验。挡的是「用户随便开一个网页，
                 * 那个网页就能连 127.0.0.1 操作沙箱」（DNS rebinding 同理）。 */
                if (ws_wants(req)) {
                    int az = ws_authorize(req, g_port);
                    if (az != 0) { serve_forbidden(cfd); continue; }
                    if (ws_handshake(cfd, req) < 0) { close(cfd); continue; }
                    pthread_mutex_lock(&g_cl_mu);
                    unsigned long id = client_add(cfd, 0, 1);
                    pthread_mutex_unlock(&g_cl_mu);
                    if (!id) close(cfd);
                } else {
                    char host[128] = "", origin[256] = "";
                    if (hdr_get(req, "Host", host, sizeof host) < 0 || !host_is_loopback(host) ||
                        (hdr_get(req, "Origin", origin, sizeof origin) >= 0 &&
                         !origin_trusted(origin, g_port))) {
                        serve_forbidden(cfd);
                        continue;
                    }
                    if (strstr(req, "/windows")) {
                        /* 返回沙箱里的窗口列表 + 精确坐标，agent 据此决定点哪里 */
                        char *tree = sway_get_tree();
                        if (!tree) {
                            static const char b[] =
                                "{\"error\":\"无法读取窗口树（沙箱不是 sway？）\"}\n";
                            char h[192];
                            int hn = snprintf(h, sizeof h,
                                "HTTP/1.1 501 Not Implemented\r\n"
                                "Content-Type: application/json; charset=utf-8\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                sizeof b - 1);
                            if (send_all(cfd, h, (size_t)hn) == 0)
                                send_all(cfd, b, sizeof b - 1);
                        } else {
                            size_t jc = strlen(tree) + 4096;
                            char *js = malloc(jc);
                            if (js) {
                                sway_extract_windows(tree, js, jc);
                                char h[192];
                                int hn = snprintf(h, sizeof h,
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/json; charset=utf-8\r\n"
                                    "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                                    "Connection: close\r\n\r\n", strlen(js));
                                if (send_all(cfd, h, (size_t)hn) == 0)
                                    send_all(cfd, js, strlen(js));
                                free(js);
                            }
                            free(tree);
                        }
                        close(cfd);
                    } else if (strstr(req, "/snapshot.jpg")) {
                        /* 取一张实时快照，不需要订阅流、不需要重启进程 */
                        uint8_t *jpg = NULL;
                        size_t jl = 0;
                        if (snap_wait(&jpg, &jl)) {
                            char h[192];
                            int hn = snprintf(h, sizeof h,
                                "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                                "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                                "Connection: close\r\n\r\n", jl);
                            if (send_all(cfd, h, (size_t)hn) == 0)
                                send_all(cfd, jpg, jl);
                            free(jpg);
                        } else {
                            static const char b[] = "503 no frame yet\n";
                            char h[192];
                            int hn = snprintf(h, sizeof h,
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Type: text/plain\r\nContent-Length: %zu\r\n"
                                "Connection: close\r\n\r\n", sizeof b - 1);
                            if (send_all(cfd, h, (size_t)hn) == 0) send_all(cfd, b, sizeof b - 1);
                        }
                        close(cfd);
                    } else if (strstr(req, "/stream.mjpg")) {
                        serve_stream(cfd);
                        pthread_mutex_lock(&g_cl_mu);
                        unsigned long id = client_add(cfd, 1, 0);
                        pthread_mutex_unlock(&g_cl_mu);
                        if (!id) close(cfd);
                    } else {
                        serve_index(cfd);   /* 内部 close */
                    }
                }
            }
        }

        /* 2) WS 输入。按下标访问前先按 id 反查并确认还活着 —— 见 client_by_id 的说明。 */
        pthread_mutex_lock(&g_cl_mu);
        for (int i = 1; i < nfd; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int idx = client_by_id(ws_id[i]);
            if (idx < 0) continue;              /* 已经被回收了，跳过 */
            ws_read(&clients[idx]);
        }
        for (int i = nclients - 1; i >= 0; i--)
            if (!clients[i].is_stream && !clients[i].is_ws) drop_client(i);
        pthread_mutex_unlock(&g_cl_mu);
    }
    return NULL;
}

/* ============================================================ main */

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv) {
    int port = 8765, fps = 10, opt;
    const char *ppm_out = NULL;
    while ((opt = getopt(argc, argv, "p:f:q:vs:")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'f': fps = atoi(optarg); break;
        case 'q': g_quality = atoi(optarg); break;
        case 'v': g_verbose = 1; break;
        case 's': ppm_out = optarg; break;
        default:
            fprintf(stderr, "用法: %s [-p 端口(默认8765)] [-f 帧率(默认10)] [-q JPEG质量(默认70)]\n"
                        "          [-s 快照.ppm  (抓一帧存盘就退出，不需要浏览器)] [-v]\n", argv[0]);
            return 2;
        }
    }
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    g_fps = fps;
    g_port = port;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* ---------- 连接 Wayland ---------- */
    memset(&cap, 0, sizeof cap);
    cap.display = wl_display_connect(NULL);
    if (!cap.display) {
        const char *wl = getenv("WAYLAND_DISPLAY");
        const char *rt = getenv("XDG_RUNTIME_DIR");
        die("连不上 Wayland（WAYLAND_DISPLAY=%s, XDG_RUNTIME_DIR=%s）",
            wl ? wl : "(未设)", rt ? rt : "(未设)");
    }
    cap.registry = wl_display_get_registry(cap.display);
    wl_registry_add_listener(cap.registry, &registry_listener, &cap);
    wl_display_roundtrip(cap.display);
    wl_display_roundtrip(cap.display);

    if (!cap.mgr) die("合成器没提供 zwlr_screencopy_manager_v1"
                      "（Hyprland 不支持；请指向 labwc/sway 的沙箱 socket）");
    if (!cap.shm) die("合成器没提供 wl_shm");
    if (!cap.output) die("合成器没提供 wl_output");

    /* ---------- 拿输出尺寸 ---------- */
    int W = 0, H = 0;
    {
        struct zwlr_screencopy_frame_v1 *fr =
            zwlr_screencopy_manager_v1_capture_output(cap.mgr, 1, cap.output);
        zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, &cap);
        while (!cap.frame_ready && cap.fb_w <= 0) {
            if (wl_display_dispatch(cap.display) < 0) die("Wayland 断开");
        }
        zwlr_screencopy_frame_v1_destroy(fr);
        if (cap.fb_w <= 0 || cap.fb_h <= 0) die("拿不到输出尺寸");
        W = cap.fb_w; H = cap.fb_h;
        logf_("检测到输出: %dx%d (format 0x%x)", W, H, cap.fb_format);
    }

    struct shmbuf fb[2];
    if (create_shmbuf(&cap, &fb[0], W, H, cap.fb_format) < 0) die("创建共享内存失败");
    if (create_shmbuf(&cap, &fb[1], W, H, cap.fb_format) < 0) die("创建共享内存失败");

    /* ---------- 输入注入 ---------- */
    input_init(&cap);

    /* ---------- 剪贴板同步 ---------- */
    clipboard_init(&cap);

    /* ---------- HTTP 服务 ---------- */
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) die("socket: %s", strerror(errno));
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);      /* 只本机可访问 */
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("bind 127.0.0.1:%d 失败: %s", port, strerror(errno));
    if (listen(srv, 64) < 0) die("listen: %s", strerror(errno));
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);

    logf_("──────────────────────────────────────────────");
    logf_(" 实时画面 + 鼠标键盘 → http://127.0.0.1:%d", port);
    logf_(" 输出 %dx%d @ %d fps，MJPEG 下行 / WebSocket 上行", W, H, fps);
    logf_(" 不建窗口、不抢焦点。Ctrl-C 退出。");
    logf_("──────────────────────────────────────────────");

    pthread_t th;
    if (pthread_create(&th, NULL, http_thread, (void *)(intptr_t)srv) != 0)
        die("创建网络线程失败");

    struct jpeg_enc enc;
    if (jpeg_open(&enc) < 0) die("初始化 JPEG 编码器失败");

    const long period_ns = 1000000000L / fps;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    int cur = 0;
    long dropped = 0;
    (void)dropped;

    while (g_run) {
        /* 发起抓帧 */
        struct zwlr_screencopy_frame_v1 *fr =
            zwlr_screencopy_manager_v1_capture_output(cap.mgr, 1, cap.output);
        cap.frame_ready = 0;
        cap.frame_failed = 0;
        zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, &cap);
        zwlr_screencopy_frame_v1_copy_with_damage(fr, fb[cur].wl_buf);

        /* 只等这一帧，别的什么都不做（HTTP 在独立线程里）。
         *
         * 为什么是 roundtrip 而不是裸 poll：合成器在没有新输入、画面不变时
         * 可能长时间不给 Wayland socket 可读事件。裸 poll 会让抓帧永远等不到
         * 第二条 frame.ready（实测：只有第一帧）。roundtrip 会主动 flush 并等
         * 一次 sync 回应，天然保证请求被推出、事件被处理。 */
        wl_display_flush(cap.display);
        while (g_run && !cap.frame_ready) {
            struct pollfd p = { .fd = wl_display_get_fd(cap.display), .events = POLLIN };
            int pr = poll(&p, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) continue;
                g_run = 0; break;
            }
            /* 网络线程刚注入了输入：由主线程统一 roundtrip，
             * 既把事件推给合成器，也把随之而来的回调处理掉。
             *
             * 同时把排队的文本取出来注入 —— inject_text() 需要 roundtrip，
             * 只能在这里（主线程）做。一次最多处理一条，避免长时间占住循环。 */
            /* ⚠️ 这里必须同时看两个标志：g_input_dirty（键鼠/文本注入）和
             * g_clip_dirty（剪贴板设置/读取）。踩过的坑：剪贴板请求只置了
             * g_clip_dirty，而主循环只看 g_input_dirty，于是请求永远不被处理，
             * 页面那边表现为「读剪贴板一直超时」。 */
            if (g_input_dirty || g_clip_dirty) {
                int was_input = g_input_dirty;
                g_input_dirty = 0;
                g_clip_dirty = 0;
                if (was_input) wl_display_roundtrip(cap.display);
                char txt[TEXT_Q_MAX];
                size_t tn = text_dequeue(txt, sizeof txt);
                if (tn > 0) {
                    int got = inject_text(txt, tn);
                    if (g_verbose) logf_("⌨️ 文本注入 %d 字符 / %zu 字节", got, tn);
                }
                /* 剪贴板：设置 / 读取都必须在主线程（要 roundtrip）。
                 * ⚠️ 用 static 缓冲，不要每帧 malloc 256KB —— 那在 10fps 下
                 * 是无谓的分配压力，而且失败路径容易漏 free。 */
                static char cb[CLIP_MAX];
                size_t cbn = 0;
                if (clip_dequeue(cb, sizeof cb, &cbn)) {
                    clipboard_set(cb, cbn);
                    /* 设完等一次，保证合成器已采纳 */
                    wl_display_roundtrip(cap.display);
                }
                int req = g_clip_req;
                g_clip_req = 0;
                if (req) push_clipboard((unsigned long)req);
                continue;
            }
            /* 这一分支不能省：poll 超时也要 dispatch_pending，否则已到队列的事件
             * 永远不被处理，抓帧就停在第一帧（踩过）。 */
            if (p.revents & POLLIN) {
                if (wl_display_dispatch(cap.display) < 0) { g_run = 0; break; }
            } else {
                wl_display_dispatch_pending(cap.display);
                wl_display_flush(cap.display);
            }
        }
        zwlr_screencopy_frame_v1_destroy(fr);
        if (!g_run) break;

        if (cap.frame_failed) {
            dropped++;
        } else if (has_viewers() || g_snap_want) {
            /* 有人看流、或有人在等快照，都要编码 */
            int need_snap = g_snap_want;
            unsigned long jlen = jpeg_encode(&enc, fb[cur].addr, W, H);
            if (jlen > 0) {
                snap_cache_last(enc.buf, jlen);   /* 给快照端点兜底 */
                if (need_snap) snap_publish(enc.buf, jlen);
                if (has_viewers()) push_frame(enc.buf, jlen);
            } else if (need_snap) {
                /* 编码失败也要唤醒，否则 HTTP 线程白等 3 秒 */
                snap_publish(NULL, 0);
            }
        }
        /* -s：抓一帧存 PPM 就退出 */
        if (ppm_out && !cap.frame_failed) {
            if (save_ppm(fb[cur].addr, W, H, ppm_out) == 0)
                logf_("已存快照 %s (%dx%d)", ppm_out, W, H);
            else
                logf_("存快照失败: %s", ppm_out);
            g_run = 0;
        }
        cur ^= 1;

        /* 限速 */
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        struct timespec nowt;
        clock_gettime(CLOCK_MONOTONIC, &nowt);
        long diff = (next.tv_sec - nowt.tv_sec) * 1000000000L + (next.tv_nsec - nowt.tv_nsec);
        if (diff > 0) {
            struct timespec ts = {0, diff};
            nanosleep(&ts, NULL);
        } else {
            next = nowt;
        }
    }

    logf_("退出");
    /* 网络线程还卡在 poll 里，先把它叫停再拆 clients[]，否则会踩已释放的 fd。
     * 主线程已经不再碰 fd，所以这里持锁读完就够。 */
    g_net_stop = 1;
    pthread_mutex_lock(&g_cl_mu);
    for (int i = 0; i < nclients; i++)
        if (clients[i].fd >= 0) close(clients[i].fd);
    pthread_mutex_unlock(&g_cl_mu);
    close(srv);
    for (int i = 0; i < 2; i++) {
        wl_buffer_destroy(fb[i].wl_buf);
        munmap(fb[i].addr, fb[i].size);
        close(fb[i].fd);
    }
    if (cap.ptr) zwlr_virtual_pointer_v1_destroy(cap.ptr);
    if (cap.kbd) zwp_virtual_keyboard_v1_destroy(cap.kbd);
    zwlr_screencopy_manager_v1_destroy(cap.mgr);
    if (cap.output) wl_output_destroy(cap.output);
    wl_shm_destroy(cap.shm);
    wl_registry_destroy(cap.registry);
    wl_display_disconnect(cap.display);
    tjDestroy(enc.tj); free(enc.buf);
    return 0;
}
