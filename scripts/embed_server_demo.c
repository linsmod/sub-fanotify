/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* embed_server_demo.c - 内嵌 server 的最小示例（issue #10 的 e2e 用）
 *
 * 用法：embed_server_demo <mount> <socket> [mount2 socket2] [--events]
 * 行为：用 sfa_srv_* 在同一进程里起 watcher（给第二组参数则**同时开两个实例**，
 *       用来验证实例间没有共享状态；--events 则注册进程内回调，事件不经 socket）；
 *       收到 SIGINT/SIGTERM 时在信号处理器里调用 sfa_srv_stop()，主循环退出后
 *       sfa_srv_close() 清理。
 * 退出码：0 = 正常停止；1 = 启动/运行失败。
 *
 * 这正是下游客户的形态：调用方自己就是特权守护进程，watcher 是它的一个组件。
 */
#define _GNU_SOURCE
#include "sfa_server.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

static struct sfa_srv *g_srv[2];   /* 供信号处理器使用（只允许 async-signal-safe 调用） */

static void on_signal(int sig)
{
    (void)sig;
    for (int i = 0; i < 2; i++)
        sfa_srv_stop(g_srv[i]);    /* stop(NULL) 是安全的 no-op */
}

/* 把库的日志原样转出去（演示日志回调；NULL 则库直接写 stderr） */
static void on_log(void *user, const char *msg)
{
    fprintf(stderr, "[demo%s] %s\n", (const char *)user, msg);
}

/* 进程内订阅回调：**只做打印**（真实消费者应入队到自己的线程，别在这里做重活）。 */
static void on_event(void *user, const struct sfa_event *ev)
{
    char names[128];
    printf("EVENT %s %s\n", sfa_event_names(ev->mask, names, sizeof(names)),
           ev->path[0] ? ev->path : "(无路径)");
    fflush(stdout);
    (void)user;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <mount> <socket> [mount2 socket2] [--events]\n", argv[0]);
        return 1;
    }

    /* --events：注册进程内订阅（不经 socket 收事件），用于 e2e 断言 */
    int want_events = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--events")) want_events = 1;

    int two = argc >= 5 && argv[3][0] != '-';
    struct sfa_srv_opts opts = {
        .mount   = argv[1],
        .sock    = argv[2],
        .log     = on_log,
        .log_user = (void *)"",
    };
    if (want_events) {
        opts.on_event      = on_event;
        opts.on_event_mask = SFA_EV_ALL;
    }
    if (sfa_srv_open(&g_srv[0], &opts) < 0) {
        fprintf(stderr, "sfa_srv_open: %s\n", sfa_srv_error(g_srv[0]));
        return 1;
    }

    if (two) {
        struct sfa_srv_opts opts2 = {
            .mount    = argv[3],
            .sock     = argv[4],
            .log      = on_log,
            .log_user = (void *)"2",
        };
        if (sfa_srv_open(&g_srv[1], &opts2) < 0) {
            fprintf(stderr, "sfa_srv_open(2): %s\n", sfa_srv_error(g_srv[1]));
            sfa_srv_close(g_srv[0]);
            return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "[demo] 就绪：socket=%s fanotify fd=%d%s\n",
            argv[2], sfa_srv_fd(g_srv[0]),
            two ? "（第二个实例亦已启动）" : "");

    int rc = 0;
    if (!two) {
        /* 单实例：用 sfa_srv_run() 阻塞跑 */
        if (sfa_srv_run(g_srv[0]) < 0) {
            fprintf(stderr, "[demo] run: %s\n", sfa_srv_error(g_srv[0]));
            rc = 1;
        }
    } else {
        /* 两实例：用 sfa_srv_poll() 自己驱动（演示嵌入事件循环的形态） */
        while (!sfa_srv_stopped(g_srv[0])) {
            if (sfa_srv_poll(g_srv[0], 200) < 0 ||
                sfa_srv_poll(g_srv[1], 0) < 0) {
                rc = 1;
                break;
            }
        }
    }

    if (rc != 0) {
        const char *e1 = sfa_srv_error(g_srv[0]);
        const char *e2 = two ? sfa_srv_error(g_srv[1]) : "";
        fprintf(stderr, "[demo] run: %s%s%s\n",
                e1[0] ? e1 : "", (e1[0] && e2[0]) ? " / " : "", e2);
    }

    sfa_srv_close(g_srv[1]);
    sfa_srv_close(g_srv[0]);
    fprintf(stderr, "[demo] 已优雅退出\n");
    return rc;
}
