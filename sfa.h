/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* sfa.h - SFA 协议定义（服务端与客户端共享） */
#ifndef SFA_H
#define SFA_H

#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define SFA_SOCKET_PATH "/run/sfa.sock"
#define SFA_MAX_PATH    4096
#define SFA_MAX_NAME    256
#define SFA_PROTO_VERSION 1

/* 注：fanotify 相关常量（含老 glibc 的兜底）统一在 sfa_probe.h 中提供，
 * 本头文件保持为纯协议定义，客户端 SDK 不依赖 fanotify。 */

/* 客户端订阅的事件类型（位掩码） */
enum sfa_event_type {
    SFA_EV_CREATE      = 1u << 0,
    SFA_EV_DELETE      = 1u << 1,
    SFA_EV_MOVED_FROM  = 1u << 2,
    SFA_EV_MOVED_TO    = 1u << 3,
    SFA_EV_CLOSE_WRITE = 1u << 4,
    SFA_EV_ATTRIB      = 1u << 5,
    SFA_EV_OVERFLOW    = 1u << 6,
    /* rename：内核 5.17+ 用一条 FAN_RENAME 事件同时给出旧路径与新路径，
     * 无需像 MOVED_FROM/MOVED_TO 那样配对。仅在内核支持时才会出现。 */
    SFA_EV_MOVED       = 1u << 7,
    /* 丢失信号：有事件发生，但没能以可解析路径送达 —— 路径反解失败
     * （句柄已过期、双路径超长）或投递缓冲满被丢弃（服务端背压）。
     * path 为空（path_len == 1），与 SFA_EV_OVERFLOW 同族：客户端收到后
     * 应把索引标脏做一次全量对账。 */
    SFA_EV_UNRESOLVED  = 1u << 8,
};

#define SFA_EV_ALL (SFA_EV_CREATE | SFA_EV_DELETE | SFA_EV_MOVED_FROM | \
                    SFA_EV_MOVED_TO | SFA_EV_CLOSE_WRITE | SFA_EV_ATTRIB | \
                    SFA_EV_OVERFLOW | SFA_EV_MOVED | SFA_EV_UNRESOLVED)

/* sfa_event.flags 标志（与事件类型分开，不参与订阅过滤） */
#define SFA_F_ONDIR  0x1u   /* 事件对象是目录（mkdir/rmdir/目录 rename/chmod 目录） */

/* 事件消息（服务端 -> 客户端）
 *
 * 一条内核事件对应一条 sfa_event，不做拆分：内核常把同一对象上的多次变化
 * 合并成一条（例如 touch 产生 CREATE|CLOSE_WRITE|ATTRIB），拆分会丢掉
 * "这些变化同属一次操作"的信息，也会把 4KB 消息放大成多条。
 * 因此：
 *   - 只关心主语义的客户端：switch (ev.type)，type 是 mask 中最低位。
 *   - 需要完整语义的客户端：if (ev.mask & SFA_EV_X) 逐位判断。
 *
 * path 缓冲区布局：
 *   - 普通事件："/path/to/file\0"，path2_off = 0
 *   - SFA_EV_MOVED："/new/path\0/old/path\0"，path2_off 指向旧路径
 */
struct sfa_event {
    uint32_t type;        /* 主事件，单一位（= mask 中最低位） */
    uint32_t mask;        /* 本次内核事件的完整位掩码，可能多位 */
    uint32_t pid;         /* 触发进程 PID */
    uint32_t path2_off;   /* 第二条路径在 path 中的偏移，0 表示无 */
    uint64_t timestamp;   /* CLOCK_REALTIME 纳秒 */
    uint32_t path_len;    /* path 中有效字节数（含其中全部 '\0'） */
    uint32_t flags;       /* SFA_F_* 标志，如 SFA_F_ONDIR */
    char     path[SFA_MAX_PATH];
};

/* 订阅请求（客户端 -> 服务端） */
struct sfa_subscribe_req {
    uint32_t mask;        /* SFA_EV_* 位掩码；0 表示取消订阅 */
    uint32_t reserved;
};

/* sfa_welcome.flags：服务端工作模式，连接握手即可知（issue #9）。
 *
 * 该字段此前是未使用的 reserved，沿用同一 4 字节位置，sizeof(struct sfa_welcome)
 * 与解析方式都不变，因此不升 SFA_PROTO_VERSION；老客户端把非零值当垃圾忽略即可。
 * 填充规则：服务端按实测协商结果置位；客户端必须忽略不认识的位。
 * flags == 0 表示「旧服务端，未报告」——新服务端 MARK_MOUNT / MARK_FILESYSTEM 必居其一。 */
#define SFA_WF_MARK_MOUNT      0x0001u  /* 监控范围 = 挂载点（FAN_MARK_MOUNT） */
#define SFA_WF_MARK_FILESYSTEM 0x0002u  /* 监控范围 = 整个文件系统（降级路径，覆盖更宽） */
#define SFA_WF_RENAME_PAIR     0x0004u  /* FAN_RENAME：SFA_EV_MOVED 单条带旧+新路径 */
#define SFA_WF_ONDIR           0x0008u  /* FAN_ONDIR：目录自身的事件可见 */
#define SFA_WF_PREFIX_FILTER   0x0010u  /* 服务端已按 --prefix 裁剪事件 */
#define SFA_WF_PATH_LOOKUP     0x0020u  /* open_by_handle_at 可用（否则路径反解失败率高） */

/* 连接建立后服务端推送的欢迎消息 */
struct sfa_welcome {
    uint32_t version;
    uint32_t flags;                /* SFA_WF_*；0 = 旧服务端未报告 */
    char     mount[SFA_MAX_PATH];  /* 被监控的挂载点 */
};

/* 把 flags 拼成 "FILESYSTEM|RENAME_PAIR|ONDIR" 形式；0 得到 "(未报告)" */
const char *sfa_work_flags_str(uint32_t flags, char *buf, size_t len);

/* 取主路径 / 第二条路径（rename 的旧路径），无第二条路径时返回 NULL */
static inline const char *sfa_event_path(const struct sfa_event *ev)
{
    return ev->path;
}
static inline const char *sfa_event_path2(const struct sfa_event *ev)
{
    return ev->path2_off ? ev->path + ev->path2_off : NULL;
}

/* 事件对象是否为目录 */
static inline int sfa_event_is_dir(const struct sfa_event *ev)
{
    return (ev->flags & SFA_F_ONDIR) != 0;
}

/* ---- 客户端 SDK 接口 ---- */
int  sfa_connect(const char *sock_path);           /* 返回 fd，<0 失败 */
/* 同 sfa_connect，但把握手消息（协议版本、工作模式 flags、被监控的挂载点）交给调用方；
 * welcome 可为 NULL。旧服务端的 flags 为 0（表示未报告），不是错误。 */
int  sfa_connect2(const char *sock_path, struct sfa_welcome *welcome);
int  sfa_subscribe(int fd, uint32_t mask);         /* 0 成功 */
ssize_t sfa_recv(int fd, struct sfa_event *ev, int timeout_ms);
                                                   /* >0 收到事件；0 超时/对端关闭；<0 错误 */
void sfa_close(int fd);
const char *sfa_event_name(uint32_t type);                          /* 单个位 -> 名字 */
const char *sfa_event_names(uint32_t mask, char *buf, size_t len);   /* 掩码 -> "A|B|C" */

#endif /* SFA_H */
