/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* sfa-server.c - CLI 包装：特权 fanotify 代理
 *
 * 服务器逻辑全在 sfa_server.c（库）里，这里只做三件事：解析参数、装信号处理、
 * 调库。库化的动机与权限边界见 sfa_server.h 顶部（issue #10）。
 * 本文件的输出（stdout/stderr 文案、退出码）与库化之前保持一致 ——
 * scripts/ 下的 e2e 脚本依赖其中的关键字（desync、丢失）。
 */
#define _GNU_SOURCE
#include "sfa.h"
#include "sfa_probe.h"
#include "sfa_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* 信号处理器里只允许 async-signal-safe 的调用：sfa_srv_stop() 内部是置标志 +
 * 往自管道 write() 一字节，符合这个约束。 */
static struct sfa_srv *g_srv;

static void on_signal(int sig)
{
    (void)sig;
    sfa_srv_stop(g_srv);
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
            "  -h, --help        打印本用法\n"
            "\n"
            "把代理嵌进自己的进程：见 sfa_server.h（sfa_srv_open/run/stop）\n",
            prog, prog);
}

struct options {
    const char *mount;
    const char *sock;
    const char *group;
    const char *prefix[SFA_SRV_MAX_PREFIXES];
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
            if (o->nprefix >= SFA_SRV_MAX_PREFIXES) {
                fprintf(stderr, "sfa-server: --prefix 最多 %d 个\n", SFA_SRV_MAX_PREFIXES);
                return -1;
            }
            o->prefix[o->nprefix++] = argv[i];
        } else if (!strncmp(a, "--prefix=", 9)) {
            if (o->nprefix >= SFA_SRV_MAX_PREFIXES) {
                fprintf(stderr, "sfa-server: --prefix 最多 %d 个\n", SFA_SRV_MAX_PREFIXES);
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

    if (o.selftest) return sfa_srv_selftest();

    /* 前缀必须是绝对路径：事件路径全部来自 /proc/self/fd 的 readlink，
     * 一定是绝对路径，相对前缀会一条都不匹配而静默收不到事件。
     * （库也会校验一次，这里是 CLI 侧的早失败，报错文案保持原样。） */
    for (int i = 0; i < o.nprefix; i++) {
        if (o.prefix[i][0] != '/') {
            fprintf(stderr, "sfa-server: --prefix 必须是绝对路径: %s\n", o.prefix[i]);
            return 1;
        }
    }

    /* --probe：只打印能力报告，不启动服务 */
    if (o.probe) {
        struct sfa_caps caps;
        sfa_probe(o.mount, &caps);
        sfa_probe_print(&caps, stdout);
        return caps.usable ? 0 : 1;
    }

    struct sfa_srv_opts opts = {
        .mount   = o.mount,
        .sock    = o.sock,
        .group   = o.group,
        .prefix  = o.prefix,
        .nprefix = o.nprefix,
    };
    struct sfa_srv *srv = NULL;

    /* 探测报告、错误原因都由库经日志出口打出（默认 stderr，与库化前一致） */
    if (sfa_srv_open(&srv, &opts) < 0)
        return 1;

    g_srv = srv;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int rc = sfa_srv_run(srv) < 0 ? 1 : 0;

    sfa_srv_close(srv);
    return rc;
}
