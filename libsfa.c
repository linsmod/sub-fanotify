/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* libsfa.c - 客户端 SDK 实现 */
#define _GNU_SOURCE
#include "sfa.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

int sfa_connect2(const char *sock_path, struct sfa_welcome *welcome)
{
    if (sock_path == NULL) sock_path = SFA_SOCKET_PATH;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* 读取欢迎消息并校验协议版本 */
    struct sfa_welcome w;
    ssize_t n = recv(fd, &w, sizeof(w), 0);
    if (n != (ssize_t)sizeof(w) || w.version != SFA_PROTO_VERSION) {
        close(fd);
        errno = EPROTO;
        return -1;
    }
    if (welcome) *welcome = w;
    return fd;
}

int sfa_connect(const char *sock_path)
{
    return sfa_connect2(sock_path, NULL);
}

int sfa_subscribe(int fd, uint32_t mask)
{
    struct sfa_subscribe_req req = { .mask = mask };
    ssize_t n = send(fd, &req, sizeof(req), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(req)) return -1;
    return 0;
}

ssize_t sfa_recv(int fd, struct sfa_event *ev, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;   /* 超时 */

    ssize_t n = recv(fd, ev, sizeof(*ev), 0);
    if (n < 0) return -1;
    if (n == 0) return 0;    /* 对端关闭 */
    if (n != (ssize_t)sizeof(*ev)) { errno = EPROTO; return -1; }
    return n;
}

void sfa_close(int fd)
{
    close(fd);
}

const char *sfa_event_name(uint32_t type)
{
    switch (type) {
    case SFA_EV_CREATE:      return "CREATE";
    case SFA_EV_DELETE:      return "DELETE";
    case SFA_EV_MOVED_FROM:  return "MOVED_FROM";
    case SFA_EV_MOVED_TO:    return "MOVED_TO";
    case SFA_EV_CLOSE_WRITE: return "CLOSE_WRITE";
    case SFA_EV_ATTRIB:      return "ATTRIB";
    case SFA_EV_OVERFLOW:    return "OVERFLOW";
    case SFA_EV_MOVED:       return "MOVED";
    case SFA_EV_UNRESOLVED:  return "UNRESOLVED";
    default:                 return "UNKNOWN";
    }
}

/* 把 sfa_welcome.flags 拼成人可读串，便于日志与调试 */
const char *sfa_work_flags_str(uint32_t flags, char *buf, size_t len)
{
    if (!buf || len == 0) return "";
    buf[0] = '\0';
    if (flags == 0) {   /* 旧服务端，未报告 */
        strncat(buf, "(未报告)", len - 1);
        return buf;
    }
    static const struct { uint32_t bit; const char *name; } B[] = {
        { SFA_WF_MARK_MOUNT,      "MOUNT"          },
        { SFA_WF_MARK_FILESYSTEM, "FILESYSTEM"     },
        { SFA_WF_RENAME_PAIR,     "RENAME_PAIR"    },
        { SFA_WF_ONDIR,           "ONDIR"          },
        { SFA_WF_PREFIX_FILTER,   "PREFIX_FILTER"  },
        { SFA_WF_PATH_LOOKUP,     "PATH_LOOKUP"    },
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
        if (!(flags & B[i].bit)) continue;
        size_t used = strlen(buf);
        if (used) {
            if (used + 1 >= len) break;
            buf[used] = '|';
            buf[used + 1] = '\0';
        }
        strncat(buf, B[i].name, len - strlen(buf) - 1);
    }
    return buf;
}

/* 把完整掩码拼成 "CREATE|CLOSE_WRITE" 形式，便于日志与调试 */
const char *sfa_event_names(uint32_t mask, char *buf, size_t len)
{
    if (!buf || len == 0) return "";
    buf[0] = '\0';
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if (!(mask & bit)) continue;
        size_t used = strlen(buf);
        if (used) {
            if (used + 1 >= len) break;
            buf[used] = '|';
            buf[used + 1] = '\0';
            used++;
        }
        strncat(buf, sfa_event_name(bit), len - strlen(buf) - 1);
    }
    return buf;
}
