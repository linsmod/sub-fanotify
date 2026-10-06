/* sfa-server.c - 特权 fanotify 代理：捕获事件并广播给订阅客户端 */
#define _GNU_SOURCE
#include "sfa.h"
#include "sfa_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/fanotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <grp.h>

#define MAX_CLIENTS      64
#define EVENT_BUF_SIZE   (64 * 1024)
#define MAX_PREFIXES     16

/* desynced 客户端持续这么久仍无法投递就断开。
 * 取 10 秒：一个正常排空的客户端 10 秒读不了一点 socket 只能是卡死了，
 * 继续占着槽位和 4KB 级的内核缓冲没有意义；取 0 等于立刻踢（丢掉补发
 * 信号的价值），取无限等于放任永不读取的客户端泄漏槽位（MAX_CLIENTS
 * 总共 64 个）。
 * 注意是墙钟而不是「失败次数/轮数」：一轮 poll 的时长取决于事件流量，
 * 一次突发里 send 可以在几毫秒内连续 EAGAIN 上百次，按次/按轮计数会把
 * 还在正常排空的客户端误杀（e2e 实测踩过）。 */
#define DESYNC_KICK_TIMEOUT_MS 10000

struct client {
    int      fd;
    uint32_t mask;
    int64_t  desync_since_ms; /* 投递缓冲满的起始时刻（monotonic ms），0 = 正常 */
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* monotonic 毫秒：desync 计时用，不受系统时间跳变影响 */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 将 fanotify_event_info_fid 解析为路径。
 * is_dirent=1：fid 为父目录句柄，名字紧跟其后；
 * is_dirent=0：fid 为文件句柄本身。 */
static int resolve_dfid_event(int mount_fd,
                              struct fanotify_event_info_fid *fid,
                              int is_dirent,
                              char *out, size_t outlen)
{
    struct file_handle *fh = (struct file_handle *)fid->handle;

    int hfd = open_by_handle_at(mount_fd, fh, O_RDONLY | O_CLOEXEC);
    if (hfd < 0) return -1;

    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", hfd);
    ssize_t n = readlink(link, out, outlen - 1);
    close(hfd);
    if (n < 0) return -1;
    out[n] = '\0';

    if (is_dirent) {
        const char *name = (const char *)fh->f_handle + fh->handle_bytes;
        size_t nlen = strnlen(name, SFA_MAX_NAME);
        /* 对目录自身的事件内核会填名字 "."，此时对象就是该目录，不再拼接 */
        if (nlen == 1 && name[0] == '.')
            return 0;
        size_t plen = (size_t)n;
        if (plen + 1 + nlen + 1 > outlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (plen == 0 || out[plen - 1] != '/') out[plen++] = '/';
        memcpy(out + plen, name, nlen);
        out[plen + nlen] = '\0';
    }
    return 0;
}

/* 从事件元数据中提取指定类型的 fid 信息记录。type 必须显式给出：
 * 曾经支持 type=0「任意 fid 记录、返回第一条」的宽松匹配，正确性隐式
 * 依赖「DFID_NAME 排在 FID 之前」的内核记录顺序 —— 顺序一旦翻转，
 * is_dirent 的猜测就会错，每条 DELETE 的路径都会带上 " (deleted)" 后缀，
 * 不报错不崩溃，只是内容错（issue #5 的实测与判别方法见
 * issues/closed/dfid-record-order-unknown.md）。按类型选取后与位置无关。 */
static struct fanotify_event_info_fid *
find_fid_info(struct fanotify_event_metadata *meta, int type)
{
    char *p   = (char *)meta + sizeof(*meta);
    char *end = (char *)meta + meta->event_len;

    while (p + sizeof(struct fanotify_event_info_header) <= end) {
        struct fanotify_event_info_header *h =
            (struct fanotify_event_info_header *)p;
        if (h->len < sizeof(*h)) break;
        if ((int)h->info_type == type)
            return (struct fanotify_event_info_fid *)p;
        p += h->len;
    }
    return NULL;
}

static uint32_t fanotify_mask_to_sfa(uint64_t mask)
{
    uint32_t t = 0;
    if (mask & FAN_CREATE)      t |= SFA_EV_CREATE;
    if (mask & FAN_DELETE)      t |= SFA_EV_DELETE;
    if (mask & FAN_MOVED_FROM)  t |= SFA_EV_MOVED_FROM;
    if (mask & FAN_MOVED_TO)    t |= SFA_EV_MOVED_TO;
    if (mask & FAN_CLOSE_WRITE) t |= SFA_EV_CLOSE_WRITE;
    if (mask & FAN_ATTRIB)      t |= SFA_EV_ATTRIB;
    if (mask & FAN_Q_OVERFLOW)  t |= SFA_EV_OVERFLOW;
    if (mask & FAN_RENAME)      t |= SFA_EV_MOVED;
    return t;
}

/* 主事件 = mask 中最低位，保证 ev->type 始终是单一位 */
static uint32_t sfa_primary_type(uint32_t mask)
{
    return mask ? (mask & (~mask + 1u)) : 0;
}

/* ---- 丢失统计（一次 read 批次） ----
 *
 * 两类失败性质不同，分开计数（issue #2）：
 *   no_info     内核没给出必要的 info 记录（fid / oldf / newf 为空）——
 *               说明协商或内核行为出了问题；
 *   unresolved  有记录但没能产出路径（open_by_handle_at 返回 ESTALE、
 *               双路径之和超出 4KB 等）—— fanotify 的时序窗口，删除比
 *               读事件快时必然发生。
 * 混在一个计数器里，stderr 上的数字会把这两种病因混成一件事。 */
#define LOSS_ERRNO_SLOTS 8
struct loss_stats {
    unsigned no_info;
    unsigned unresolved;
    struct { int err; unsigned n; } errnos[LOSS_ERRNO_SLOTS];
    unsigned nerrno;
};

static void loss_noinfo(struct loss_stats *s)
{
    s->no_info++;
}

static void broadcast(struct client *clients, int nclients,
                      const struct sfa_event *ev);   /* 定义在下方 */

/* err 传 resolve 失败的 errno；双路径超长不是 syscall 失败，传 EMSGSIZE */
static void loss_resolve(struct loss_stats *s, int err)
{
    s->unresolved++;
    for (unsigned i = 0; i < s->nerrno; i++)
        if (s->errnos[i].err == err) { s->errnos[i].n++; return; }
    /* 超过 8 种 errno 时宁可少记分布也不丢总计数 */
    if (s->nerrno < LOSS_ERRNO_SLOTS) {
        s->errnos[s->nerrno].err = err;
        s->errnos[s->nerrno].n   = 1;
        s->nerrno++;
    }
}

/* 批次收尾：有丢失就向客户端发一条 UNRESOLVED 信号事件，stderr 打一行计数。
 * 每批次一条而不是每次失败一条：客户端拿到第一条之后的动作是同一种
 * （补一次全量），精确计数对它没有价值，对运维有价值所以放 stderr。 */
static void report_loss(struct client *clients, int nclients,
                        const struct loss_stats *s)
{
    if (!s->no_info && !s->unresolved) return;

    fprintf(stderr, "sfa-server: 本批次丢失 %u 条事件（内核未给信息 %u，反解失败 %u",
            s->no_info + s->unresolved, s->no_info, s->unresolved);
    for (unsigned i = 0; i < s->nerrno; i++)
        fprintf(stderr, "%s%s x%u",
                i == 0 ? "; " : ", ", strerror(s->errnos[i].err), s->errnos[i].n);
    fprintf(stderr, ")\n");

    struct sfa_event ev = {
        .type      = SFA_EV_UNRESOLVED,
        .mask      = SFA_EV_UNRESOLVED,
        .timestamp = now_ns(),
        .path_len  = 1,
    };
    ev.path[0] = '\0';
    broadcast(clients, nclients, &ev);
}

/* --prefix 过滤表放在文件作用域：broadcast 被四处调用，而过滤对所有订阅者一致，
 * 不值得为它给broadcast 加参数。 */
static const char *g_prefix[MAX_PREFIXES];
static int         g_nprefix;

/* 前缀按路径分量比较，不能用裸 strncmp —— 那样 /usrlocal 会被 /usr 收进来。
 * 末尾多余的 '/' 忽略；"/" 表示整棵文件系统。 */
static int path_has_prefix(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    while (n > 1 && prefix[n - 1] == '/') n--;
    if (n == 1) return path[0] == '/';
    if (strncmp(path, prefix, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/';
}

static int path_allowed(const char *path)
{
    for (int i = 0; i < g_nprefix; i++)
        if (path_has_prefix(path, g_prefix[i])) return 1;
    return 0;
}

/* 订阅过滤按完整掩码做交集：内核会把多个变化合并进一条事件，
 * 只看主事件会让订阅了其他位的客户端漏收。 */
static void broadcast(struct client *clients, int nclients,
                      const struct sfa_event *ev)
{
    for (int i = 0; i < nclients; i++) {
        if (clients[i].fd < 0) continue;
        if (!(clients[i].mask & ev->mask)) continue;
        /* 无路径事件（OVERFLOW/UNRESOLVED）一律放行，不参与前缀过滤：
         * 它们是「有东西丢了」的通知，按前缀挡掉等于把静默丢失重新引进来。 */
        if (g_nprefix && ev->path[0] && !path_allowed(ev->path)) continue;
        /* 非阻塞 send：慢客户端只丢它自己的事件（置 desynced，轮末补发
         * 丢失信号），绝不能停住整个代理 —— 否则可用性下限等于最慢订阅者。
         * SOCK_SEQPACKET 保证消息边界，一条事件要么完整送达要么没有。 */
        if (send(clients[i].fd, ev, sizeof(*ev),
                 MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                if (!clients[i].desync_since_ms) {
                    fprintf(stderr, "sfa-server: 客户端 fd=%d 排空不及，"
                            "开始丢弃它的事件（desynced）\n", clients[i].fd);
                    clients[i].desync_since_ms = now_ms();
                }
            } else {
                /* EPIPE/ECONNRESET 等：立即断开。压缩逻辑只搬 fd<0 的条目、
                 * 不回收 fd，这里必须真的 close。 */
                close(clients[i].fd);
                clients[i].fd = -1;
            }
        }
    }
}

/* 轮末对 desynced 客户端补发丢失信号（SFA_EV_UNRESOLVED，与反解失败共用
 * 同一种信号语义，不另开第三种）。收件人就是丢了事件的那个客户端本身，
 * 不走 broadcast 的订阅过滤 —— 它需要知道自己落后了；老客户端收到不认识
 * 的位不会崩，只是把 mask 打印成 UNKNOWN。
 * 发成功说明缓冲已排空，恢复投递；持续 DESYNC_KICK_TIMEOUT_MS 仍发不出去
 * 则断开（上界理由见该宏注释）。本函数可安全地被重复调用。 */
static void desync_kick(struct client *clients, int nclients, int *have_desynced)
{
    struct sfa_event ev = {
        .type      = SFA_EV_UNRESOLVED,
        .mask      = SFA_EV_UNRESOLVED,
        .timestamp = now_ns(),
        .path_len  = 1,
    };
    ev.path[0] = '\0';
    int64_t now = now_ms();

    *have_desynced = 0;
    for (int i = 0; i < nclients; i++) {
        if (clients[i].fd < 0 || !clients[i].desync_since_ms) continue;
        if (now - clients[i].desync_since_ms >= DESYNC_KICK_TIMEOUT_MS) {
            fprintf(stderr, "sfa-server: 客户端 fd=%d 持续 %d 秒未排空，断开\n",
                    clients[i].fd, DESYNC_KICK_TIMEOUT_MS / 1000);
            close(clients[i].fd);
            clients[i].fd = -1;
            continue;
        }
        if (send(clients[i].fd, &ev, sizeof(ev), MSG_NOSIGNAL | MSG_DONTWAIT)
                == (ssize_t)sizeof(ev)) {
            clients[i].desync_since_ms = 0;
        } else if (errno == EPIPE || errno == ECONNRESET) {
            close(clients[i].fd);
            clients[i].fd = -1;
        } else {
            *have_desynced = 1;   /* 还满着，下一轮再试 */
        }
    }
}

/* 前缀比较错得很安静（少收或多收事件都不会报错），而仓库里没有测试框架，
 * 所以留一个可运行的自检入口：make selftest。 */
static int selftest_prefix(void)
{
    static const struct { const char *path, *prefix; int want; } T[] = {
        { "/usr/bin/tool", "/usr",   1 },
        { "/usr",          "/usr",   1 },
        { "/usr/",         "/usr",   1 },
        { "/usrlocal/x",   "/usr",   0 },
        { "/etc/passwd",   "/usr",   0 },
        { "/usr2",         "/usr",   0 },
        { "/a/b/c",        "/a/b",   1 },
        { "/a/bc",         "/a/b",   0 },
        { "/tmp/x",        "/",     1 },
        { "/usr/x",        "/usr/",  1 },
    };
    size_t n = sizeof(T) / sizeof(T[0]);
    size_t i;
    int bad = 0;

    for (i = 0; i < n; i++) {
        int got = path_has_prefix(T[i].path, T[i].prefix);
        if (got != T[i].want) {
            fprintf(stderr, "selftest: path_has_prefix(\"%s\", \"%s\") = %d，期望 %d\n",
                    T[i].path, T[i].prefix, got, T[i].want);
            bad++;
        }
    }
    if (bad) { fprintf(stderr, "selftest: %d/%zu 项失败\n", bad, n); return 1; }
    printf("selftest: path_has_prefix %zu/%zu 全过\n", n, n);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法:\n"
            "  %s <mount-path> [socket-path] [选项]\n"
            "  %s --probe <mount-path>只探测本机 fanotify 能力并退出\n"
            "\n"
            "选项:\n"
            "  --group <组名>    socket 属组设成该组、权限 0660（默认 0600 root:root）\n"
            "  --prefix <路径>   只推送该前缀下的事件，可重复；按路径分量比较\n"
            "  --selftest        运行内置自检（不需要挂载点）后退出\n"
            "  -h, --help        打印本用法\n",
            prog, prog);
}

struct options {
    const char *mount;
    const char *sock;
    const char *group;
    const char *prefix[MAX_PREFIXES];
    int         nprefix;
    int         probe;
    int         selftest;
};

/* 位置参数与选项可以任意顺序混排；未知选项一律报错，不静默当成路径。
 * 返回 0 正常，-1 出错，1 已打印用法（--help / 缺 mount）。 */
static int parse_args(int argc, char **argv, struct options *o)
{
    const char *pos[2] = { NULL, NULL };
    int npos = 0;

    memset(o, 0, sizeof(*o));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--probe") || !strcmp(a, "-p")) {
            o->probe = 1;
        } else if (!strcmp(a, "--selftest")) {
            o->selftest = 1;
        } else if (!strcmp(a, "--group")) {
            if (++i >= argc) { fprintf(stderr, "sfa-server: --group 缺少组名\n"); return -1; }
            o->group = argv[i];
        } else if (!strncmp(a, "--group=", 8)) {
            o->group = a + 8;
        } else if (!strcmp(a, "--prefix")) {
            if (++i >= argc) { fprintf(stderr, "sfa-server: --prefix 缺少路径\n"); return -1; }
            if (o->nprefix >= MAX_PREFIXES) {
                fprintf(stderr, "sfa-server: --prefix 最多 %d 个\n", MAX_PREFIXES);
                return -1;
            }
            o->prefix[o->nprefix++] = argv[i];
        } else if (!strncmp(a, "--prefix=", 9)) {
            if (o->nprefix >= MAX_PREFIXES) {
                fprintf(stderr, "sfa-server: --prefix 最多 %d 个\n", MAX_PREFIXES);
                return -1;
            }
            o->prefix[o->nprefix++] = a + 9;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "sfa-server: 未知选项 %s\n", a);
            return -1;
        } else if (npos >= 2) {
            fprintf(stderr, "sfa-server: 多余的位置参数 %s\n", a);
            return -1;
        } else {
            pos[npos++] = a;
        }
    }

    if (o->selftest && !pos[0]) return 0;   /* 自检不需要挂载点 */
    if (!pos[0]) { usage(argv[0]); return -1; }
    o->mount = pos[0];
    o->sock  = pos[1] ? pos[1] : SFA_SOCKET_PATH;
    return 0;
}

int main(int argc, char **argv)
{
    struct options o;
    int opt_rc = parse_args(argc, argv, &o);
    if (opt_rc < 0) return 1;
    if (opt_rc > 0) return 0;

    if (o.selftest) return selftest_prefix();

    /* 前缀必须是绝对路径：事件路径全部来自 /proc/self/fd 的 readlink，
     * 一定是绝对路径，相对前缀会一条都不匹配而静默收不到事件。 */
    for (int i = 0; i < o.nprefix; i++) {
        if (o.prefix[i][0] != '/') {
            fprintf(stderr, "sfa-server: --prefix 必须是绝对路径: %s\n", o.prefix[i]);
            return 1;
        }
        g_prefix[g_nprefix++] = o.prefix[i];
    }

    const char *mount_path = o.mount;
    const char *sock_path  = o.sock;

    /* --probe：只打印能力报告，不启动服务。原先靠 argv[1] 认自己，
     * 现在由parse_args 统一识别，位置不再敏感。 */
    if (o.probe) {
        struct sfa_caps caps;
        sfa_probe(mount_path, &caps);
        sfa_probe_print(&caps, stdout);
        return caps.usable ? 0 : 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* 0. 探测本机 fanotify 能力，并据此协商 init/mark/mask */
    struct sfa_caps caps;
    sfa_probe(mount_path, &caps);
    sfa_probe_print(&caps, stderr);
    if (!caps.usable) {
        fprintf(stderr, "sfa-server: 当前环境不满足所需 fanotify 能力，退出\n");
        return 1;
    }
    if (!caps.paths_ok)
        fprintf(stderr, "sfa-server: 警告: open_by_handle_at 不可用，无路径的事件将被丢弃\n");

    /* 1. 打开挂载点，用于 open_by_handle_at */
    int mount_fd = open(mount_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mount_fd < 0) { perror("open mount"); return 1; }

    /* 2. fanotify：按探测结果初始化并下发给目标路径 */
    int fan_fd = fanotify_init(caps.init_flags,
                               O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fan_fd < 0) { perror("fanotify_init"); return 1; }

    if (fanotify_mark(fan_fd, FAN_MARK_ADD | caps.mark_flags,
                      caps.mask, AT_FDCWD, mount_path) < 0) {
        perror("fanotify_mark");
        return 1;
    }

    /* 3. 监听套接字 */
    unlink(sock_path);
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    /* 属组必须在 chmod 之前设好：chown 会清掉 set-id 位，而最终权限由后面那次
     * chmod 决定，所以顺序不能反。--group 失败一律退出，不静默退回 0600 ——
     * 那正是这个选项要修的那个形态。 */
    if (o.group) {
        struct group *gr = getgrnam(o.group);
        if (!gr) {
            fprintf(stderr, "sfa-server: 未知组 %s\n", o.group);
            return 1;
        }
        if (chown(sock_path, 0, gr->gr_gid) < 0) { perror("chown"); return 1; }
        if (chmod(sock_path, 0660) < 0) { perror("chmod"); return 1; }
    } else if (chmod(sock_path, 0600) < 0) {
        perror("chmod");
        return 1;
    }
    if (listen(listen_fd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    if (o.group)
        fprintf(stderr, "sfa-server: mount=%s socket=%s mode=0660 owner=root:%s\n",
                mount_path, sock_path, o.group);
    else
        fprintf(stderr, "sfa-server: mount=%s socket=%s mode=0600 owner=root:root\n",
                mount_path, sock_path);

    struct client clients[MAX_CLIENTS];
    int nclients = 0;
    int have_desynced = 0;   /* 上一轮 desync_kick 的结果，决定本轮 poll 超时 */
    char *evbuf = malloc(EVENT_BUF_SIZE);
    if (!evbuf) { perror("malloc"); return 1; }

    while (!g_stop) {
        struct pollfd pfds[MAX_CLIENTS + 2];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = fan_fd,    .events = POLLIN };
        pfds[nfds++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        int cli_base = nfds;
        for (int i = 0; i < nclients; i++)
            pfds[nfds++] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };

        /* 有 desynced 客户端时给 1s 超时：desync_rounds 的「轮」才有时间
         * 下界，否则一个不读的客户端配一个安静的系统会让补发永远不发生。 */
        int pr = poll(pfds, nfds, have_desynced ? 1000 : -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* --- fanotify 事件 --- */
        if (pfds[0].revents & POLLIN) {
            ssize_t len = read(fan_fd, evbuf, EVENT_BUF_SIZE);
            if (len > 0) {
                struct fanotify_event_metadata *meta =
                    (struct fanotify_event_metadata *)evbuf;
                struct loss_stats loss = {0};

                while (FAN_EVENT_OK(meta, len)) {
                    uint32_t mask  = fanotify_mask_to_sfa(meta->mask);
                    uint32_t flags = (meta->mask & FAN_ONDIR) ? SFA_F_ONDIR : 0;

                    if (meta->mask & FAN_Q_OVERFLOW) {
                        struct sfa_event ev = {
                            .type = SFA_EV_OVERFLOW,
                            .mask = mask,
                            .pid = meta->pid,
                            .timestamp = now_ns(),
                            .path_len = 1,
                        };
                        ev.path[0] = '\0';
                        broadcast(clients, nclients, &ev);
                    } else if (meta->mask & FAN_RENAME) {
                        /* 一条事件同时带旧路径（OLD）和新路径（NEW），天然配对 */
                        struct fanotify_event_info_fid *oldf =
                            find_fid_info(meta, FAN_EVENT_INFO_TYPE_OLD_DFID_NAME);
                        struct fanotify_event_info_fid *newf =
                            find_fid_info(meta, FAN_EVENT_INFO_TYPE_NEW_DFID_NAME);
                        char newp[SFA_MAX_PATH], oldp[SFA_MAX_PATH];

                        if (oldf && newf && mask) {
                            int rn = resolve_dfid_event(mount_fd, newf, 1,
                                                        newp, sizeof(newp));
                            int ro = rn == 0 ? resolve_dfid_event(mount_fd, oldf, 1,
                                                                  oldp, sizeof(oldp))
                                             : -1;
                            if (rn == 0 && ro == 0) {
                                size_t n1 = strlen(newp) + 1, n2 = strlen(oldp) + 1;
                                if (n1 + n2 <= sizeof(((struct sfa_event *)0)->path)) {
                                    struct sfa_event ev = {
                                        .type      = SFA_EV_MOVED,
                                        .mask      = mask,
                                        .flags     = flags,
                                        .pid       = meta->pid,
                                        .timestamp = now_ns(),
                                        .path2_off = (uint32_t)n1,
                                        .path_len  = (uint32_t)(n1 + n2),
                                    };
                                    memcpy(ev.path, newp, n1);
                                    memcpy(ev.path + n1, oldp, n2);
                                    broadcast(clients, nclients, &ev);
                                } else {
                                    /* 双路径之和超出 4KB：事件真实存在但无法完整表达 */
                                    loss_resolve(&loss, EMSGSIZE);
                                }
                            } else {
                                loss_resolve(&loss, errno);
                            }
                        } else if (!oldf || !newf) {
                            /* mask==0 时事件类型不在我们的枚举里，不算丢失 */
                            loss_noinfo(&loss);
                        }
                    } else {
                        /* 按类型显式取，优先 DFID_NAME（父目录句柄 + 名字，
                         * 路径可拼到文件级）；没有它再退到 DFID / FID
                         * （对象自身句柄，此时不能按事件类型猜语义）。 */
                        struct fanotify_event_info_fid *fid =
                            find_fid_info(meta, FAN_EVENT_INFO_TYPE_DFID_NAME);
                        int is_dirent = fid != NULL;
                        if (!fid)
                            fid = find_fid_info(meta, FAN_EVENT_INFO_TYPE_DFID);
                        if (!fid)
                            fid = find_fid_info(meta, FAN_EVENT_INFO_TYPE_FID);

                        if (fid && mask) {
                            struct sfa_event ev = {
                                .mask      = mask,
                                .flags     = flags,
                                .pid       = meta->pid,
                                .timestamp = now_ns(),
                            };
                            if (resolve_dfid_event(mount_fd, fid, is_dirent,
                                                   ev.path, sizeof(ev.path)) == 0) {
                                ev.type     = sfa_primary_type(mask);
                                ev.path_len = (uint32_t)strlen(ev.path) + 1;
                                broadcast(clients, nclients, &ev);
                            } else {
                                loss_resolve(&loss, errno);
                            }
                        } else if (!fid) {
                            /* mask==0 时事件类型不在我们的枚举里，不算丢失 */
                            loss_noinfo(&loss);
                        }
                    }
                    if (meta->fd >= 0) close(meta->fd);   /* 关键：防止 fd 泄漏 */
                    meta = FAN_EVENT_NEXT(meta, len);
                }

                report_loss(clients, nclients, &loss);
            }
        }

        /* --- 新客户端 --- */
        if (pfds[1].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                struct sfa_welcome w = { .version = SFA_PROTO_VERSION };
                strncpy(w.mount, mount_path, sizeof(w.mount) - 1);
                if (send(cfd, &w, sizeof(w), MSG_NOSIGNAL) < 0) {
                    close(cfd);
                } else if (nclients < MAX_CLIENTS) {
                    clients[nclients].fd             = cfd;
                    clients[nclients].mask           = 0;   /* 未订阅前不接收 */
                    clients[nclients].desync_since_ms = 0;
                    nclients++;
                } else {
                    close(cfd);
                }
            }
        }

        /* --- 客户端消息 --- */
        for (int i = 0; i < nclients; i++) {
            int pidx = cli_base + i;
            short rev = pfds[pidx].revents;
            if (clients[i].fd < 0) continue;   /* 本轮 broadcast 中刚断开 */
            if (!(rev & (POLLIN | POLLHUP | POLLERR))) continue;

            struct sfa_subscribe_req req;
            ssize_t n = recv(clients[i].fd, &req, sizeof(req), 0);
            if (n <= 0) {
                close(clients[i].fd);
                clients[i].fd = -1;   /* 标记删除 */
                continue;
            }
            if (n == (ssize_t)sizeof(req))
                clients[i].mask = req.mask;
        }

        /* 轮末补发丢失信号：必须在压缩之前，它也可能把 fd 置成 -1 */
        desync_kick(clients, nclients, &have_desynced);

        /* 压缩客户端列表 */
        int w = 0;
        for (int i = 0; i < nclients; i++) {
            if (clients[i].fd < 0) continue;
            if (w != i) clients[w] = clients[i];
            w++;
        }
        nclients = w;
    }

    for (int i = 0; i < nclients; i++) close(clients[i].fd);
    free(evbuf);
    close(listen_fd);
    close(fan_fd);
    close(mount_fd);
    unlink(sock_path);
    return 0;
}
