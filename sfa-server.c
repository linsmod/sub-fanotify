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

#define MAX_CLIENTS      64
#define EVENT_BUF_SIZE   (64 * 1024)

struct client {
    int      fd;
    uint32_t mask;
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
        if (plen + 1 + nlen + 1 > outlen) return -1;
        if (plen == 0 || out[plen - 1] != '/') out[plen++] = '/';
        memcpy(out + plen, name, nlen);
        out[plen + nlen] = '\0';
    }
    return 0;
}

/* 从事件元数据中提取指定类型的 fid 信息记录；type 传 0 表示任意 fid 记录 */
static struct fanotify_event_info_fid *
find_fid_info(struct fanotify_event_metadata *meta, int type)
{
    char *p   = (char *)meta + sizeof(*meta);
    char *end = (char *)meta + meta->event_len;

    while (p + sizeof(struct fanotify_event_info_header) <= end) {
        struct fanotify_event_info_header *h =
            (struct fanotify_event_info_header *)p;
        if (h->len < sizeof(*h)) break;
        if (type == 0) {
            if (h->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
                h->info_type == FAN_EVENT_INFO_TYPE_DFID ||
                h->info_type == FAN_EVENT_INFO_TYPE_FID)
                return (struct fanotify_event_info_fid *)p;
        } else if ((int)h->info_type == type) {
            return (struct fanotify_event_info_fid *)p;
        }
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

/* 订阅过滤按完整掩码做交集：内核会把多个变化合并进一条事件，
 * 只看主事件会让订阅了其他位的客户端漏收。 */
static void broadcast(struct client *clients, int nclients,
                      const struct sfa_event *ev)
{
    for (int i = 0; i < nclients; i++) {
        if (!(clients[i].mask & ev->mask)) continue;
        /* 事件较大，用阻塞 send 保证完整；SOCK_SEQPACKET 保证消息边界 */
        if (send(clients[i].fd, ev, sizeof(*ev), MSG_NOSIGNAL) < 0) {
            /* 单次失败不清理；下一轮 poll 会看到 POLLHUP/ERR 再清理 */
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法:\n"
            "  %s <mount-path> [socket-path]   启动事件代理\n"
            "  %s --probe <mount-path>         只探测本机 fanotify 能力并退出\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }

    /* --probe：只打印能力报告，不启动服务 */
    if (strcmp(argv[1], "--probe") == 0 || strcmp(argv[1], "-p") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        struct sfa_caps caps;
        sfa_probe(argv[2], &caps);
        sfa_probe_print(&caps, stdout);
        return caps.usable ? 0 : 1;
    }

    const char *mount_path = argv[1];
    const char *sock_path  = argc > 2 ? argv[2] : SFA_SOCKET_PATH;

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
    chmod(sock_path, 0600);   /* 收紧权限；需要多用户访问时调整 */
    if (listen(listen_fd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "sfa-server: mount=%s socket=%s\n", mount_path, sock_path);

    struct client clients[MAX_CLIENTS];
    int nclients = 0;
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

        int pr = poll(pfds, nfds, -1);
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

                        if (oldf && newf && mask &&
                            resolve_dfid_event(mount_fd, newf, 1, newp, sizeof(newp)) == 0 &&
                            resolve_dfid_event(mount_fd, oldf, 1, oldp, sizeof(oldp)) == 0) {
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
                            }
                        }
                    } else {
                        struct fanotify_event_info_fid *fid = find_fid_info(meta, 0);
                        /* 只有 DFID_NAME 记录是「父目录句柄 + 名字」，
                         * FID/DFID 记录的句柄就是对象本身，不能按事件类型猜。 */
                        int is_dirent = fid &&
                            fid->hdr.info_type == FAN_EVENT_INFO_TYPE_DFID_NAME;

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
                            }
                        }
                    }
                    if (meta->fd >= 0) close(meta->fd);   /* 关键：防止 fd 泄漏 */
                    meta = FAN_EVENT_NEXT(meta, len);
                }
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
                    clients[nclients].fd   = cfd;
                    clients[nclients].mask = 0;      /* 未订阅前不接收 */
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
