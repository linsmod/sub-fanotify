/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* sfa_server.c - 特权 fanotify 代理的库实现（issue #10）
 *
 * 逻辑与行为与 CLI（sfa-server.c）共用同一份实现：CLI 只是薄包装。
 * 所有可变状态都在 struct sfa_srv 里，同进程可开多个实例。
 * 详细约定见 sfa_server.h 顶部（权限边界、崩溃域）。
 */
#define _GNU_SOURCE
#include "sfa_server.h"
#include "sfa_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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
#define DESYNC_KICK_TIMEOUT_MS 10000

/* desynced 客户端持续这么久仍无法投递就断开。
 * 取 10 秒：一个正常排空的客户端 10 秒读不了一点 socket 只能是卡死了，
 * 继续占着槽位和 4KB 级的内核缓冲没有意义；取 0 等于立刻踢（丢掉补发
 * 信号的价值），取无限等于放任永不读取的客户端泄漏槽位（MAX_CLIENTS
 * 总共 64 个）。
 * 注意是墙钟而不是「失败次数/轮数」：一轮 poll 的时长取决于事件流量，
 * 一次突发里 send 可以在几毫秒内连续 EAGAIN 上百次，按次/按轮计数会把
 * 还在正常排空的客户端误杀（e2e 实测踩过）。 */

struct client {
    int      fd;
    uint32_t mask;
    int64_t  desync_since_ms; /* 投递缓冲满的起始时刻（monotonic ms），0 = 正常 */
};

struct sfa_srv {
    /* 配置（open 时拷贝，不持有调用方的字符串） */
    char        mount[SFA_MAX_PATH];
    char        sock[SFA_MAX_PATH];
    char        group[256];
    char        prefix[SFA_SRV_MAX_PREFIXES][SFA_MAX_PATH];
    int         nprefix;

    /* 运行时 */
    int         mount_fd;
    int         fan_fd;
    int         listen_fd;
    int         wake_r, wake_w;          /* 自管道：stop 从任何上下文唤醒 poll */
    struct client clients[MAX_CLIENTS];
    int         nclients;
    int         have_desynced;           /* 上轮结果，决定本轮 poll 超时 */
    uint32_t    welcome_flags;           /* issue #9 */
    char       *evbuf;

    volatile sig_atomic_t stop;

    /* 进程内订阅者：不经 socket，同步回调（见 sfa_server.h 的契约） */
    void      (*cb)(void *user, const struct sfa_event *ev);
    void       *cb_user;
    uint32_t    cb_mask;

    void      (*log)(void *user, const char *msg);
    void       *log_user;

    char        err[256];
};

/* ---- 日志与错误 ---- */

static void srv_log_raw(struct sfa_srv *s, const char *msg)
{
    if (s && s->log) s->log(s->log_user, msg);
    else { fputs(msg, stderr); fputc('\n', stderr); }
}

/* 默认带上 "sfa-server: " 前缀，与 CLI 的历史输出一致（e2e 脚本依赖其中的关键字） */
static void srv_log(struct sfa_srv *s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s && s->log) {
        s->log(s->log_user, buf);
    } else {
        fprintf(stderr, "sfa-server: %s\n", buf);
    }
}

static int srv_fail(struct sfa_srv *s, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s->err, sizeof(s->err), fmt, ap);
    va_end(ap);
    return -1;
}

/* 失败并同时记日志：错误串不带前缀，日志行带上 */
#define SRV_FAIL(s, ...) do { \
    srv_fail((s), __VA_ARGS__); \
    srv_log((s), "%s", (s)->err); \
    return -1; \
} while (0)

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

static void loss_noinfo(struct loss_stats *st)
{
    st->no_info++;
}

/* err 传 resolve 失败的 errno；双路径超长不是 syscall 失败，传 EMSGSIZE */
static void loss_resolve(struct loss_stats *st, int err)
{
    st->unresolved++;
    for (unsigned i = 0; i < st->nerrno; i++)
        if (st->errnos[i].err == err) { st->errnos[i].n++; return; }
    /* 超过 8 种 errno 时宁可少记分布也不丢总计数 */
    if (st->nerrno < LOSS_ERRNO_SLOTS) {
        st->errnos[st->nerrno].err = err;
        st->errnos[st->nerrno].n   = 1;
        st->nerrno++;
    }
}

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

static int path_allowed(const struct sfa_srv *s, const char *path)
{
    for (int i = 0; i < s->nprefix; i++)
        if (path_has_prefix(path, s->prefix[i])) return 1;
    return 0;
}

/* 订阅过滤按完整掩码做交集：内核会把多个变化合并进一条事件，
 * 只看主事件会让订阅了其他位的客户端漏收。 */
static void broadcast(struct sfa_srv *s, const struct sfa_event *ev)
{
    for (int i = 0; i < s->nclients; i++) {
        if (s->clients[i].fd < 0) continue;
        if (!(s->clients[i].mask & ev->mask)) continue;
        /* 无路径事件（OVERFLOW/UNRESOLVED）一律放行，不参与前缀过滤：
         * 它们是「有东西丢了」的通知，按前缀挡掉等于把静默丢失重新引进来。 */
        if (s->nprefix && ev->path[0] && !path_allowed(s, ev->path)) continue;
        /* 非阻塞 send：慢客户端只丢它自己的事件（置 desynced，轮末补发
         * 丢失信号），绝不能停住整个代理 —— 否则可用性下限等于最慢订阅者。
         * SOCK_SEQPACKET 保证消息边界，一条事件要么完整送达要么没有。 */
        if (send(s->clients[i].fd, ev, sizeof(*ev),
                 MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                if (!s->clients[i].desync_since_ms) {
                    srv_log(s, "客户端 fd=%d 排空不及，开始丢弃它的事件（desynced）",
                            s->clients[i].fd);
                    s->clients[i].desync_since_ms = now_ms();
                }
            } else {
                /* EPIPE/ECONNRESET 等：立即断开。压缩逻辑只搬 fd<0 的条目、
                 * 不回收 fd，这里必须真的 close。 */
                close(s->clients[i].fd);
                s->clients[i].fd = -1;
            }
        }
    }

    /* 进程内订阅者：同步回调，与 socket 订阅者共用同一套过滤语义
     * （掩码、prefix、以及 OVERFLOW/UNRESOLVED 这类无路径的丢失信号）。
     * 它没有缓冲，所以不存在 desync 形态；代价是回调慢会推迟内核事件读取 ——
     * 契约写在 sfa_server.h，不在这里兜底。 */
    if (s->cb && (s->cb_mask & ev->mask)) {
        int filtered = s->nprefix && ev->path[0] && !path_allowed(s, ev->path);
        if (!filtered) s->cb(s->cb_user, ev);
    }
}

/* 批次收尾：有丢失就向客户端发一条 UNRESOLVED 信号事件，stderr 打一行计数。
 * 每批次一条而不是每次失败一条：客户端拿到第一条之后的动作是同一种
 * （补一次全量），精确计数对它没有价值，对运维有价值所以放 stderr。 */
static void report_loss(struct sfa_srv *s, const struct loss_stats *st)
{
    if (!st->no_info && !st->unresolved) return;

    char detail[256];
    int  off = snprintf(detail, sizeof(detail), "本批次丢失 %u 条事件"
                        "（内核未给信息 %u，反解失败 %u",
                        st->no_info + st->unresolved, st->no_info, st->unresolved);
    for (unsigned i = 0; i < st->nerrno && off < (int)sizeof(detail); i++)
        off += snprintf(detail + off, sizeof(detail) - (size_t)off, "%s%s x%u",
                        i == 0 ? "; " : ", ",
                        strerror(st->errnos[i].err), st->errnos[i].n);
    snprintf(detail + (off < (int)sizeof(detail) ? off : (int)sizeof(detail) - 2),
             sizeof(detail) - (size_t)(off < (int)sizeof(detail) ? off : (int)sizeof(detail) - 2),
             ")");
    srv_log(s, "%s", detail);

    struct sfa_event ev = {
        .type      = SFA_EV_UNRESOLVED,
        .mask      = SFA_EV_UNRESOLVED,
        .timestamp = now_ns(),
        .path_len  = 1,
    };
    ev.path[0] = '\0';
    broadcast(s, &ev);
}

/* 轮末对 desynced 客户端补发丢失信号（SFA_EV_UNRESOLVED，与反解失败共用
 * 同一种信号语义，不另开第三种）。收件人就是丢了事件的那个客户端本身，
 * 不走 broadcast 的订阅过滤 —— 它需要知道自己落后了；老客户端收到不认识
 * 的位不会崩，只是把 mask 打印成 UNKNOWN。
 * 发成功说明缓冲已排空，恢复投递；持续 DESYNC_KICK_TIMEOUT_MS 仍发不出去
 * 则断开（上界理由见该宏注释）。本函数可安全地被重复调用。 */
static void desync_kick(struct sfa_srv *s)
{
    struct sfa_event ev = {
        .type      = SFA_EV_UNRESOLVED,
        .mask      = SFA_EV_UNRESOLVED,
        .timestamp = now_ns(),
        .path_len  = 1,
    };
    ev.path[0] = '\0';
    int64_t now = now_ms();

    s->have_desynced = 0;
    for (int i = 0; i < s->nclients; i++) {
        if (s->clients[i].fd < 0 || !s->clients[i].desync_since_ms) continue;
        if (now - s->clients[i].desync_since_ms >= DESYNC_KICK_TIMEOUT_MS) {
            srv_log(s, "客户端 fd=%d 持续 %d 秒未排空，断开",
                    s->clients[i].fd, DESYNC_KICK_TIMEOUT_MS / 1000);
            close(s->clients[i].fd);
            s->clients[i].fd = -1;
            continue;
        }
        if (send(s->clients[i].fd, &ev, sizeof(ev), MSG_NOSIGNAL | MSG_DONTWAIT)
                == (ssize_t)sizeof(ev)) {
            s->clients[i].desync_since_ms = 0;
        } else if (errno == EPIPE || errno == ECONNRESET) {
            close(s->clients[i].fd);
            s->clients[i].fd = -1;
        } else {
            s->have_desynced = 1;   /* 还满着，下一轮再试 */
        }
    }
}

/* 把探测协商出的工作模式编码成 sfa_welcome.flags（issue #9）。
 * 客户端据此判断事件语义：监控范围有多大、rename 是否成对、目录事件是否可见、
 * 事件是否已按前缀裁剪、路径反解是否可用。 */
static uint32_t work_flags(const struct sfa_caps *caps, int nprefix)
{
    uint32_t f = 0;
    if (caps->mark_flags == FAN_MARK_MOUNT)
        f |= SFA_WF_MARK_MOUNT;
    else if (caps->mark_flags == FAN_MARK_FILESYSTEM)
        f |= SFA_WF_MARK_FILESYSTEM;
    if (caps->rename_ok) f |= SFA_WF_RENAME_PAIR;
    if (caps->ondir_ok)  f |= SFA_WF_ONDIR;
    if (caps->paths_ok)  f |= SFA_WF_PATH_LOOKUP;
    if (nprefix > 0)     f |= SFA_WF_PREFIX_FILTER;
    return f;
}

/* 能力报告也走日志出口：内嵌方设了回调就能自己决定要不要打、打到哪 */
static void log_probe_report(struct sfa_srv *s, const struct sfa_caps *caps)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&buf, &len);

    if (!f) return;
    sfa_probe_print(caps, f);
    fclose(f);
    if (!buf) return;

    for (char *line = buf, *nl; (nl = strchr(line, '\n')) != NULL; line = nl + 1) {
        *nl = '\0';
        srv_log_raw(s, line);
    }
    free(buf);
}

/* ---- 生命周期 ---- */

int sfa_srv_open(struct sfa_srv **out, const struct sfa_srv_opts *opts)
{
    if (!out || !opts || !opts->mount) return -1;

    struct sfa_srv *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->mount_fd = s->fan_fd = s->listen_fd = s->wake_r = s->wake_w = -1;
    s->log      = opts->log;
    s->log_user = opts->log_user;
    s->cb        = opts->on_event;
    s->cb_user   = opts->on_event_user;
    s->cb_mask   = opts->on_event ? opts->on_event_mask : 0;

    snprintf(s->mount, sizeof(s->mount), "%s", opts->mount);
    snprintf(s->sock,  sizeof(s->sock),  "%s",
             opts->sock ? opts->sock : SFA_SOCKET_PATH);
    if (opts->group) snprintf(s->group, sizeof(s->group), "%s", opts->group);

    /* 前缀表拷进实例：调用方的数组可能先于实例释放；相对前缀在此拒绝 ——
     * 事件路径全部来自 /proc/self/fd 的 readlink，一定是绝对路径，相对前缀会
     * 一条都不匹配而静默收不到事件。 */
    for (int i = 0; i < opts->nprefix; i++) {
        if (i >= SFA_SRV_MAX_PREFIXES)
            SRV_FAIL(s, "prefix 最多 %d 个", SFA_SRV_MAX_PREFIXES);
        if (!opts->prefix || !opts->prefix[i])
            SRV_FAIL(s, "prefix 表为空项");
        if (opts->prefix[i][0] != '/')
            SRV_FAIL(s, "prefix 必须是绝对路径: %s", opts->prefix[i]);
        snprintf(s->prefix[i], sizeof(s->prefix[i]), "%s", opts->prefix[i]);
        s->nprefix++;
    }

    /* 0. 探测本机 fanotify 能力，并据此协商 init/mark/mask */
    struct sfa_caps caps;
    sfa_probe(s->mount, &caps);
    log_probe_report(s, &caps);
    if (!caps.usable)
        SRV_FAIL(s, "当前环境不满足所需 fanotify 能力，退出");
    if (!caps.paths_ok)
        srv_log(s, "警告: open_by_handle_at 不可用，无路径的事件将被丢弃");

    /* 1. 打开挂载点，用于 open_by_handle_at */
    s->mount_fd = open(s->mount, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (s->mount_fd < 0)
        SRV_FAIL(s, "open mount %s: %s", s->mount, strerror(errno));

    /* 2. fanotify：按探测结果初始化并下发给目标路径 */
    s->fan_fd = fanotify_init(caps.init_flags, O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (s->fan_fd < 0)
        SRV_FAIL(s, "fanotify_init: %s", strerror(errno));

    if (fanotify_mark(s->fan_fd, FAN_MARK_ADD | caps.mark_flags,
                      caps.mask, AT_FDCWD, s->mount) < 0)
        SRV_FAIL(s, "fanotify_mark %s: %s", s->mount, strerror(errno));

    /* 3. 自管道：sfa_srv_stop() 在里面写一字节唤醒 poll（write 是
     * async-signal-safe 的，所以信号处理器里调用 stop 也安全） */
    int wake[2];
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0)
        SRV_FAIL(s, "pipe2: %s", strerror(errno));
    s->wake_r = wake[0];
    s->wake_w = wake[1];

    /* 4. 监听套接字 */
    unlink(s->sock);
    s->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s->listen_fd < 0)
        SRV_FAIL(s, "socket: %s", strerror(errno));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    /* sun_path 上限 107 字节（含 NUL 共 108）：超了就直接报错，不静默截断 ——
     * 截断后的路径 bind 得成功，但客户端按原路径连不上，是难查的部署故障。 */
    if (strlen(s->sock) >= sizeof(addr.sun_path))
        SRV_FAIL(s, "socket 路径过长（上限 %zu 字节）: %s",
                 sizeof(addr.sun_path) - 1, s->sock);
    memcpy(addr.sun_path, s->sock, strlen(s->sock) + 1);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        SRV_FAIL(s, "bind %s: %s", s->sock, strerror(errno));

    /* 属组必须在 chmod 之前设好：chown 会清掉 set-id 位，而最终权限由后面那次
     * chmod 决定，所以顺序不能反。--group 失败一律报错，不静默退回 0600 ——
     * 那正是这个选项要修的那个形态。 */
    if (s->group[0]) {
        struct group *gr = getgrnam(s->group);
        if (!gr)
            SRV_FAIL(s, "未知组 %s", s->group);
        if (chown(s->sock, 0, gr->gr_gid) < 0)
            SRV_FAIL(s, "chown %s: %s", s->sock, strerror(errno));
        if (chmod(s->sock, 0660) < 0)
            SRV_FAIL(s, "chmod %s: %s", s->sock, strerror(errno));
    } else if (chmod(s->sock, 0600) < 0) {
        SRV_FAIL(s, "chmod %s: %s", s->sock, strerror(errno));
    }

    if (listen(s->listen_fd, MAX_CLIENTS) < 0)
        SRV_FAIL(s, "listen: %s", strerror(errno));

    s->evbuf = malloc(EVENT_BUF_SIZE);
    if (!s->evbuf)
        SRV_FAIL(s, "malloc: %s", strerror(errno));

    /* 工作模式在协商完成后就固定，握手时直接带给客户端（issue #9） */
    s->welcome_flags = work_flags(&caps, s->nprefix);

    if (s->group[0])
        srv_log(s, "mount=%s socket=%s mode=0660 owner=root:%s",
                s->mount, s->sock, s->group);
    else
        srv_log(s, "mount=%s socket=%s mode=0600 owner=root:root",
                s->mount, s->sock);

    *out = s;
    return 0;
}

void sfa_srv_close(struct sfa_srv *s)
{
    if (!s) return;

    for (int i = 0; i < s->nclients; i++)
        if (s->clients[i].fd >= 0) close(s->clients[i].fd);

    free(s->evbuf);
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->fan_fd    >= 0) close(s->fan_fd);
    if (s->mount_fd  >= 0) close(s->mount_fd);
    if (s->wake_r    >= 0) close(s->wake_r);
    if (s->wake_w    >= 0) close(s->wake_w);
    if (s->sock[0]) unlink(s->sock);
    free(s);
}

void sfa_srv_stop(struct sfa_srv *s)
{
    if (!s) return;
    s->stop = 1;
    if (s->wake_w >= 0) {
        /* async-signal-safe；管道满时丢弃信号无妨，stop 标志已经置上 */
        ssize_t r = write(s->wake_w, "x", 1);
        (void)r;
    }
}

int sfa_srv_stopped(const struct sfa_srv *s)
{
    return s ? s->stop : 1;
}

int sfa_srv_fd(const struct sfa_srv *s)
{
    return s ? s->fan_fd : -1;
}

const char *sfa_srv_error(const struct sfa_srv *s)
{
    return s ? s->err : "";
}

const char *sfa_srv_mount(const struct sfa_srv *s)
{
    return s ? s->mount : "";
}

uint32_t sfa_srv_work_flags(const struct sfa_srv *s)
{
    return s ? s->welcome_flags : 0;
}

/* ---- 主循环 ---- */

/* fanotify 事件 fd 上的一批事件 */
static void handle_fanotify(struct sfa_srv *s)
{
    ssize_t len = read(s->fan_fd, s->evbuf, EVENT_BUF_SIZE);
    if (len <= 0) return;

    struct fanotify_event_metadata *meta =
        (struct fanotify_event_metadata *)s->evbuf;
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
            broadcast(s, &ev);
        } else if (meta->mask & FAN_RENAME) {
            /* 一条事件同时带旧路径（OLD）和新路径（NEW），天然配对 */
            struct fanotify_event_info_fid *oldf =
                find_fid_info(meta, FAN_EVENT_INFO_TYPE_OLD_DFID_NAME);
            struct fanotify_event_info_fid *newf =
                find_fid_info(meta, FAN_EVENT_INFO_TYPE_NEW_DFID_NAME);
            char newp[SFA_MAX_PATH], oldp[SFA_MAX_PATH];

            if (oldf && newf && mask) {
                int rn = resolve_dfid_event(s->mount_fd, newf, 1,
                                            newp, sizeof(newp));
                int ro = rn == 0 ? resolve_dfid_event(s->mount_fd, oldf, 1,
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
                        broadcast(s, &ev);
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
            /* 按类型显式取，优先 DFID_NAME（父目录句柄 + 名字，路径可拼到
             * 文件级）；没有它再退到 DFID / FID（对象自身句柄）。 */
            struct fanotify_event_info_fid *fid =
                find_fid_info(meta, FAN_EVENT_INFO_TYPE_DFID_NAME);
            int is_dirent = fid != NULL;
            if (!fid) fid = find_fid_info(meta, FAN_EVENT_INFO_TYPE_DFID);
            if (!fid) fid = find_fid_info(meta, FAN_EVENT_INFO_TYPE_FID);

            if (fid && mask) {
                struct sfa_event ev = {
                    .mask      = mask,
                    .flags     = flags,
                    .pid       = meta->pid,
                    .timestamp = now_ns(),
                };
                if (resolve_dfid_event(s->mount_fd, fid, is_dirent,
                                       ev.path, sizeof(ev.path)) == 0) {
                    ev.type     = sfa_primary_type(mask);
                    ev.path_len = (uint32_t)strlen(ev.path) + 1;
                    broadcast(s, &ev);
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

    report_loss(s, &loss);
}

static void handle_accept(struct sfa_srv *s)
{
    int cfd = accept(s->listen_fd, NULL, NULL);
    if (cfd < 0) return;

    struct sfa_welcome w = { .version = SFA_PROTO_VERSION,
                             .flags   = s->welcome_flags };
    snprintf(w.mount, sizeof(w.mount), "%s", s->mount);

    if (send(cfd, &w, sizeof(w), MSG_NOSIGNAL) < 0) {
        close(cfd);
    } else if (s->nclients < MAX_CLIENTS) {
        s->clients[s->nclients].fd               = cfd;
        s->clients[s->nclients].mask             = 0;   /* 未订阅前不接收 */
        s->clients[s->nclients].desync_since_ms  = 0;
        s->nclients++;
    } else {
        close(cfd);
    }
}

/* 跑一轮。cli_base 由调用方给出 pollfd 中第一个客户端的下标 */
static int srv_round(struct sfa_srv *s, int timeout_ms)
{
    struct pollfd pfds[MAX_CLIENTS + 3];
    int nfds = 0;

    pfds[nfds++] = (struct pollfd){ .fd = s->fan_fd,    .events = POLLIN };
    pfds[nfds++] = (struct pollfd){ .fd = s->listen_fd, .events = POLLIN };
    pfds[nfds++] = (struct pollfd){ .fd = s->wake_r,    .events = POLLIN };
    int cli_base = nfds;
    for (int i = 0; i < s->nclients; i++)
        pfds[nfds++] = (struct pollfd){ .fd = s->clients[i].fd, .events = POLLIN };

    int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;      /* 交给调用方检查 stop */
        return srv_fail(s, "poll: %s", strerror(errno));
    }

    if (pfds[2].revents & POLLIN) {        /* 被 stop 唤醒：抽干即可 */
        char drain[64];
        while (read(s->wake_r, drain, sizeof(drain)) > 0) { }
    }

    if (pfds[0].revents & POLLIN)
        handle_fanotify(s);

    if (pfds[1].revents & POLLIN)
        handle_accept(s);

    /* --- 客户端消息 --- */
    for (int i = 0; i < s->nclients; i++) {
        short rev = pfds[cli_base + i].revents;
        if (s->clients[i].fd < 0) continue;   /* 本轮 broadcast 中刚断开 */
        if (!(rev & (POLLIN | POLLHUP | POLLERR))) continue;

        struct sfa_subscribe_req req;
        ssize_t n = recv(s->clients[i].fd, &req, sizeof(req), 0);
        if (n <= 0) {
            close(s->clients[i].fd);
            s->clients[i].fd = -1;   /* 标记删除 */
            continue;
        }
        if (n == (ssize_t)sizeof(req))
            s->clients[i].mask = req.mask;
    }

    /* 轮末补发丢失信号：必须在压缩之前，它也可能把 fd 置成 -1 */
    desync_kick(s);

    /* 压缩客户端列表 */
    int w = 0;
    for (int i = 0; i < s->nclients; i++) {
        if (s->clients[i].fd < 0) continue;
        if (w != i) s->clients[w] = s->clients[i];
        w++;
    }
    s->nclients = w;
    return 0;
}

int sfa_srv_poll(struct sfa_srv *s, int timeout_ms)
{
    if (!s) return -1;
    if (s->stop) return 0;
    return srv_round(s, timeout_ms);
}

int sfa_srv_run(struct sfa_srv *s)
{
    if (!s) return -1;

    while (!s->stop) {
        /* 有 desynced 客户端时给 1s 超时：补发轮次才有时间下界，否则
         * 一个不读的客户端配一个安静的系统会让补发永远不发生。 */
        int timeout = s->have_desynced ? 1000 : -1;
        if (srv_round(s, timeout) < 0) return -1;
    }
    return 0;
}

/* ---- 自检（纯逻辑，不需要特权） ---- */

/* 前缀比较错得很安静（少收或多收事件都不会报错），所以留一个可运行入口 */
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

/* work_flags 错了不会崩，只会让客户端对事件语义判断错，所以也留一个自检。
 * 检查三件事：MARK_MOUNT / MARK_FILESYSTEM 互斥且新服务端必居其一、
 * 给了 --prefix 才置 PREFIX_FILTER、能力缺失时不置对应位。 */
static int selftest_work_flags(void)
{
    struct sfa_caps c;
    int bad = 0;

    memset(&c, 0, sizeof(c));
    c.mark_flags = FAN_MARK_MOUNT;
    c.rename_ok = c.ondir_ok = c.paths_ok = 1;

    uint32_t fm = work_flags(&c, 0);
    if (!(fm & SFA_WF_MARK_MOUNT) || (fm & SFA_WF_MARK_FILESYSTEM)) {
        fprintf(stderr, "selftest: MOUNT 模式应置 MOUNT 位且不置 FILESYSTEM 位\n");
        bad++;
    }
    if (fm & SFA_WF_PREFIX_FILTER) {
        fprintf(stderr, "selftest: 未给 --prefix 不应置 PREFIX_FILTER\n");
        bad++;
    }

    c.mark_flags = FAN_MARK_FILESYSTEM;
    uint32_t ff = work_flags(&c, 2);
    if (!(ff & SFA_WF_MARK_FILESYSTEM) || (ff & SFA_WF_MARK_MOUNT)) {
        fprintf(stderr, "selftest: FILESYSTEM 模式应置 FILESYSTEM 位且不置 MOUNT 位\n");
        bad++;
    }
    if (!(ff & SFA_WF_PREFIX_FILTER)) {
        fprintf(stderr, "selftest: 给了 --prefix 应置 PREFIX_FILTER\n");
        bad++;
    }

    c.rename_ok = c.ondir_ok = c.paths_ok = 0;
    uint32_t f0 = work_flags(&c, 0);
    if (f0 & (SFA_WF_RENAME_PAIR | SFA_WF_ONDIR | SFA_WF_PATH_LOOKUP)) {
        fprintf(stderr, "selftest: 能力缺失时不应置对应位\n");
        bad++;
    }

    if (bad) return 1;
    printf("selftest: work_flags 全过\n");
    return 0;
}

int sfa_srv_selftest(void)
{
    int rc = selftest_prefix();
    if (selftest_work_flags()) rc = 1;
    return rc;
}
