/* sfa_server.h - 把特权 fanotify 代理嵌进调用方进程（issue #10）
 *
 * 用法骨架：
 *
 *   struct sfa_srv_opts o = { .mount = "/data" };
 *   struct sfa_srv *s;
 *   if (sfa_srv_open(&s, &o) < 0) return 1;   // 失败原因经日志回调给出
 *   sfa_srv_run(s);                           // 阻塞，直到 sfa_srv_stop()
 *   sfa_srv_close(s);
 *
 * 想接进自己的事件循环，就用 sfa_srv_poll()：
 *
 *   poll(...自己的 fd..., sfa_srv_fd(s), ...);   // 事件源就绪
 *   sfa_srv_poll(s, 0);                          // 让 server 处理这一轮
 *
 * 两个必须先知道的约束（与 README 的「特权 watcher + 无特权索引器分离」直接相关）：
 *
 *  1. **权限边界**：fanotify 需要 CAP_SYS_ADMIN，所以内嵌 server 的进程必须是特权
 *     进程。只有当调用方**本来就是一个独立的特权守护进程**时，内嵌才是合适的；
 *     把 watcher 塞进无特权的索引器，等于让索引器变成特权进程，也就把这个项目
 *     存在的理由取消了 —— 那种场景应该继续用子进程 + socket 的形态。
 *  2. **崩溃域合并**：server 跑在调用方进程里，它的崩溃会带走调用方；子进程模型下
 *     代理挂了只是代理挂了。这是内嵌换来的代价，调用方要自己接受。
 *
 * 同进程可以开多个实例：所有可变状态都在实例内，互不影响。
 */
#ifndef SFA_SERVER_H
#define SFA_SERVER_H

#include <stdint.h>
#include "sfa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 与 CLI 的 --prefix 上限一致 */
#define SFA_SRV_MAX_PREFIXES 16

struct sfa_srv_opts {
    const char *mount;              /* 必填：监控的挂载点/目录 */
    const char *sock;               /* NULL = SFA_SOCKET_PATH */
    const char *group;              /* 可空，同 CLI --group（socket 0660 root:GROUP） */
    const char *const *prefix;      /* 可空，同 CLI --prefix；open 时拷贝进实例 */
    int         nprefix;            /* prefix 个数 */
    /* 日志出口：NULL 表示直接写 stderr（与 CLI 行为一致）。回调里不要阻塞太久，
     * 它在主循环里被调用。能力探测报告也会走这里。 */
    void      (*log)(void *user, const char *msg);
    void       *log_user;

    /* 进程内订阅：事件不经 socket，按 mask 过滤后**同步回调**（issue #10 后续，
     * 单进程消费者不必再连自己的 socket）。NULL 表示不用这条通路。
     *
     * **回调必须快且不得阻塞**：它在 fanotify 读循环里执行，慢回调会推迟内核
     * 事件的读取，把缓冲压力交给内核队列 —— 句柄随之过期，于是把 #1（停摆）与
     * #2（静默丢失）的形态从 socket 换到进程内重演。要解耦请自行入队到自己的
     * 线程，别在回调里做重活。
     *
     * 与 socket 订阅者共用同一套语义：掩码过滤、prefix 裁剪、丢失信号
     * （OVERFLOW/UNRESOLVED）都会送到这里，所以进程内消费者不会成为唯一
     * 发现不了丢失的人。回调里只允许调用 sfa_srv_stop() 与只读访问者。 */
    void      (*on_event)(void *user, const struct sfa_event *ev);
    void       *on_event_user;
    uint32_t    on_event_mask;      /* 0 = 不回调；通常用 SFA_EV_ALL */
};

struct sfa_srv;                     /* opaque */

/* 建实例：探测能力 → 打开挂载点 → fanotify_init/mark → bind/listen → 设权限。
 * 返回 0 成功，-1 失败（原因已通过日志回调给出，也可用 sfa_srv_error() 取）。
 * 成功时必须用 sfa_srv_close() 释放。 */
int  sfa_srv_open(struct sfa_srv **out, const struct sfa_srv_opts *opts);

/* 跑一轮 poll。timeout_ms 语义同 poll：-1 阻塞，0 立即返回。
 * 返回 0 正常（含被 stop 或超时），-1 出错（错误串见 sfa_srv_error）。 */
int  sfa_srv_poll(struct sfa_srv *s, int timeout_ms);

/* 循环 poll 直到 sfa_srv_stop()。返回 0 正常结束，-1 出错。 */
int  sfa_srv_run(struct sfa_srv *s);

/* 请求停止。**可从信号处理器或其他线程调用**（内部自管道唤醒 poll，
 * 不依赖 poll 被打断）。 */
void sfa_srv_stop(struct sfa_srv *s);

int  sfa_srv_stopped(const struct sfa_srv *s);

/* fanotify 事件 fd，供调用方并入自己的 poll/epoll；配合 sfa_srv_poll(s, 0) 使用。 */
int  sfa_srv_fd(const struct sfa_srv *s);

/* 被监控的挂载点 / 协商出的工作模式（SFA_WF_*）。
 * 走 socket 的客户端从 welcome 拿这两样，进程内订阅者用这两个入口。 */
const char *sfa_srv_mount(const struct sfa_srv *s);
uint32_t    sfa_srv_work_flags(const struct sfa_srv *s);

void sfa_srv_close(struct sfa_srv *s);

/* 最近一次失败的人类可读原因（空串表示无）。调用方自己管理日志时用它取原因。 */
const char *sfa_srv_error(const struct sfa_srv *s);

/* 纯逻辑自检（前缀分量比较、welcome 工作模式位），不碰 fanotify、不需要特权。
 * 返回 0 全过；详情写 stdout/stderr。内嵌方可以自己跑一遍。 */
int  sfa_srv_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* SFA_SERVER_H */
