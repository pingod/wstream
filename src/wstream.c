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

/* ============================================================ 基础工具 */

static int g_verbose;
static int g_quality = 70;
static int g_fps = 10;
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
static int g_cur_x, g_cur_y;      /* 我们以为光标在哪（像素） */

static void inject_move(int vx, int vy, int W, int H) {
    if (!cap.ptr || W <= 0 || H <= 0) return;
    int tx = (int)((long long)vx * W / 65535);
    int ty = (int)((long long)vy * H / 65535);
    if (tx >= W) tx = W - 1;
    if (ty >= H) ty = H - 1;
    int dx = tx - g_cur_x, dy = ty - g_cur_y;
    if (dx == 0 && dy == 0) return;
    zwlr_virtual_pointer_v1_motion(cap.ptr, now_ms(),
        wl_fixed_from_int(dx), wl_fixed_from_int(dy));
    zwlr_virtual_pointer_v1_frame(cap.ptr);
    wl_display_flush(cap.display);
    g_cur_x = tx; g_cur_y = ty;
}

static void inject_button(uint32_t btn, uint32_t down) {
    if (!cap.ptr) return;
    zwlr_virtual_pointer_v1_button(cap.ptr, now_ms(), btn, down ? 1 : 0);
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

    if (mods) {
        if (down) g_mods |= mods; else g_mods &= ~mods;
        zwp_virtual_keyboard_v1_modifiers(cap.kbd, g_mods, 0, 0, 0);
    } else if (g_mods) {
        zwp_virtual_keyboard_v1_modifiers(cap.kbd, g_mods, 0, 0, 0);
    }
    /* 两个必须一起做的动作，顺序也不能反：
     *   (1) 先推完整的 evdev 布局 keymap —— 多虚拟键盘并存时生效的是"最后推过来的
     *       那份"，别人（wtype/wshowkeys）留下的单键位表会让我们的键码全部错位；
     *   (2) 紧接着发 key。合成器一收到 keymap 就把它标成"已处理、可以按键"，
     *       随后才处理 key 事件，所以 keymap 必须在 key 之前。 */
    push_keymap_snapshot(&cap, code);
    /* 🔑 这里是整个键盘问题真正的根因（2026-10-23 实测坐实）：
     *   zvp_virtual_keyboard_v1.key(code) 里的 code 是 **evdev 扫描码**，
     *   wlroots 收到后自己 +8 得到 xkb keycode（evdev 30 = 'a' → xkb 38）。
     *   如果这里再 +8（历史上误以为协议要求和 wl_keyboard 一样要键码），
     *   合成器就会 +16：a(30)→46('l')、b(48)→56、c(46)→54。
     *   屏幕上的现象正好是 abc 打成 "l]c"，b 打成 "m"，与实测逐字节吻合。
     *   另一路证据：最小复现程序不 +8 时永远正确，一 +8 立刻错乱。
     *   所以：直接发 code，绝不加 8。 */
    zwp_virtual_keyboard_v1_key(cap.kbd, now_ms(), code, down ? 1 : 0);
    /* 注入发生在网络线程，不能在这里 roundtrip（主线程才 dispatch）。
     * 只 flush，主线程看到 g_input_dirty 会补一次 roundtrip。 */
    wl_display_flush(cap.display);
    g_input_dirty = 1;
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
    "#k{position:absolute;left:-9999px;opacity:0}\n"
    "#hint{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;"
    "background:#0009;color:#fff;font-size:14px;letter-spacing:.1em;cursor:pointer;"
    "text-transform:uppercase;backdrop-filter:blur(2px)}\n"
    "</style></head><body>\n"
    "<div id=wrap><img id=v src=\"/stream.mjpg\"><div id=cur></div>"
    "<div id=hint>点击画面开始操作</div></div>\n"
    "<div id=bar>agent sandbox · <b id=st>连接中…</b> · 点画面接管 · Ctrl+Esc 释放</div>\n"
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
    "k.addEventListener('keydown',e=>{\n"
    "  if(!active)return;\n"
    "  if(e.key==='Escape'&&e.ctrlKey){e.preventDefault();release();return;}\n"
    "  e.preventDefault();\n"
    "  const c=evdev(e);if(!c)return;\n"
    "  if(!e.repeat)send(4,c,1);\n"
    "});\n"
    "k.addEventListener('keyup',e=>{\n"
    "  if(!active)return;e.preventDefault();\n"
    "  const c=evdev(e);if(c)send(4,c,0);\n"
    "});\n"
    "/* ---------- 接管 / 释放 ---------- */\n"
    "function take(){active=true;hint.style.display='none';cur.style.display='block';\n"
    "  v.style.cursor='none';st.textContent='已接管 · 可操作沙箱';k.focus();\n"
    "  const p=pos({clientX:v.getBoundingClientRect().left+50,clientY:v.getBoundingClientRect().top+50});\n"
    "  send(1,p[0],p[1]);}\n"
    "function release(){active=false;hint.style.display='flex';cur.style.display='none';\n"
    "  v.style.cursor='crosshair';st.textContent='已释放 · 但你仍可正常使用自己电脑';k.blur();}\n"
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

/* 有没有人在看画面。主线程用它决定这一帧要不要编码 —— 必须走锁，
 * 否则读到的是网络线程正在改的 nclients（数据竞争，UB）。 */
static int has_viewers(void) {
    pthread_mutex_lock(&g_cl_mu);
    int n = nclients;
    pthread_mutex_unlock(&g_cl_mu);
    return n > 0;
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

/* 把 WS 帧负载交给注入器；返回 0 表示正常 */
static void handle_msg(const uint8_t *m, size_t n) {
    if (n < 8) return;
    uint8_t type = m[0];
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
        handle_msg(payload, (size_t)plen);
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
                    if (strstr(req, "/stream.mjpg")) {
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
    if (listen(srv, 8) < 0) die("listen: %s", strerror(errno));
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
             * 既把事件推给合成器，也把随之而来的回调处理掉。 */
            if (g_input_dirty) {
                g_input_dirty = 0;
                wl_display_roundtrip(cap.display);
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
        } else if (has_viewers()) {
            unsigned long jlen = jpeg_encode(&enc, fb[cur].addr, W, H);
            if (jlen > 0) push_frame(enc.buf, jlen);
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
