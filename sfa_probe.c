/* sfa_probe.c - fanotify 能力探测（只探测本实现真正用到的部分） */
#define _GNU_SOURCE
#include "sfa_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/utsname.h>

/* 探测 mark 模式时用的基线掩码：这两个事件自 fanotify 诞生就有，
 * 用它来把「mark 模式不支持」和「某个事件位不支持」两种 EINVAL 区分开。 */
#define PROBE_BASE_MASK (FAN_ACCESS | FAN_MODIFY)

#define CAP_DAC_READ_SEARCH  2
#define CAP_SYS_ADMIN       21

static uint64_t read_cap_eff(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    char line[256];
    uint64_t v = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            v = strtoull(line + 7, NULL, 16);
            break;
        }
    }
    fclose(f);
    return v;
}

static int has_cap(uint64_t cap_eff, int cap)
{
    return (cap_eff >> cap) & 1u;
}

/* 试一次 fanotify_init：0 成功，<0 返回 -errno */
static int try_init(unsigned extra, int *err)
{
    int fd = fanotify_init(FAN_CLASS_NOTIF | extra,
                           O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    if (err) *err = errno;
    return -errno;
}

/* 试一次 fanotify_mark：0 成功，<0 返回 -errno */
static int try_mark(unsigned init_extra, unsigned mark_flag,
                    uint64_t mask, const char *path, int *err)
{
    int fd = fanotify_init(FAN_CLASS_NOTIF | init_extra,
                           O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = errno;
        return -errno;
    }
    int r = fanotify_mark(fd, FAN_MARK_ADD | mark_flag, mask, AT_FDCWD, path);
    if (r < 0 && err) *err = errno;
    close(fd);
    return r < 0 ? -errno : 0;
}

/* 路径反解能力：能否用目标目录的句柄反查回绝对路径 */
static int probe_paths(const char *target, int *err)
{
    int dfd = open(target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        if (err) *err = errno;
        return -errno;
    }

    struct file_handle *fh = calloc(1, sizeof(*fh) + 128);
    if (!fh) {
        close(dfd);
        if (err) *err = ENOMEM;
        return -ENOMEM;
    }
    fh->handle_bytes = 128;

    int mount_id = -1;
    int r = name_to_handle_at(dfd, "", fh, &mount_id, AT_EMPTY_PATH);
    if (r < 0 && errno == EOVERFLOW) {
        struct file_handle *nfh = realloc(fh, sizeof(*fh) + fh->handle_bytes);
        if (nfh) {
            fh = nfh;
            r = name_to_handle_at(dfd, "", fh, &mount_id, AT_EMPTY_PATH);
        }
    }
    if (r < 0) {
        if (err) *err = errno;
        free(fh);
        close(dfd);
        return -errno;
    }

    int hfd = open_by_handle_at(dfd, fh, O_RDONLY | O_CLOEXEC);
    free(fh);
    if (hfd < 0) {
        if (err) *err = errno;
        close(dfd);
        return -errno;
    }

    char link[64], buf[SFA_MAX_PATH];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", hfd);
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    close(hfd);
    close(dfd);
    if (n <= 0) {
        if (err) *err = errno;
        return -errno;
    }
    buf[n] = '\0';
    return buf[0] == '/' ? 0 : -ENOTSUP;
}

/* 我们关心的 6 个事件位，顺序与 SFA_NR_EV_BITS 一致 */
static const struct { uint64_t bit; const char *name; } EV_BITS[] = {
    { FAN_CREATE,      "FAN_CREATE"      },
    { FAN_DELETE,      "FAN_DELETE"      },
    { FAN_MOVED_FROM,  "FAN_MOVED_FROM"  },
    { FAN_MOVED_TO,    "FAN_MOVED_TO"    },
    { FAN_CLOSE_WRITE, "FAN_CLOSE_WRITE" },
    { FAN_ATTRIB,      "FAN_ATTRIB"      },
    { FAN_RENAME,      "FAN_RENAME"      },
};

/* 逐个事件位实测：某些内核在 FAN_MARK_MOUNT 上不接受目录类事件，
 * 因此必须按 mark 模式分别试，而不是一次性下发全量掩码。 */
static uint64_t probe_event_bits(unsigned init_extra, unsigned mark_flag,
                                 const char *target, int *errs)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(EV_BITS) / sizeof(EV_BITS[0]); i++) {
        int err = 0;
        if (try_mark(init_extra, mark_flag, EV_BITS[i].bit, target, &err) == 0) {
            mask |= EV_BITS[i].bit;
            err = 0;
        }
        if (errs) errs[i] = err;
    }
    return mask;
}

static int bit_count(uint64_t v)
{
    return __builtin_popcountll(v);
}

void sfa_probe(const char *target, struct sfa_caps *c)
{
    memset(c, 0, sizeof(*c));

    struct utsname u;
    if (uname(&u) == 0)
        snprintf(c->kernel, sizeof(c->kernel), "%s", u.release);

    uint64_t cap_eff = read_cap_eff();
    c->cap_sys_admin       = has_cap(cap_eff, CAP_SYS_ADMIN);
    c->cap_dac_read_search = has_cap(cap_eff, CAP_DAC_READ_SEARCH);

    /* 1. fanotify_init 相关 */
    c->init_notif     = try_init(0, &c->err_notif) == 0;
    c->init_dfid_name = try_init(FAN_REPORT_DFID_NAME, &c->err_dfid_name) == 0;
    c->init_uqueue    = try_init(FAN_UNLIMITED_QUEUE, &c->err_uqueue) == 0;
    c->init_umarks    = try_init(FAN_UNLIMITED_MARKS, &c->err_umarks) == 0;
    c->init_target_fid = try_init(FAN_REPORT_DFID_NAME | FAN_REPORT_FID |
                                  FAN_REPORT_TARGET_FID, &c->err_target_fid) == 0;

    /* 2. 协商 init 标志：DFID_NAME 是路径解析的前提 */
    unsigned init_extra = 0;
    if (c->init_dfid_name) {
        init_extra |= FAN_REPORT_DFID_NAME;
        if (c->init_uqueue) init_extra |= FAN_UNLIMITED_QUEUE;
        if (c->init_umarks) init_extra |= FAN_UNLIMITED_MARKS;
    } else {
        if (c->init_uqueue) init_extra |= FAN_UNLIMITED_QUEUE;
        if (c->init_umarks) init_extra |= FAN_UNLIMITED_MARKS;
    }

    /* 3. mark 模式：优先 MOUNT，退回 FILESYSTEM */
    c->mark_mount = try_mark(init_extra, FAN_MARK_MOUNT,
                             PROBE_BASE_MASK, target, &c->err_mark_mount) == 0;
    c->mark_fs    = try_mark(init_extra, FAN_MARK_FILESYSTEM,
                             PROBE_BASE_MASK, target, &c->err_mark_fs) == 0;

    /* 4. 逐个事件位实测（内核 5.1 之前没有 CREATE/DELETE/MOVED_*；
     *    且同一位在 MOUNT / FILESYSTEM 两种模式下可用性可能不同） */
    int errs_mount[SFA_NR_EV_BITS] = {0};
    int errs_fs[SFA_NR_EV_BITS]    = {0};
    if (c->mark_mount)
        c->ev_mask_mount = probe_event_bits(init_extra, FAN_MARK_MOUNT,
                                            target, errs_mount);
    if (c->mark_fs)
        c->ev_mask_fs = probe_event_bits(init_extra, FAN_MARK_FILESYSTEM,
                                         target, errs_fs);

    /* 5. 选覆盖事件位最多的模式；同分优先 MOUNT（语义更贴近挂载点） */
    unsigned mark_flags = 0;
    uint64_t mask = 0;
    if (c->mark_mount && bit_count(c->ev_mask_mount) >= bit_count(c->ev_mask_fs)) {
        mark_flags = FAN_MARK_MOUNT;
        mask       = c->ev_mask_mount;
        memcpy(c->ev_err, errs_mount, sizeof(errs_mount));
    } else if (c->mark_fs) {
        mark_flags = FAN_MARK_FILESYSTEM;
        mask       = c->ev_mask_fs;
        memcpy(c->ev_err, errs_fs, sizeof(errs_fs));
    } else if (c->mark_mount) {
        memcpy(c->ev_err, errs_mount, sizeof(errs_mount));
    }

    /* 6. rename：内核 5.17+ 可用一条 FAN_RENAME 事件同时给出旧路径和新路径，
     *    用它替代 MOVED_FROM/MOVED_TO（后者 fanotify 不提供配对 cookie）。 */
    if (c->init_dfid_name && c->init_target_fid && mark_flags) {
        unsigned init_ren = init_extra | FAN_REPORT_FID | FAN_REPORT_TARGET_FID;
        if (try_mark(init_ren, mark_flags, FAN_RENAME, target, &c->err_rename) == 0) {
            c->rename_ok = 1;
            init_extra   = init_ren;
            mask = (mask & ~(uint64_t)(FAN_MOVED_FROM | FAN_MOVED_TO)) | FAN_RENAME;
        }
    }

    /* 7. FAN_ONDIR：修饰符，不是事件位。缺了它 mkdir/rmdir/目录 rename 全收不到。 */
    if (mark_flags &&
        try_mark(init_extra, mark_flags, FAN_ONDIR, target, &c->err_ondir) == 0) {
        c->ondir_ok = 1;
        mask |= FAN_ONDIR;
    }

    /* 8. 路径反解 */
    c->paths_ok = probe_paths(target, &c->err_paths) == 0;

    /* 9. 最终结论 */
    c->init_flags = (unsigned)(FAN_CLASS_NOTIF | init_extra);
    c->mark_flags = mark_flags;
    c->mask       = mask;
    c->ev_mask    = mask;
    c->usable     = c->init_notif && c->init_dfid_name &&
                    mark_flags != 0 && mask != 0;
}

/* ---- 打印 ---- */
static void row(FILE *out, const char *name, int ok, int err)
{
    if (ok) fprintf(out, "  %-22s : yes\n", name);
    else    fprintf(out, "  %-22s : no   (%s)\n", name, strerror(err ? err : ENOTSUP));
}

static void append(char *dst, size_t cap, const char *s)
{
    if (dst[0]) strncat(dst, "|", cap - strlen(dst) - 1);
    strncat(dst, s, cap - strlen(dst) - 1);
}

static void init_flags_str(unsigned f, char *out, size_t cap)
{
    out[0] = '\0';
    append(out, cap, "FAN_CLASS_NOTIF");
    if (f & FAN_REPORT_DFID_NAME)  append(out, cap, "FAN_REPORT_DFID_NAME");
    if (f & FAN_REPORT_FID)        append(out, cap, "FAN_REPORT_FID");
    if (f & FAN_REPORT_TARGET_FID) append(out, cap, "FAN_REPORT_TARGET_FID");
    if (f & FAN_UNLIMITED_QUEUE)   append(out, cap, "FAN_UNLIMITED_QUEUE");
    if (f & FAN_UNLIMITED_MARKS)   append(out, cap, "FAN_UNLIMITED_MARKS");
}

static void mask_str(uint64_t m, char *out, size_t cap)
{
    out[0] = '\0';
    if (m & FAN_CREATE)      append(out, cap, "FAN_CREATE");
    if (m & FAN_DELETE)      append(out, cap, "FAN_DELETE");
    if (m & FAN_MOVED_FROM)  append(out, cap, "FAN_MOVED_FROM");
    if (m & FAN_MOVED_TO)    append(out, cap, "FAN_MOVED_TO");
    if (m & FAN_CLOSE_WRITE) append(out, cap, "FAN_CLOSE_WRITE");
    if (m & FAN_ATTRIB)      append(out, cap, "FAN_ATTRIB");
    if (m & FAN_RENAME)      append(out, cap, "FAN_RENAME");
    if (m & FAN_ONDIR)       append(out, cap, "FAN_ONDIR");
    if (!out[0]) append(out, cap, "(none)");
}

void sfa_probe_print(const struct sfa_caps *c, FILE *out)
{
    char buf[512];

    fprintf(out, "== sfa fanotify 能力探测 ==\n");
    fprintf(out, "  %-22s : %s\n", "kernel", c->kernel);
    row(out, "CAP_SYS_ADMIN",       c->cap_sys_admin,       0);
    row(out, "CAP_DAC_READ_SEARCH", c->cap_dac_read_search, 0);
    row(out, "fanotify_init(NOTIF)", c->init_notif,     c->err_notif);
    row(out, "FAN_REPORT_DFID_NAME", c->init_dfid_name, c->err_dfid_name);
    row(out, "FAN_UNLIMITED_QUEUE",  c->init_uqueue,    c->err_uqueue);
    row(out, "FAN_UNLIMITED_MARKS",  c->init_umarks,    c->err_umarks);
    row(out, "FAN_MARK_MOUNT",       c->mark_mount,     c->err_mark_mount);
    row(out, "FAN_MARK_FILESYSTEM",  c->mark_fs,        c->err_mark_fs);
    row(out, "FAN_REPORT_TARGET_FID", c->init_target_fid, c->err_target_fid);
    row(out, "FAN_RENAME",           c->rename_ok,      c->err_rename);
    row(out, "FAN_ONDIR",            c->ondir_ok,       c->err_ondir);
    row(out, "open_by_handle_at",    c->paths_ok,       c->err_paths);

    fprintf(out, "  %-22s : MOUNT %d/%zu, FILESYSTEM %d/%zu\n", "事件位覆盖率",
            bit_count(c->ev_mask_mount), sizeof(EV_BITS) / sizeof(EV_BITS[0]),
            bit_count(c->ev_mask_fs),    sizeof(EV_BITS) / sizeof(EV_BITS[0]));
    uint64_t mode_cap = c->mark_flags == FAN_MARK_MOUNT ? c->ev_mask_mount
                                                        : c->ev_mask_fs;
    for (size_t i = 0; i < sizeof(EV_BITS) / sizeof(EV_BITS[0]); i++) {
        if (!c->mark_flags) {
            fprintf(out, "    %-20s : -      (mark 不可用，未探测)\n", EV_BITS[i].name);
        } else if (c->ev_mask & EV_BITS[i].bit) {
            fprintf(out, "    %-20s : yes\n", EV_BITS[i].name);
        } else if (mode_cap & EV_BITS[i].bit) {
            fprintf(out, "    %-20s : yes    (未启用，已由 FAN_RENAME 取代)\n",
                    EV_BITS[i].name);
        } else {
            snprintf(buf, sizeof(buf), "no   (%s)",
                     strerror(c->ev_err[i] ? c->ev_err[i] : ENOTSUP));
            fprintf(out, "    %-20s : %s\n", EV_BITS[i].name, buf);
        }
    }

    fprintf(out, "-- 协商结果 --\n");
    init_flags_str(c->init_flags, buf, sizeof(buf));
    fprintf(out, "  %-22s : %s\n", "fanotify_init flags", buf);
    fprintf(out, "  %-22s : %s\n", "mark 模式",
            c->mark_flags == FAN_MARK_MOUNT      ? "FAN_MARK_MOUNT" :
            c->mark_flags == FAN_MARK_FILESYSTEM ? "FAN_MARK_FILESYSTEM" : "(不可用)");
    mask_str(c->mask, buf, sizeof(buf));
    fprintf(out, "  %-22s : %s\n", "事件掩码", buf);
    fprintf(out, "  %-22s : %s\n", "rename 语义",
            c->rename_ok ? "SFA_EV_MOVED（单条事件带旧+新路径）"
                         : "MOVED_FROM/MOVED_TO 两条（fanotify 无 cookie，无法配对）");
    fprintf(out, "  %-22s : %s\n", "可用性",
            c->usable ? "OK" : "不满足最低要求（需 FAN_REPORT_DFID_NAME + 一种 mark 模式 + 至少一个事件位）");
    if (c->usable && !c->paths_ok)
        fprintf(out, "  注意：open_by_handle_at 不可用，事件路径无法反解（将丢弃无路径事件）\n");
    fflush(out);
}
