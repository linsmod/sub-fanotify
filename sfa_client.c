/* sfa_client.c - 示例客户端：连接并打印事件 */
#define _GNU_SOURCE
#include "sfa.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    const char *sock_path = argc > 1 ? argv[1] : SFA_SOCKET_PATH;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    struct sfa_welcome w;
    int fd = sfa_connect2(sock_path, &w);
    if (fd < 0) { perror("sfa_connect"); return 1; }

    if (sfa_subscribe(fd, SFA_EV_ALL) < 0) {
        perror("sfa_subscribe");
        sfa_close(fd);
        return 1;
    }

    /* 工作模式来自握手：监控范围（MOUNT/FILESYSTEM）决定事件路径可能落在哪，
     * RENAME_PAIR 决定 rename 是否单条，ONDIR 决定目录事件是否可见。 */
    char wf[192];
    fprintf(stderr, "[client] 已订阅 %s\n"
                    "[client] watch=%s mode=%s\n",
            sock_path,
            w.mount[0] ? w.mount : "(未知)",
            sfa_work_flags_str(w.flags, wf, sizeof(wf)));

    while (!g_stop) {
        struct sfa_event ev;
        char names[128];
        ssize_t n = sfa_recv(fd, &ev, 1000);
        if (n < 0) {
            perror("sfa_recv");
            break;
        }
        if (n == 0) continue;   /* 超时 */

        const char *path2 = sfa_event_path2(&ev);
        printf("%-24s %-4s pid=%-6u path=%s%s%s\n",
               sfa_event_names(ev.mask, names, sizeof(names)),
               sfa_event_is_dir(&ev) ? "[dir]" : "", ev.pid,
               ev.path[0] ? sfa_event_path(&ev) : "(overflow)",
               path2 ? "  old=" : "",
               path2 ? path2 : "");
        fflush(stdout);

        /* 真实索引场景：
         *   - 只看主语义：switch (ev.type)
         *   - 完整语义：if (ev.mask & SFA_EV_CLOSE_WRITE) ...
         *   - 是否目录：sfa_event_is_dir(&ev)（SFA_F_ONDIR）
         *   - rename：ev.type == SFA_EV_MOVED 时 path 为新路径，sfa_event_path2() 为旧路径
         * 把 ev 投递到本地索引更新队列（按路径幂等 upsert 即可） */
    }

    sfa_close(fd);
    return 0;
}
