---
id: 10
status: accepted
type: feature
priority: P1
created: 2026-10-06
closed:
commit:
verdict:
---

# 客户要求「库文件包含 server 能力」：server 逻辑全在 `main()` 里，外部无法复用

> 提出者：维护者本人，转述下游客户诉求（客户明确要的是**同进程内嵌**，
> 不是「包里带个可执行文件」——后者 `make install-bin` 与 `-bin` 包已覆盖）。

## 现象

`sfa-server.c` 的服务器逻辑（能力探测 → `fanotify_init`/`fanotify_mark` →
bind/listen → poll 主循环 → broadcast/desync/丢失统计）**全部写在 `main()` 里**，
状态放在文件作用域：

| 位置 | 状态 | 内嵌时会出什么问题 |
|---|---|---|
| `g_stop`（`sig_atomic_t`） | 停止标志，只由 `signal()` 处理器置位 | 调用方没法停；多实例共享同一个标志 |
| `g_prefix[]` / `g_nprefix` | `--prefix` 过滤表 | 多实例互相污染；实例与调用方数组生命周期不一致 |
| `main()` 局部 `clients[]` / `sock_path` 等 | 连接表、路径 | 无法从外部取得 fd / 跑单轮 |

于是「把 `sfa-server.c` 编进 `libsfa.a`」得到的只是几个无法调用的符号——
**能链接，没法用**。客户要的是在自己进程里起 watcher：共享事件循环、
省掉 IPC 一跳、少一个部署单元。

## 建议

### 1. 抽 `sfa_srv_*` API（新头文件 `sfa_server.h`，opaque 实例）

```c
struct sfa_srv_opts {
    const char *mount;               /* 必填：挂载点/目录 */
    const char *sock;                /* NULL = SFA_SOCKET_PATH */
    const char *group;               /* 可空，同 --group */
    const char *const *prefix;       /* 可空，同 --prefix；open 时拷贝进实例 */
    int         nprefix;
    void      (*log)(void *user, const char *msg);  /* NULL = stderr */
    void       *log_user;
};

struct sfa_srv;                      /* opaque */

int  sfa_srv_open(struct sfa_srv **out, const struct sfa_srv_opts *o);
int  sfa_srv_fd(const struct sfa_srv *s);        /* 可嵌进调用方自己的 poll/epoll */
int  sfa_srv_poll(struct sfa_srv *s, int timeout_ms);  /* 跑一轮；-1 = 阻塞 */
int  sfa_srv_run(struct sfa_srv *s);             /* 循环到 stop，等价现 main 的循环 */
void sfa_srv_stop(struct sfa_srv *s);            /* 可从信号处理器/其他线程调用 */
void sfa_srv_close(struct sfa_srv *s);
const char *sfa_srv_error(const struct sfa_srv *s);   /* 人类可读，替代 exit() */
int  sfa_srv_selftest(void);                     /* 纯逻辑自检，供内嵌方自己也跑一遍 */
```

要点（都是这次会绊到的）：

1. **状态全部收进 `sfa_srv`**：`stop`、prefix 表（拷贝）、客户端表、fd、错误串。
   目标：同进程可以开多个实例而不互相串味。
2. **错误用返回值**：库调用方要决定怎么报错、要不要继续，所以不 `exit(1)`；
   `sfa_srv_error()` 给人类可读原因。日志默认仍走 stderr，可换成回调。
3. **`sfa_srv_stop()` 必须 async-signal-safe**：内部自管道（`pipe2(O_NONBLOCK)`）写入
   一字节唤醒 `poll`，这样信号处理器里调用也安全，且不必依赖 `poll` 被打断（`EINTR`）。
4. **`sfa_srv_fd()` + `sfa_srv_poll(timeout)` 允许嵌入调用方事件循环**：不想用
   `sfa_srv_run()` 的调用方可以只在 fanotify fd 就绪时跑一轮。
5. **`main()` 退化为薄 CLI**：`parse_args()` 保留，随后 `open → run → close`。
   **CLI 的输出、stderr 文案、退出码、`--probe` / `--selftest` 行为必须与现在一致** ——
   仓库的 e2e 脚本正靠这些字符串做断言（`scripts/test_slow_client.sh` 断言 stderr 含
   `desync`，`test_unresolved.sh` 断言含 `丢失`）。不得出现第二份会漂移的服务器实现。

### 2. 包切分：三个归档，不把 server 硬塞进 `libsfa.a`

| 归档 | 内容 | 依赖 |
|---|---|---|
| `libsfa.a` | 客户端 SDK（不变） | 仅 libc，**不需要任何特权** |
| `libsfa-server.a` | `sfa_server.o` + `sfa_probe.o` | fanotify（需 `CAP_SYS_ADMIN`） |
| `libsfa-all.a` | 两者合并，客户「一个库搞定」的入口 | 同上 |

理由：只订阅的消费者（esidx）不该因为「有人要 server」而背上 fanotify 依赖；
但客户明说想要一个库，所以提供合并归档而不是替所有消费者做决定。

> **注意实现陷阱**：`sfa-server.c` 里的 `main` 不能进 `libsfa-server.a`——
> 静态库按符号拉取目标文件，调用方一旦引用 `sfa_srv_open`（同目标文件内），
> 就会把 `main` 一起拉进来，与调用方自己的 `main` 冲突。因此库实现放
> `sfa_server.c`，`sfa-server.c` 只留 CLI。

### 3. 必须写进接口文档的架构约束（**权限边界**）

`sfa_srv_*` 让 watcher 与调用方同进程，于是：

- 调用方进程**必须持有 `CAP_SYS_ADMIN`**（fanotify 的前提），
  而 README 的核心卖点正是「特权 watcher + 无特权索引器分离」；
- **崩溃域合并**：server 的断言/段错误会带走调用方进程；子进程模型下
  代理挂了只是代理挂了。

因此本 API 的适用前提是「**调用方本来就是独立的特权守护进程**」。
文档（`sfa_server.h` 顶部注释 + README）必须把这两条写明，避免下游把它当成
「索引器顺便起个 watcher」的捷径 —— 那等于把本项目要解决的问题重新引回来。

## 验收标准

1. `sfa_server.h` / `sfa_server.c` 落地上述 API；实例内无跨实例共享的可变状态
   （同进程可开两个实例，各自 socket 互不影响）；
2. `sfa_srv_stop()` 在信号处理器里调用可让 `sfa_srv_run()` 正常返回
   （不依赖 `EINTR`，自管道唤醒）；
3. `sfa_srv_fd()` + `sfa_srv_poll()` 可用：调用方自己 poll 该 fd 也能驱动事件投递；
4. CLI（`sfa-server`）行为不变：`--probe` / `--selftest` / `--group` / `--prefix` /
   非法参数退出码、stderr 文案（含 `desync`、`丢失`）与现在一致，现有 e2e 全过；
5. `libsfa.a` / `libsfa-server.a` / `libsfa-all.a` 三个归档产出，且
   **`libsfa.a` 不含 `main`、不含 fanotify 符号**；
6. `make install` 装上三个归档 + 三个头文件（`sfa.h` / `sfa_probe.h` / `sfa_server.h`）；
7. 新增 e2e：一个内嵌 server 的小程序（用库 API 起服务、信号停）与 `sfa_client`
   互操作，能收到事件并优雅退出；
8. README 增加「Embedding the server」小节，含权限边界与崩溃域两条约束。
