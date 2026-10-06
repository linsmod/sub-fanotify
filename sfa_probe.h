/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* sfa_probe.h - fanotify 能力探测（只探测本实现真正用到的部分） */
#ifndef SFA_PROBE_H
#define SFA_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <sys/fanotify.h>
#include "sfa.h"

/* ---- 老 glibc / 老内核头文件可能缺失的常量兜底 ---- */
#ifndef FAN_REPORT_FID
#define FAN_REPORT_FID     0x00000200
#endif
#ifndef FAN_REPORT_DIR_FID
#define FAN_REPORT_DIR_FID 0x00000400
#endif
#ifndef FAN_REPORT_NAME
#define FAN_REPORT_NAME    0x00000800
#endif
#ifndef FAN_REPORT_DFID_NAME
#define FAN_REPORT_DFID_NAME (FAN_REPORT_DIR_FID | FAN_REPORT_NAME)
#endif
#ifndef FAN_UNLIMITED_QUEUE
#define FAN_UNLIMITED_QUEUE 0x00000010
#endif
#ifndef FAN_UNLIMITED_MARKS
#define FAN_UNLIMITED_MARKS 0x00000020
#endif
#ifndef FAN_MARK_MOUNT
#define FAN_MARK_MOUNT      0x00000010
#endif
#ifndef FAN_MARK_FILESYSTEM
#define FAN_MARK_FILESYSTEM 0x00000100
#endif
#ifndef FAN_ATTRIB
#define FAN_ATTRIB      0x00000004
#endif
#ifndef FAN_MOVED_FROM
#define FAN_MOVED_FROM  0x00000040
#endif
#ifndef FAN_MOVED_TO
#define FAN_MOVED_TO    0x00000080
#endif
#ifndef FAN_CREATE
#define FAN_CREATE      0x00000100
#endif
#ifndef FAN_DELETE
#define FAN_DELETE      0x00000200
#endif
/* 5.17+：一条事件同时带旧/新路径，可替代 MOVED_FROM/MOVED_TO 配对 */
#ifndef FAN_REPORT_TARGET_FID
#define FAN_REPORT_TARGET_FID 0x00001000
#endif
#ifndef FAN_RENAME
#define FAN_RENAME      0x10000000
#endif
#ifndef FAN_EVENT_INFO_TYPE_OLD_DFID_NAME
#define FAN_EVENT_INFO_TYPE_OLD_DFID_NAME 10
#endif
#ifndef FAN_EVENT_INFO_TYPE_NEW_DFID_NAME
#define FAN_EVENT_INFO_TYPE_NEW_DFID_NAME 12
#endif
/* 修饰符：不带它，mkdir/rmdir/目录 rename 这类"对象是目录"的事件会被内核过滤 */
#ifndef FAN_ONDIR
#define FAN_ONDIR       0x40000000
#endif

/* 我们关心的事件位个数（与 sfa_probe.c 中的事件表一一对应，含 FAN_RENAME） */
#define SFA_NR_EV_BITS 7

/* 探测结果 */
struct sfa_caps {
    char     kernel[128];

    /* 权限 */
    int      cap_sys_admin;        /* fanotify_mark 需要 */
    int      cap_dac_read_search;  /* open_by_handle_at 需要 */

    /* fanotify_init 能力：1 支持，0 不支持，err 为 errno */
    int      init_notif,      err_notif;
    int      init_dfid_name,  err_dfid_name;
    int      init_uqueue,     err_uqueue;
    int      init_umarks,     err_umarks;
    int      init_target_fid, err_target_fid;  /* FAN_RENAME 的前提：可报新/旧 fid */

    /* fanotify_mark 模式（在目标路径上实测） */
    int      mark_mount,      err_mark_mount;
    int      mark_fs,         err_mark_fs;

    /* 实测可用的事件位（FAN_CREATE/... 的子集）
     * 注意：同一事件位在不同 mark 模式下可用性可能不同，
     *       例如部分内核的 FAN_MARK_MOUNT 不接受目录类事件。 */
    uint64_t ev_mask_mount;
    uint64_t ev_mask_fs;
    uint64_t ev_mask;                    /* 最终选中模式下可用的事件位 */
    int      ev_err[SFA_NR_EV_BITS];     /* 各事件位在选中模式下的 errno，0 表示可用 */

    /* rename 模式：可用时服务端用 SFA_EV_MOVED（一条带旧+新路径），
     * 并自动去掉 MOVED_FROM/MOVED_TO 避免同一操作上报两次 */
    int      rename_ok,       err_rename;

    /* FAN_ONDIR：缺失会导致 mkdir/rmdir/目录 rename 收不到 */
    int      ondir_ok,        err_ondir;

    /* 路径反解：name_to_handle_at + open_by_handle_at + /proc/self/fd/N */
    int      paths_ok,        err_paths;

    /* ---- 协商后的最终配置 ---- */
    unsigned init_flags;   /* 传给 fanotify_init */
    unsigned mark_flags;   /* FAN_MARK_MOUNT 或 FAN_MARK_FILESYSTEM */
    uint64_t mask;         /* 实际下发的事件掩码 */
    int      usable;       /* 1 = 可以正常工作 */
};

/* 对 target 路径做能力探测并协商出最终配置 */
void sfa_probe(const char *target, struct sfa_caps *c);

/* 打印探测报告 */
void sfa_probe_print(const struct sfa_caps *c, FILE *out);

#endif /* SFA_PROBE_H */
