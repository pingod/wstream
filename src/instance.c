/*
 * instance.c — wstream 的实例编排子命令（start/stop/list/stop-all/dashboard）。
 *
 * 原来这些逻辑在 scripts 目录的 shell 脚本里，1.6.0 起搬进二进制，脚本废弃。
 * 移植时**逐条保留**了 shell 里踩坑换来的判据（见 docs/HANDOFF.md 坑 21~27、35）：
 *   - 进程归属用 XDG_RUNTIME_DIR == <root>/run，绝不按进程名裸杀（坑 21/23）
 *   - 「活着」= 进程在且非僵尸（容器里 PID 1 不 reap，坑 26 的同源教训）
 *   - 「沙箱可用」= socket 真能握手，不是文件存在（坑 26）
 *   - 进程清理按 XDG_RUNTIME_DIR 归属（不限名），--app 的任意程序也能清掉（坑 25 的推广）
 *   - 实例目录被删后仍能从 cmdline 认出孤儿（坑 27）
 *
 * 隔离红线：本模块只起/停 XDG_RUNTIME_DIR 指向 <root>/run 的无头沙箱，
 * 绝不触碰用户真实桌面（/run/user/<uid>）。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "instance.h"

#define DEFAULT_ROOT "/tmp/wstream"
#define DEFAULT_PORT 8765

static const char *STOPALL_APPS[] = {
    "wstream", "sway-nocap", "sway", "Xwayland", "alacritty", "desktop.sh",
    "sh", "sleep", NULL
};

/* ---------------------------------------------------------------- 日志 */

static void ilog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("\033[1;36m[wstream]\033[0m ", stderr);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void iwarn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("\033[1;33m[wstream]\033[0m ", stderr);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void idie(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("\033[1;31m[wstream]\033[0m ", stderr);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint32_t ims(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------------------------------------------------------------- 文件 */

/* 读整个文件到 buf，NUL 结尾。返回长度，失败 -1。 */
static ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (ssize_t)n;
}

static int write_file(const char *path, const char *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static int write_pid(const char *path, pid_t pid) {
    char b[32]; int n = snprintf(b, sizeof b, "%d\n", (int)pid);
    return write_file(path, b, (size_t)n, 0644);
}

static pid_t read_pid(const char *path) {
    char b[32];
    if (read_file(path, b, sizeof b) < 0) return -1;
    return (pid_t)atoi(b);
}

static int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

static int rm_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    return remove(path);
}
static void remove_tree(const char *path) {
    nftw(path, rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

/* ---------------------------------------------------------------- 进程 */

/* /proc/<pid>/stat 第 3 字段是状态。Z = 僵尸。 */
static char pid_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, buf, sizeof buf) < 0) return 0;
    /* comm 可能含空格/括号，从最后一个 ')' 之后取状态字段 */
    char *p = strrchr(buf, ')');
    if (!p || !p[1]) return 0;
    p += 2;   /* 跳过 ") " */
    return *p;
}
static int is_zombie(pid_t pid) { return pid_state(pid) == 'Z'; }

static int pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) < 0) return 0;
    return !is_zombie(pid);
}

/* 取 /proc/<pid>/environ 里某个 KEY 的值。找到返回 0。 */
static int pid_env(pid_t pid, const char *key, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/environ", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) return -1;
    size_t kl = strlen(key);
    for (ssize_t i = 0; i < n; ) {
        char *e = memchr(buf + i, 0, (size_t)(n - i));
        size_t len = e ? (size_t)(e - (buf + i)) : (size_t)(n - i);
        if (len > kl && !strncmp(buf + i, key, kl) && buf[i + kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, buf + i + kl + 1, vl);
            out[vl] = 0;
            return 0;
        }
        i += (ssize_t)len + 1;
    }
    return -1;
}

static int pid_runtime_dir(pid_t pid, char *out, size_t cap) {
    return pid_env(pid, "XDG_RUNTIME_DIR", out, cap);
}

/* /proc/<pid>/cmdline 用空格拼起来。 */
static int pid_cmdline(pid_t pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) { out[0] = 0; return -1; }
    size_t o = 0;
    for (ssize_t i = 0; i < n && o < cap - 1; i++)
        out[o++] = buf[i] ? buf[i] : ' ';
    out[o] = 0;
    return 0;
}

/* /proc/<pid>/comm（进程名，≤15 字符）等于 name？ */
static int comm_is(pid_t pid, const char *name) {
    char path[64], buf[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    if (read_file(path, buf, sizeof buf) < 0) return 0;
    buf[strcspn(buf, "\n")] = 0;
    return !strcmp(buf, name);
}

/* 遍历 /proc，收集 comm == name 的 pid。返回个数。 */
static int scan_comm(const char *name, pid_t *pids, int max) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    int n = 0;
    while (n < max && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0) continue;
        if (comm_is(pid, name)) pids[n++] = pid;
    }
    closedir(d);
    return n;
}

/* 归属判据：XDG_RUNTIME_DIR == <root>/run（坑 21）。 */
static int belongs_to_root(pid_t pid, const char *root) {
    char rt[512], want[512];
    if (pid_runtime_dir(pid, rt, sizeof rt) < 0) return 0;
    snprintf(want, sizeof want, "%s/run", root);
    return !strcmp(rt, want);
}

/* 收集归属本实例（root）的所有非僵尸进程（XDG_RUNTIME_DIR == root/run，不限进程名）。
 * 归属判据保证不误伤用户桌面（其 XDG_RUNTIME_DIR 是 /run/user/<uid>）；
 * 也能清掉 --app 启动的任意程序及其子进程（进程名不在任何白名单里）。 */
static int own_pids(const char *root, pid_t *pids, int max) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    int n = 0;
    while (n < max && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0 || pid == getpid()) continue;
        if (is_zombie(pid)) continue;
        if (belongs_to_root(pid, root)) pids[n++] = pid;
    }
    closedir(d);
    return n;
}

/* ---------------------------------------------------------------- 网络 */

/* 端口上有没有人在服务（真发一个 GET / 看 200，比「进程在」可靠）。 */
static int port_serving(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return 0; }
    const char *req = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (send(fd, req, strlen(req), MSG_NOSIGNAL) < 0) { close(fd); return 0; }
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "200") != NULL;
}

/* ---------------------------------------------------------------- 沙箱探测 */

/* 找 <root>/run/wayland-* 的**真 socket**。找到返回 0 并写 out。
 * ⚠️ 必须跳过 wayland-N.lock（sway 的锁文件）：readdir 无序，可能先读到 .lock，
 * 拿它当 socket 会「连不上 Wayland」。原 start.sh 用排序的 ls wayland-* | head -1
 * 恰好总排在 .lock 前面才避开；C 里必须显式排除（CI 裸容器实测踩到）。 */
static int find_wl_socket(const char *root, char *out, size_t cap) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/run", root);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "wayland-", 8)) continue;
        size_t l = strlen(de->d_name);
        if (l >= 5 && !strcmp(de->d_name + l - 5, ".lock")) continue;  /* 跳过锁文件 */
        snprintf(out, cap, "%s", de->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

/* 删掉 <root>/run 下所有 wayland-*（socket 与 .lock）。渲染器回退重试前必须清：
 * 上一次失败的 sway 可能已经建了 socket 文件，find_wl_socket 会命中它，
 * 于是「重试成功」是假象 —— 实际连的是个没人监听的死文件。 */
static void unlink_sockets(const char *root) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/run", root);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "wayland-", 8)) continue;
        char fp[1024];
        snprintf(fp, sizeof fp, "%s/%s", dir, de->d_name);
        unlink(fp);
    }
    closedir(d);
}

static int have_prog(const char *name) {
    char *path = getenv("PATH");
    if (!path) return 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", tok, name);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

static int find_in_path(const char *name, char *out, size_t cap) {
    char *path = getenv("PATH");
    if (!path) return -1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", tok, name);
        if (access(full, X_OK) == 0) { snprintf(out, cap, "%s", full); return 0; }
    }
    return -1;
}

/* 带沙箱环境跑 wlr-randr，捕获 stdout（out 非空时），返回退出码。
 * extra_arg 非空时执行 --output <extra_arg> --custom-mode <size>。
 * 父进程侧 3 秒超时（对应 shell 的 timeout 3）。 */
static int run_wlr_randr(const char *root, const char *wl,
                         const char *extra_arg, const char *size,
                         char *out, size_t outcap) {
    int pipefd[2] = {-1, -1};
    if (out && pipe(pipefd) < 0) return -1;
    char rundir[512];
    snprintf(rundir, sizeof rundir, "%s/run", root);
    pid_t pid = fork();
    if (pid < 0) { if (out) { close(pipefd[0]); close(pipefd[1]); } return -1; }
    if (pid == 0) {
        if (out) { close(pipefd[0]); dup2(pipefd[1], 1); close(pipefd[1]); }
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { if (!out) dup2(dn, 1); dup2(dn, 2); close(dn); }
        setenv("XDG_RUNTIME_DIR", rundir, 1);
        setenv("WAYLAND_DISPLAY", wl, 1);
        if (extra_arg)
            execlp("wlr-randr", "wlr-randr", "--output", extra_arg,
                   "--custom-mode", size, (char *)NULL);
        else
            execlp("wlr-randr", "wlr-randr", (char *)NULL);
        _exit(127);
    }
    if (out) {
        close(pipefd[1]);
        size_t n = 0;
        while (n < outcap - 1) {
            ssize_t r = read(pipefd[0], out + n, outcap - 1 - n);
            if (r <= 0) break;
            n += (size_t)r;
        }
        out[n] = 0;
        close(pipefd[0]);
    }
    /* 3 秒超时 */
    int status = -1;
    uint32_t dl = ims() + 3000;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (ims() > dl) { kill(pid, SIGKILL); waitpid(pid, &status, 0); status = -1; break; }
        msleep(20);
    }
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* socket 真能握手才算活（坑 26）。没有 wlr-randr 时退化看 sway.pid。 */
static int sandbox_alive(const char *root, const char *wl) {
    if (!wl || !*wl) return 0;
    if (have_prog("wlr-randr"))
        return run_wlr_randr(root, wl, NULL, NULL, NULL, 0) == 0;
    char path[512];
    snprintf(path, sizeof path, "%s/sway.pid", root);
    return pid_alive(read_pid(path));
}

/* ---------------------------------------------------------------- 起进程 */

static pid_t spawn_sway(const char *root, const char *sway_bin,
                        const char *renderer, const char *logname) {
    char logpath[512], rundir[512], cfgdir[512];
    snprintf(logpath, sizeof logpath, "%s/%s", root, logname);
    snprintf(rundir, sizeof rundir, "%s/run", root);
    snprintf(cfgdir, sizeof cfgdir, "%s/cfg", root);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, 0); close(dn); }
        unsetenv("DISPLAY");
        /* ⚠️ 必须清掉继承来的 SWAYSOCK / I3SOCK：sway 会用它们绑定自己的 IPC
         * socket，而调用方（用户桌面跑 sway/i3，或另一个 wstream 实例的 shell）
         * 环境里通常就带着一个指向**别处**的路径 —— 于是新 sway 报
         * "Unable to bind IPC socket" 直接退出，`wstream start` 报
         * 「连不上 Wayland」。这个坑在「多 agent 各起一个实例」时几乎必现，
         * 因为前一个实例的 shell 里就 export 过 SWAYSOCK。 */
        unsetenv("SWAYSOCK");
        unsetenv("I3SOCK");
        setenv("XDG_RUNTIME_DIR", rundir, 1);
        setenv("XDG_CONFIG_HOME", cfgdir, 1);
        setenv("WLR_BACKENDS", "headless", 1);
        /* 渲染器由调用方决定（见 start_instance 的回退逻辑）：
         *   gles2  —— 走 EGL，会导出 zwp_linux_dmabuf_v1，Vulkan/EGL 客户端（gpui、
         *             游戏、视频播放器）才能创建 surface。需要 /dev/dri 或可用的
         *             EGL 设备；裸容器（CI）没有，会报 "no DRM FD available"。
         *   pixman —— 纯软件，任何环境都能起，但**不导出** dmabuf。 */
        setenv("WLR_RENDERER", renderer, 1);
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
        setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
        /* ⚠️ 必须把配置路径放进 cmdline：instance_root_of() 判定「这个进程属于哪个
         * 实例」靠两条证据 —— root 下的 .pid 文件，或 cmdline 里出现 "<root>/"。
         * 默认实例不再写 desktop.sh（窗口改由浏览器按钮按需拉起）之后，sway 的
         * cmdline 里就一个 root 路径都没有了；目录一旦被删（测试会这么干），
         * stop-all 就再也认不出它，留下孤儿 sway-nocap。显式 -c 补上这条证据。 */
        char cfgpath[1024];
        int cn = snprintf(cfgpath, sizeof cfgpath, "%s/sway/config", cfgdir);
        if (cn < 0 || (size_t)cn >= sizeof cfgpath) _exit(127);
        execlp(sway_bin, "sway", "--unsupported-gpu", "-c", cfgpath, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_wstream(const char *root, int port, const char *wl) {
    char logpath[512], rundir[512], portstr[16];
    snprintf(logpath, sizeof logpath, "%s/wstream.log", root);
    snprintf(rundir, sizeof rundir, "%s/run", root);
    snprintf(portstr, sizeof portstr, "%d", port);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, 0); close(dn); }
        unsetenv("DISPLAY");
        setenv("XDG_RUNTIME_DIR", rundir, 1);
        setenv("WAYLAND_DISPLAY", wl, 1);
        /* ⚠️ 必须 exec 真实路径，不能 exec "/proc/self/exe"：Linux 把 comm 设成
         * 路径末段，exec /proc/self/exe 会让 comm 变成 "exe"，list/stop-all
         * 按名字就找不到它了。readlink 解析出真实路径，comm 才是 "wstream"。 */
        char self[512];
        ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
        if (sl < 0) _exit(127);
        self[sl] = 0;
        execl(self, "wstream", "stream", "-p", portstr,
              "-f", "10", "--verbose", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* 文件能力（cap_sys_nice）检测：有 security.capability xattr 即有。
 * 容器里 root 没有 CAP_SYS_NICE 时 execve 带文件能力的二进制会 EPERM，
 * 复制一份丢掉 xattr 再启动（沿用原 start.sh 的 sway-nocap 绕过）。 */
static int has_file_caps(const char *path) {
    return getxattr(path, "security.capability", NULL, 0) >= 0;
}
static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in); close(out);
    chmod(dst, 0755);
    return 0;
}

/* ---------------------------------------------------------------- 沙箱内容 */

/* 生成 desktop.sh：**只在 --app 时写**。
 *
 * 默认沙箱启动后是**空的** —— 交互命令行和毫秒时钟不再自动拉起，改由浏览器里
 * 的两个按钮（WS type 12）按需打开。理由：自动开窗口的沙箱没法「先看看干净
 * 桌面」，而关掉它们只能停实例重建。--app 是用户显式要求，仍然自动启动。
 *
 * stop 按 XDG_RUNTIME_DIR 归属清理，故任意程序名都能收掉。 */
static int write_desktop_sh(const char *root, const char *app) {
    char path[512];
    snprintf(path, sizeof path, "%s/desktop.sh", root);
    char buf[4096];
    int n = snprintf(buf, sizeof buf,
        "#!/bin/sh\n"
        "%s &\n"
        "sleep 36000\n", app);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return write_file(path, buf, (size_t)n, 0755);
}

static void dump_log_tail(const char *path) {
    char buf[8192];
    ssize_t n = read_file(path, buf, sizeof buf);
    fprintf(stderr, "──── %s ────\n", path);
    if (n > 0) {
        /* 只打最后 ~2000 字节 */
        const char *p = n > 2000 ? buf + n - 2000 : buf;
        fputs(p, stderr);
    }
    fprintf(stderr, "\n──────────────────\n");
}

/* 从 wstream 进程 cmdline 里读 -p 端口。读不到返回 0。 */
static int port_of_pid(pid_t pid) {
    char cmd[4096];
    if (pid_cmdline(pid, cmd, sizeof cmd) < 0) return 0;
    char *p = strstr(cmd, "-p ");
    if (!p) return 0;
    return atoi(p + 3);
}

/* ---------------------------------------------------------------- start */

static int start_instance(const char *root, int port, const char *size, const char *app) {
    /* ⚠️ 第一件事：确认这个端口不是**别的实例**在服务。
     *
     * 原逻辑只等 port_serving(port) 变真就判定成功。于是当端口被别人的实例占着时：
     * 我们自己的 wstream 因 EADDRINUSE 立刻退出，而别人的实例照常应答 ——
     * start 报告「浏览器打开 http://127.0.0.1:8765」，但那其实是别人的沙箱。
     * 两个 agent 于是共用同一个实例、互相抢输入权（真实踩到）。
     *
     * 必须在**起沙箱之前**检查：放在后面的话失败时已经留下一个 sway 进程。 */
    {
        pid_t wp0[64];
        int wn0 = scan_comm("wstream", wp0, 64);
        /* 注意判据是「本实例的 wstream **且**端口是 port」，不能只看「本实例有
         * wstream 在跑」：那样本实例跑在 8766、而 8765 被别人占着时，会被误判成
         * 「是我自己的」而放行。 */
        int mine_serving = 0;
        for (int i = 0; i < wn0; i++)
            if (belongs_to_root(wp0[i], root) && pid_alive(wp0[i]) &&
                port_of_pid(wp0[i]) == port)
                mine_serving = 1;
        if (!mine_serving && port_serving(port))
            idie("端口 %d 已被另一个 wstream 实例占用（不是 %s 这个实例）。\n"
                 "  两边会连到同一个沙箱、互相抢输入权，所以直接失败而不是假装成功。\n"
                 "  换个端口：WSTREAM_ROOT=%s WSTREAM_PORT=<空闲端口> ./wstream start\n"
                 "  看谁占着：./wstream list", port, root, root);
    }

    char wl[256] = "";
    find_wl_socket(root, wl, sizeof wl);

    if (wl[0] && sandbox_alive(root, wl)) {
        ilog("沙箱已在运行（%s），复用", wl);
    } else {
        if (wl[0]) ilog("发现残留的 %s（进程已死），重建沙箱", wl);
        remove_tree(root);
        char dir[512];
        snprintf(dir, sizeof dir, "%s/run", root); mkdir_p(dir);
        snprintf(dir, sizeof dir, "%s/cfg/sway", root); mkdir_p(dir);
        snprintf(dir, sizeof dir, "%s/out", root); mkdir_p(dir);
        snprintf(dir, sizeof dir, "%s/run", root); chmod(dir, 0700);

        char path[512];
        /* 默认沙箱不写 desktop.sh：起完就是空桌面，窗口由浏览器按钮按需拉起。
         * 只有 --app 才写（用户显式要求开机就启动那个程序）。 */
        if (app && *app) {
            if (write_desktop_sh(root, app) < 0) idie("写入 desktop.sh 失败");
        }

        char cfg[1024];
        int cn;
        if (app && *app)
            cn = snprintf(cfg, sizeof cfg,
                "set $mod Mod4\ndefault_border pixel 2\ndefault_floating_border pixel 2\n"
                "exec %s/desktop.sh\n", root);
        else
            cn = snprintf(cfg, sizeof cfg,
                "set $mod Mod4\ndefault_border pixel 2\ndefault_floating_border pixel 2\n");
        snprintf(path, sizeof path, "%s/cfg/sway/config", root);
        write_file(path, cfg, (size_t)cn, 0644);

        char sway_bin[512];
        if (find_in_path("sway", sway_bin, sizeof sway_bin) < 0)
            idie("找不到 sway，先装：sudo pacman -S --needed sway");
        if (has_file_caps(sway_bin)) {
            char nocap[512];
            snprintf(nocap, sizeof nocap, "%s/sway-nocap", root);
            if (copy_file(sway_bin, nocap) == 0)
                snprintf(sway_bin, sizeof sway_bin, "%s", nocap);
        }

        ilog("启动无头沙箱 %s …", size);
        /* 渲染器选择：默认 gles2（导出 dmabuf，Vulkan/EGL 客户端才能起），
         * 起不来就回退 pixman（纯软件，任何环境都能跑）。裸容器（CI）没有
         * /dev/dri，gles2 会报 "no DRM FD available" 直接退出 —— 以前这种环境
         * 只有 pixman 能过，所以回退是必需的，不是可选项。
         * WSTREAM_RENDERER 可强制指定，此时不回退（用户明确要求）。 */
        const char *forced = getenv("WSTREAM_RENDERER");
        const char *cands[2];
        int ncand = 0;
        if (forced && *forced) {
            cands[ncand++] = forced;
        } else {
            cands[ncand++] = "gles2";
            cands[ncand++] = "pixman";
        }
        pid_t spid = -1;
        int chosen = -1;
        for (int ci = 0; ci < ncand; ci++) {
            /* 每次尝试都要清掉上次的残留 socket，否则 find_wl_socket 会命中
             * 上一次（已死）留下的文件，误判成「起来了」。 */
            unlink_sockets(root);
            spid = spawn_sway(root, sway_bin, cands[ci],
                              ci == 0 ? "sway.log" : "sway-fallback.log");
            if (spid < 0) idie("fork sway 失败: %s", strerror(errno));
            snprintf(path, sizeof path, "%s/sway.pid", root);
            write_pid(path, spid);

            /* 等 socket（最多 60 秒；容器首次 fontconfig 建缓存可能很慢） */
            uint32_t dl = ims() + 60000;
            while (ims() < dl) {
                if (find_wl_socket(root, wl, sizeof wl) == 0) break;
                if (!pid_alive(spid)) break;
                msleep(250);
            }
            if (find_wl_socket(root, wl, sizeof wl) == 0) { chosen = ci; break; }
            if (ci + 1 < ncand) {
                iwarn("渲染器 %s 起不来，回退 %s 重试", cands[ci], cands[ci + 1]);
                kill(spid, SIGKILL);
                msleep(300);
                unlink_sockets(root);
            }
        }
        if (find_wl_socket(root, wl, sizeof wl) < 0) {
            snprintf(path, sizeof path, "%s/sway.log", root);
            dump_log_tail(path);
            snprintf(path, sizeof path, "%s/sway-fallback.log", root);
            dump_log_tail(path);
            idie("sway 没起来（日志见上）");
        }
        ilog("沙箱渲染器：%s", cands[chosen < 0 ? 0 : chosen]);
        snprintf(path, sizeof path, "%s/wl_name", root);
        write_file(path, wl, strlen(wl), 0644);

        /* 改分辨率 */
        if (!have_prog("wlr-randr")) {
            iwarn("没装 wlr-randr，分辨率保持合成器默认（请求的 %s 未生效）", size);
        } else {
            char out[1024];
            if (run_wlr_randr(root, wl, NULL, NULL, out, sizeof out) == 0) {
                char outname[128] = "";
                sscanf(out, "%127s", outname);
                if (outname[0]) {
                    if (run_wlr_randr(root, wl, outname, size, NULL, 0) != 0)
                        iwarn("wlr-randr 设置 %s 失败，分辨率可能未生效", size);
                }
            }
        }
        msleep(3000);
    }

    /* ---------- wstream ---------- */
    pid_t wp[64];
    int wn = scan_comm("wstream", wp, 64);
    int running = 0;
    for (int i = 0; i < wn; i++)
        if (belongs_to_root(wp[i], root) && pid_alive(wp[i])) running = 1;

    if (running && port_serving(port)) {
        ilog("wstream 已在运行（端口 %d 可服务）", port);
    } else {
        for (int i = 0; i < wn; i++)
            if (belongs_to_root(wp[i], root)) kill(wp[i], SIGKILL);
        ilog("启动 wstream（端口 %d）…", port);
        pid_t wpid = spawn_wstream(root, port, wl);
        if (wpid < 0) idie("fork wstream 失败: %s", strerror(errno));
        char path[512];
        snprintf(path, sizeof path, "%s/wstream.pid", root);
        write_pid(path, wpid);

        /* ⚠️ 不能只看 port_serving()：那个端口可能是别人在应答（见上）。
         * 必须同时要求「我们 fork 的这个进程还活着」，它死了就是 bind 失败。 */
        uint32_t dl = ims() + 20000;
        while (ims() < dl) {
            if (!pid_alive(wpid)) {
                snprintf(path, sizeof path, "%s/wstream.log", root);
                dump_log_tail(path);
                idie("wstream 启动后立即退出（日志见上；端口被占用是最常见原因）");
            }
            if (port_serving(port)) break;
            msleep(200);
        }
        if (!port_serving(port)) {
            snprintf(path, sizeof path, "%s/wstream.log", root);
            dump_log_tail(path);
            idie("wstream 已启动但端口 %d 一直不可服务", port);
        }
    }

    ilog("──────────────────────────────────────────");
    ilog(" 浏览器打开： http://127.0.0.1:%d", port);
    ilog(" 停止：       wstream stop");
    ilog("──────────────────────────────────────────");
    return 0;
}

/* ---------------------------------------------------------------- stop */

static int stop_instance(const char *root) {
    /* 先杀 pid 文件里的 */
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s", root);
    DIR *d = opendir(root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 4 || strcmp(de->d_name + l - 4, ".pid")) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", root, de->d_name);
            pid_t p = read_pid(path);
            if (p > 0) kill(p, SIGTERM);
            unlink(path);
        }
        closedir(d);
    }
    /* 兜底：按归属杀掉本沙箱全部进程（不限名，含 --app 及其子进程），绝不误伤用户桌面 */
    pid_t pids[512];
    int n = own_pids(root, pids, 512);
    for (int i = 0; i < n; i++) kill(pids[i], SIGTERM);
    /* 等真退出（最多 5 秒） */
    for (int it = 0; it < 25; it++) {
        n = own_pids(root, pids, 512);
        if (n == 0) break;
        msleep(200);
    }
    /* 仍不退出的强杀 */
    n = own_pids(root, pids, 512);
    for (int i = 0; i < n; i++) kill(pids[i], SIGKILL);
    ilog("已停止");
    return 0;
}

/* ---------------------------------------------------------------- list */

struct inst {
    char root[512];
    pid_t pids[256];
    int npids;
};

/* 某个实例（root）里 wstream 正在用的端口；没有返回 0。
 * 用于「只给了 WSTREAM_ROOT、没给端口」时复用自己已占的端口，
 * 而不是傻乎乎再抢一个。 */
static int port_of_root(const char *root) {
    pid_t pids[64];
    int n = scan_comm("wstream", pids, 64);
    for (int i = 0; i < n; i++) {
        if (is_zombie(pids[i]) || !pid_alive(pids[i])) continue;
        if (!belongs_to_root(pids[i], root)) continue;
        int port = port_of_pid(pids[i]);
        if (port > 0) return port;
    }
    return 0;
}

static int cmd_list(int argc, char **argv) {
    int url_only = 0, port_arg = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--url")) url_only = 1;
        else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) && i + 1 < argc)
            port_arg = atoi(argv[++i]);
    }
    pid_t pids[256];
    int n = scan_comm("wstream", pids, 256);
    int found = 0;
    /* 收集 (port, root, pid, wl, online) */
    for (int i = 0; i < n; i++) {
        if (is_zombie(pids[i])) continue;
        if (pids[i] == getpid()) continue;   /* 别把 list 命令自己算进去 */
        char rt[512];
        if (pid_runtime_dir(pids[i], rt, sizeof rt) < 0) continue;
        size_t rtl = strlen(rt);
        if (rtl < 4 || strcmp(rt + rtl - 4, "/run")) continue;  /* 只认 <root>/run 实例结构 */
        char root[512];
        snprintf(root, sizeof root, "%s", rt);
        char *slash = strrchr(root, '/');
        if (slash) *slash = 0;   /* dirname：去掉 /run */
        int port = port_of_pid(pids[i]);
        if (!port) port = DEFAULT_PORT;
        if (port_arg && port != port_arg) continue;
        int online = port_serving(port);
        char wlpath[600], wl[256] = "?";
        snprintf(wlpath, sizeof wlpath, "%s/wl_name", root);
        char wlb[256];
        if (read_file(wlpath, wlb, sizeof wlb) > 0) {
            wlb[strcspn(wlb, "\n")] = 0;
            snprintf(wl, sizeof wl, "%s", wlb);
        }
        if (url_only) {
            printf("http://127.0.0.1:%d\n", port);
        } else {
            if (!found) {
                ilog("共运行中的实例：");
                printf("  PORT    状态    运行时目录               URL\n");
                printf("  ----    -----   ----------------------   ---\n");
            }
            printf("  %-7d %s  %-22s  http://127.0.0.1:%d\n",
                   port, online ? "在线  " : "离线  ", root, port);
        }
        found++;
    }
    if (!found && !url_only) {
        ilog("没有正在运行的实例");
        ilog("  启动一个：  wstream start");
        ilog("  多实例：    重复 wstream start 自动新建；或 WSTREAM_ROOT=/tmp/ws-a WSTREAM_PORT=8801 wstream start");
    }
    return 0;
}

/* ---------------------------------------------------------------- stop-all */

/* 实例归属（坑 27）：XDG_RUNTIME_DIR=<root>/run，且 root 下有 pid 文件
 * 或 cmdline 含 <root>/ 路径（目录删了也认）。 */
static int instance_root_of(pid_t pid, char *root, size_t cap) {
    char rt[512];
    if (pid_runtime_dir(pid, rt, sizeof rt) < 0) return -1;
    size_t l = strlen(rt);
    if (l < 4 || strcmp(rt + l - 4, "/run")) return -1;
    char r[512];
    snprintf(r, sizeof r, "%s", rt);
    r[l - 4] = 0;   /* 去掉 /run */
    /* 证据 1：root 下有 pid 文件 */
    int has = 0;
    DIR *d = opendir(r);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t dl = strlen(de->d_name);
            if (dl >= 4 && !strcmp(de->d_name + dl - 4, ".pid")) { has = 1; break; }
        }
        closedir(d);
    }
    /* 证据 2：cmdline 含 <root>/ */
    if (!has) {
        char cmd[4096], needle[600];
        snprintf(needle, sizeof needle, "%s/", r);
        if (pid_cmdline(pid, cmd, sizeof cmd) == 0 && strstr(cmd, needle)) has = 1;
    }
    if (!has) return -1;
    snprintf(root, cap, "%s", r);
    return 0;
}

static int cmd_stop_all(int argc, char **argv) {
    int assume_yes = 0, dry_run = 0, all_insts = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) assume_yes = 1;
        else if (!strcmp(argv[i], "--dry-run")) dry_run = 1;
        else if (!strcmp(argv[i], "--all")) all_insts = 1;
    }
    /* ⚠️ 设了 WSTREAM_ROOT 就只停这个实例：测试脚本用 stop-all 清理自己时，
     * 绝不能顺手停掉用户正在看的另一个实例（踩过：跑 lifecycle 把 8765 停了）。 */
    const char *only_root = getenv("WSTREAM_ROOT");
    if (only_root && !*only_root) only_root = NULL;
    static struct inst insts[64];
    int ninst = 0;
    for (int a = 0; STOPALL_APPS[a]; a++) {
        pid_t pids[256];
        int n = scan_comm(STOPALL_APPS[a], pids, 256);
        for (int i = 0; i < n; i++) {
            if (is_zombie(pids[i])) continue;
            char root[512];
            if (instance_root_of(pids[i], root, sizeof root) < 0) continue;
            if (only_root && !all_insts && strcmp(root, only_root)) continue;
            int idx = -1;
            for (int j = 0; j < ninst; j++)
                if (!strcmp(insts[j].root, root)) { idx = j; break; }
            if (idx < 0) {
                if (ninst >= 64) continue;
                idx = ninst++;
                snprintf(insts[idx].root, sizeof insts[idx].root, "%s", root);
                insts[idx].npids = 0;
            }
            if (insts[idx].npids < 256)
                insts[idx].pids[insts[idx].npids++] = pids[i];
        }
    }
    if (ninst == 0) { ilog("没有正在运行的 wstream 实例"); return 0; }

    ilog("发现 %d 个 wstream 实例：", ninst);
    printf("  %-24s %s\n", "运行时目录", "进程数");
    for (int j = 0; j < ninst; j++) {
        int port = 0;
        for (int k = 0; k < insts[j].npids; k++) {
            if (comm_is(insts[j].pids[k], "wstream")) {
                port = port_of_pid(insts[j].pids[k]);
                if (port) break;
            }
        }
        printf("  %-24s %d 个  端口 %d\n", insts[j].root, insts[j].npids, port);
    }
    if (dry_run) { ilog("--dry-run，不做任何操作"); return 0; }
    if (!assume_yes) {
        if (!isatty(0)) {
            iwarn("非交互环境，未带 --yes 就不动手。确认要停请加 --yes");
            return 1;
        }
        printf("确认停止上面这些实例？[y/N] ");
        fflush(stdout);
        char ans[16] = "";
        if (!fgets(ans, sizeof ans, stdin)) ans[0] = 0;
        if (ans[0] != 'y' && ans[0] != 'Y') { ilog("已取消"); return 0; }
    }
    int fail = 0;
    for (int j = 0; j < ninst; j++) {
        ilog("停止 %s …", insts[j].root);
        stop_instance(insts[j].root);
        /* 兜底：stop_instance 没清掉的，按归属杀 */
        for (int k = 0; k < insts[j].npids; k++) {
            pid_t p = insts[j].pids[k];
            if (pid_alive(p)) {
                kill(p, SIGTERM);
                msleep(1000);
                if (pid_alive(p)) kill(p, SIGKILL);
            }
        }
    }
    /* 等真退出 */
    int left = 0;
    for (int it = 0; it < 25; it++) {
        left = 0;
        for (int j = 0; j < ninst; j++)
            for (int k = 0; k < insts[j].npids; k++)
                if (pid_alive(insts[j].pids[k])) left++;
        if (left == 0) break;
        msleep(200);
    }
    if (left == 0) {
        ilog("全部已停止（%d 个实例）", ninst);
        for (int j = 0; j < ninst; j++) {
            char pat[600];
            snprintf(pat, sizeof pat, "%.511s/run", insts[j].root);
            DIR *d = opendir(pat);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (!strncmp(de->d_name, "wayland-", 8)) {
                        char fp[1024];
                        snprintf(fp, sizeof fp, "%s/%s", pat, de->d_name);
                        unlink(fp);
                    }
                }
                closedir(d);
            }
        }
    } else {
        iwarn("仍有 %d 个进程没退出，可能卡在 D 状态（IO 等待）", left);
        iwarn("可以用 wstream list 复查，或 kill -9 手动处理");
        fail = 1;
    }
    return fail;
}

/* ---------------------------------------------------------------- dashboard */

static int cmd_dashboard(int argc, char **argv) {
    const char *out = "/tmp/wstream-dashboard.html";
    for (int i = 1; i < argc; i++)
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--out")) && i + 1 < argc)
            out = argv[++i];

    /* 收集在线实例 URL */
    pid_t pids[256];
    int n = scan_comm("wstream", pids, 256);
    char urls[64][64];
    int nurl = 0;
    for (int i = 0; i < n && nurl < 64; i++) {
        if (is_zombie(pids[i])) continue;
        if (pids[i] == getpid()) continue;
        int port = port_of_pid(pids[i]);
        if (!port) port = DEFAULT_PORT;
        if (port_serving(port))
            snprintf(urls[nurl++], sizeof urls[0], "http://127.0.0.1:%d", port);
    }

    FILE *f = fopen(out, "w");
    if (!f) idie("无法写 %s: %s", out, strerror(errno));
    fputs(
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n"
        "<title>wstream 实例总览</title>\n"
        "<style>\n"
        "  :root{color-scheme:dark}\n"
        "  body{margin:0;background:#0b0f14;color:#cbd5e1;\n"
        "       font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}\n"
        "  header{padding:14px 18px;border-bottom:1px solid #1e293b;display:flex;\n"
        "       align-items:baseline;gap:12px;flex-wrap:wrap}\n"
        "  h1{font-size:14px;margin:0;font-weight:600;letter-spacing:.06em;color:#f8fafc}\n"
        "  .meta{color:#64748b;font-size:11px}\n"
        "  #grid{display:grid;gap:14px;padding:16px;\n"
        "        grid-template-columns:repeat(auto-fill,minmax(420px,1fr))}\n"
        "  .card{background:#111827;border:1px solid #1e293b;border-radius:8px;overflow:hidden}\n"
        "  .bar{display:flex;align-items:center;justify-content:space-between;\n"
        "       padding:8px 11px;border-bottom:1px solid #1e293b;font-size:12px}\n"
        "  .port{color:#93c5fd;font-weight:600}\n"
        "  a{color:#93c5fd;text-decoration:none;font-size:11px}\n"
        "  a:hover{text-decoration:underline}\n"
        "  .shot{display:block;width:100%;background:#020617;min-height:200px}\n"
        "  .empty{padding:40px;text-align:center;color:#64748b}\n"
        "</style></head><body>\n"
        "<header>\n"
        "  <h1>wstream 实例总览</h1>\n"
        "  <span class=\"meta\">每个实例都是完全独立的虚拟桌面 · 画面为实时 MJPEG · 点「打开」可交互</span>\n"
        "</header>\n"
        "<div id=\"grid\">\n", f);
    if (nurl == 0) {
        fputs("  <div class=\"empty\">没有正在运行的实例。启动：<code>wstream start</code></div>\n", f);
    } else {
        for (int i = 0; i < nurl; i++) {
            const char *port = strrchr(urls[i], ':') + 1;
            fprintf(f,
                "  <div class=\"card\">\n"
                "    <div class=\"bar\">\n"
                "      <span class=\"port\">:%s</span>\n"
                "      <a href=\"%s\" target=\"_blank\">打开 ↗</a>\n"
                "    </div>\n"
                "    <img class=\"shot\" src=\"%s/stream.mjpg\" alt=\"instance %s\"\n"
                "         onerror=\"this.style.display='none';this.nextElementSibling.style.display='block'\">\n"
                "    <div class=\"empty\" style=\"display:none\">画面加载失败（实例可能已停止）</div>\n"
                "  </div>\n", port, urls[i], urls[i], port);
        }
    }
    fputs("</div></body></html>\n", f);
    fclose(f);
    printf("%s\n", out);
    return 0;
}

/* ---------------------------------------------------------------- 校验 / 分发 */

static int valid_size(const char *s) {
    const char *x = strchr(s, 'x');
    if (!x || x == s || !x[1]) return 0;
    for (const char *p = s; p < x; p++) if (!isdigit((unsigned char)*p)) return 0;
    for (const char *p = x + 1; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static void usage(void) {
    fprintf(stderr,
        "wstream %s — 隔离的无头 Wayland 虚拟桌面 + 实时画面/输入\n\n"
        "用法：\n"
        "  wstream start [WxH] [--app/-A 程序]  起隔离沙箱并推流（默认 1600x1000，端口 8765）\n"
        "                             --app/-A：启动即打开该程序；默认沙箱是空桌面，\n"
        "                             交互命令行/毫秒时钟由页面上的两个按钮按需打开\n"
        "                             多实例：重复 start 自动新建；显式 WSTREAM_ROOT/WSTREAM_PORT 复用指定实例\n"
        "  wstream stop               停止本实例（WSTREAM_ROOT 指定，默认 /tmp/wstream）\n"
        "  wstream list [--url] [-p PORT]   列出运行中的实例\n"
        "  wstream stop-all [--yes] [--dry-run] [--all]\n"
        "                             停止实例：设了 WSTREAM_ROOT 只停该实例；--all 停全部\n"
        "  wstream dashboard [-o OUT]  生成所有实例的总览页面\n"
        "  wstream stream [-p PORT] [-f FPS] [-q QUALITY] [-s FILE.ppm] [--verbose]\n"
        "                             纯推流客户端（连接 WAYLAND_DISPLAY 指向的合成器）\n"
        "  wstream version | help     版本与用法（-v / --version 同义）\n\n"
        "环境变量：WSTREAM_ROOT（沙箱根目录）、WSTREAM_PORT（端口）\n"
        "⚠️ 别裸跑 stream 连真实桌面：默认拒绝 /run/user/<uid>，用 start 起沙箱。\n",
        WSTREAM_VERSION);
}

int instance_main(int argc, char **argv) {
    const char *cmd = argv[0];
    const char *root = getenv("WSTREAM_ROOT");
    if (!root || !*root) root = DEFAULT_ROOT;

    if (!strcmp(cmd, "start")) {
        const char *ports = getenv("WSTREAM_PORT");
        int port = ports && *ports ? atoi(ports) : DEFAULT_PORT;
        const char *size = "1600x1000";
        const char *app = NULL;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--app") || !strcmp(argv[i], "-A")) {
                if (i + 1 >= argc) idie("--app/-A 需要一个程序名");
                app = argv[++i];
            } else if (argv[i][0] == '-') {
                idie("未知选项：%s（用法：wstream start [WxH] [--app/-A 程序]）", argv[i]);
            } else {
                size = argv[i];
            }
        }
        if (!valid_size(size)) idie("分辨率格式应为 WxH（如 1600x1000），收到：%s", size);
        if (port < 1 || port > 65535) idie("端口越界（1..65535）：%d", port);
        /* 多实例：**既没设 WSTREAM_ROOT 也没设 WSTREAM_PORT** 时，若目标实例已占用，
         * 自动新建实例（<base>-2、-3… + 顺延端口）。显式指定任一者时保持复用语义。
         * ⚠️ 基名仍取 DEFAULT_ROOT —— 显式 root 表示「就要这个实例」。 */
        char auto_root[128];
        int port_explicit = (ports && *ports);
        int root_explicit = (getenv("WSTREAM_ROOT") && *getenv("WSTREAM_ROOT"));

        /* 只给了 root、没给端口：先用自己已占的端口（重复 start = 复用），
         * 否则从默认端口起找一个空闲的。
         *
         * ⚠️ 为什么不能直接用 8765：两个 agent 各设一个 WSTREAM_ROOT、都不设端口时，
         * 第二个实例会绑到已被占用的 8765 —— 它的 wstream 因 EADDRINUSE 退出，
         * 而 8765 上别人的实例照常应答，于是两边**共用同一个沙箱**、互相抢输入权。
         * 这是真实踩到的「多个 agent 用同一个实例」。 */
        if (root_explicit && !port_explicit) {
            int mine = port_of_root(root);
            if (mine > 0) {
                port = mine;
            } else {
                int i;
                for (i = 0; i < 100; i++)
                    if (!port_serving(DEFAULT_PORT + i)) break;
                if (i >= 100)
                    idie("找不到空闲端口（%d..%d 全被占用）",
                         DEFAULT_PORT, DEFAULT_PORT + 99);
                port = DEFAULT_PORT + i;
                if (i > 0)
                    ilog("端口 %d 已被其它实例占用，本实例改用 %d", DEFAULT_PORT, port);
            }
        }
        if (!root_explicit && !port_explicit) {
            int i;
            for (i = 0; i < 100; i++) {
                char cand[128], wl[256];
                if (i == 0) snprintf(cand, sizeof cand, "%s", DEFAULT_ROOT);
                else snprintf(cand, sizeof cand, "%s-%d", DEFAULT_ROOT, i + 1);
                int root_busy = (find_wl_socket(cand, wl, sizeof wl) == 0 &&
                                 sandbox_alive(cand, wl));
                if (!root_busy && !port_serving(DEFAULT_PORT + i)) break;
            }
            if (i >= 100) idie("实例数过多，无法自动分配；请用 WSTREAM_ROOT/WSTREAM_PORT 显式指定");
            if (i > 0) {
                snprintf(auto_root, sizeof auto_root, "%s-%d", DEFAULT_ROOT, i + 1);
                root = auto_root;
                port = DEFAULT_PORT + i;
                ilog("默认实例已占用，自动新建实例 %s（端口 %d）", root, port);
            }
        }
        return start_instance(root, port, size, app);
    }
    if (!strcmp(cmd, "stop")) return stop_instance(root);
    if (!strcmp(cmd, "list")) return cmd_list(argc, argv);
    if (!strcmp(cmd, "stop-all")) return cmd_stop_all(argc, argv);
    if (!strcmp(cmd, "dashboard")) return cmd_dashboard(argc, argv);
    if (!strcmp(cmd, "version")) { printf("wstream %s\n", WSTREAM_VERSION); return 0; }
    if (!strcmp(cmd, "help")) { usage(); return 0; }
    fprintf(stderr, "未知子命令：%s\n\n", cmd);
    usage();
    return 2;
}