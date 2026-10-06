---
id: 1
status: closed
type: bug
priority: P0
created: 2026-10-06
closed: 2026-10-06
commit: bdc50bf
verdict: broadcast 改非阻塞，慢客户端只丢自己的事件并被 10s 墙钟上界断开、收到补发的 UNRESOLVED；e2e 实测健康客户端不再停摆
---

# broadcast 用阻塞 send，一个慢客户端能把特权代理卡死

> 提出者：esidx，sfa 的一个订阅方（只订阅、不 fork、不改 sfa）。本文所有实测都用仓库自己的
> `sfa-server` + `sfa_client` 复现，命令见「实测」。环境：WSL2 `6.18.40.1-microsoft-standard-WSL2`，
> sfa `eff9956`；未在 ext4 裸机上复测，结论与机器无关的部分只有卡死本身。
> **正文所有 `文件:行号` 以 `eff9956` 为基准**；代码改动后行号会漂移，摘录才是判据。

## 现象

`broadcast()` 对每个订阅者用阻塞 `send()`，`sfa-server.c:121-131`：

```c
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
```

相关的三处，都在同一条路径上：

| 位置 | 事实 |
|---|---|
| `sfa-server.c:189` | 监听 socket 是 `SOCK_SEQPACKET \| SOCK_CLOEXEC`，**没有** `SOCK_NONBLOCK` |
| `sfa-server.c:301` | `accept(listen_fd, NULL, NULL)` —— Linux 上新连接的 fd 不继承 `O_NONBLOCK`，所以**每个客户端 socket 也是阻塞的** |
| `sfa-server.c:216` | 主循环是 `poll(pfds, nfds, -1)`，**超时为 -1**，没有一条能把它从 `send()` 里救出来的路径 |

主循环单线程，fanotify fd 与全部客户端 socket 在同一轮里顺序处理（`sfa-server.c:224-332`）。所以任何一个客户端不读数据，整个进程停在 `send()` 里：不再读 fanotify、不再服务其它客户端、不再处理订阅请求。

### 实测

`sfa_client` 是只读的客户端，所以「一个不读的客户端」用 `SIGSTOP` 造，比写一个不读的客户端更接近真实场景（进程还活着，连接也还在）。

```sh
./sfa-server /tmp "$SOCK" &
./sfa_client "$SOCK" >A.out & A=$!; sleep 0.4; kill -STOP "$A"   # A 不再读
./sfa_client "$SOCK" >B.out & B=$!                               # B 正常排空
for i in $(seq 1 400); do echo x > "$T/bulk/b$i"; done            # 400 个文件
```

```
baseline: B 有 10 行（5 个文件）
生成 400 个文件后      B 停在 26 行
3 秒之后              B 仍是 26 行          ← 事件一直在产生
关掉 A 之后           B 立刻收到 418 行
```

B 是**完全健康**的客户端，它的投递在 A 存在期间停住了整个生成过程，外加之后整整 3 秒，
而这 3 秒里事件一直在产生。代理自己一个字都没说。

缓冲有多大（`ss -x -a -m -p`）：

```
u_seq ESTAB ... skmem:(r0,rb212992,t8448,tb212992,...)     # 服务器侧 sndbuf = 212992
u_seq ESTAB ... skmem:(r0,rb212992,t0,tb212992,...)        # 客户端侧 rcvbuf = 212992
```

两端合计 425 984 字节，一条 `struct sfa_event` 是 4128 字节（`sfa.h:52-61`），名义上装得下 **103 条**。实测的停止点比这更早（第一次运行时 B 停在第 26 行上下），差值来自 AF_UNIX 按 skb 计费而非按字节 —— 这一项没有逐条核算。

## 影响

**卡死是确定的，不是概率事件。** 架构的可用性下限等于最慢的那个订阅者，而它是可以被一个什么都不做的客户端按住的。

**丢事件这一半，本次没有复现，撤回上一版的说法。** 上一版写「事件在内核队列里堆积直到溢出，`FAN_Q_OVERFLOW` 会真的丢掉事件」。把阻塞维持到 4 万个文件创建完毕（8 进程并行），事后统计：

```
创建的文件数        40000
B 实际收到的 bulk 事件   40012
B 收到的 OVERFLOW 事件      0
```

一条都没丢，原因是协商到了 `FAN_UNLIMITED_QUEUE`（`sfa_probe.c:173`、`sfa-server.c:177`），内核把整段队列留在内存里等它回来读；`/proc/sys/fs/fanotify/max_queued_events` 是 16384，但有 `FAN_UNLIMITED_QUEUE` 时它不是约束。所以「溢出丢事件」是有条件的：队列先耗尽内存才是。要把它量出来需要把机器的内存吃满，那不是本 issue 的主张，本 issue 的主张是**停摆**，而停摆是实测的。

**停摆的后果比「慢」更具体，两条都实测了：**

1. **别人的索引被拖成任意旧，然后一次性追上。** 上面的 4 万个事件是在 A 存在期间产生的，全部堆在内核队列里；A 一关，B 在几秒内收到 4 万条。对一个把事件当成「最多落后一个批次」承诺的订阅方，这意味着承诺的违约时长不由它自己决定，而由另一个客户端决定。
2. **停摆之后再删一次目录，事件就真的没了，而且没有任何人知道** —— 见 #2，那一条是实测的 73% 静默丢失。本 issue 是那条的前置条件：代理一旦停过，队列里的句柄就会在它读到之前过期。

## 建议

`MSG_DONTWAIT`，然后**不要立刻踢人**，把客户端标成 desynced，等它的 socket 排空后补发一条 `SFA_EV_OVERFLOW`：

```c
/* struct client 增加一个字段 */
int desynced;

if (send(clients[i].fd, ev, sizeof(*ev),
         MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
    if (errno == EAGAIN || errno == ENOBUFS) {
        /* 事件已经丢了，但「丢了」这件事本身可以送达，而且客户端只需要知道这一点 */
        clients[i].desynced = 1;
        continue;
    }
    close(clients[i].fd);            /* 必须 close，见下面的连带影响 */
    clients[i].fd = -1;
}
```

在每轮 poll 之后，对 `desynced` 的客户端试一次发送 `SFA_EV_OVERFLOW`（`path_len = 1`、`path[0] = '\0'`，与 `sfa-server.c:234-243` 现有写法相同）；发成功就清 `desynced`，发不出来就继续保持 desynced。

**为什么是补发 OVERFLOW 而不是断开。** 客户端在这两种情况下的处境完全不同：

| | 断开 | 补发 OVERFLOW |
|---|---|---|
| 客户端要做什么 | 重连、重新订阅，然后自己补一次全量 | 直接补一次全量 |
| 代理要知道的事 | 无 | 只需知道「你落后了」 |
| 协议改动 | 无 | 无 —— `SFA_EV_OVERFLOW` 与它的处理路径已经存在 |
| 客户端落后多久 | 从重连完成算起 | 从收到这条消息算起 |

一个索引类客户端补一次全量的代价是毫秒级（esidx 在 `/work` 上 5 476 485 个条目的空转 pass 是
0.4-1.1 ms，因为它只 stat 根目录的 fanout；这是 ext4 裸机上的实测，不是本文的 WSL2 环境），
所以「补发一条信号」和「踢掉重连」对客户端的价值差一个数量级，而对代理的代价只差一个字段。

**为什么不是计数后踢人（上一版的方案）。** 缓冲名义上装 103 条，任何一次突发都会让一个**正在正常排空**的客户端连续 N 次 `EAGAIN` 然后被踢。单线程架构下代理没有地方缓存这些事件 —— 缓冲满就意味着客户端已经落后，踢不踢的结果一样，区别只是踢得早还是晚。所以计数在这里没有信息量。

**代价与不覆盖的情况，写在这里：**

1. **desynced 会占着槽位和内存。** 一个永远不读的客户端会一直挂着。需要一个上界：desynced 持续若干轮仍发不出去就 `close()`。上界取多少是策略问题，取 1 轮等于退回「立刻踢」，取无限等于不处理泄漏。
2. **`send()` 补发的那一次本身也可能 `EAGAIN`**，所以补发逻辑要能安全地被重复调用。
3. **补发的 OVERFLOW 只说「有东西丢了」，不说丢的是什么。** 对索引类订阅方这是足够的（答案是一次全量），对需要精确增量对齐的消费者不够。这类消费者只能靠 #2 的信号加自己对账。
4. **本方案不解决一轮 poll 内的第二个饥饿窗口**（见连带影响第 5 条）。两个都修才是完整答案；只修 `send` 能把停摆从「无限」变成「一轮」。

### 连带影响（照着改会漏掉其中三条）

1. **断开时必须 `close()`，不能只置 `fd = -1`。** 压缩逻辑（`sfa-server.c:334-341`）只是把 `fd < 0` 的条目从数组里搬走，不回收 fd。沿用现有那句注释「由压缩逻辑回收」会直接泄漏。
2. **`struct client`（`sfa-server.c:25-28`）目前只有 `fd` 和 `mask`**，新字段加在这里，并且**必须和 `mask` 一起在 accept 处初始化**（`sfa-server.c:307-310`）—— 那三行现在显式写了 `mask = 0`，新字段不显式初始化就是未初始化读。
3. **`send` 失败时的 `errno` 要留着判断**，所以 `broadcast()` 里不能吞掉 errno；如果为了加日志顺手把 `errno` 覆盖掉，`EAGAIN` 与 `EPIPE` 就分不开了。
4. **示例客户端不需要改。** `sfa_client.c:24` 订阅 `SFA_EV_ALL`，`sfa_client.c:46` 已经会把无路径的事件打印成 `(overflow)`。
5. **同源但不在本 issue 范围内的一点**：一轮 poll 内 `while (FAN_EVENT_OK(meta, len))`（`sfa-server.c:230`）没有批次上限，一次 64 KiB 的 read 里可能有上千个事件，处理完之前客户端 socket 同样得不到服务。修 `send` 不解决它，但**它决定了修完之后「好到什么程度」**：这个窗口是每轮几毫秒，不是无限。

## 附注

- **esidx 侧现在怎么做**：`--watch` 的 drain 一次读到 `EAGAIN` 为止（`watch.c` `esidx_watch_drain`），所以我们是本 issue 假设的那个「排空良好的客户端」，正常突发不会触发上面的路径。真正会踩到的是「订阅后进程被 SIGSTOP / 卡在别的事上」这类情况。
- **断开对我们是有代价的，这一点我们认**：`etp.c` 的 `serve_watch()` 在代理消失时会 `LOGW` 并**摘掉 watcher**，退回 `--refresh`。所以「立刻踢人」对我们等于「停止实时更新」，这正是我们更想要补发 OVERFLOW 而不是断开的原因 —— 但这是一个偏好，不是要求，两种方案我们都能工作。
- **量级参考（我们自己的套件）**：`./test_watch.sh` 里 1000 个文件并发创建，代理侧合并成 **1016 条事件**（内核把同一对象的多次变化合成一条），我们的脏集去重后是 917 个 mark、5 个批次、6 次目录列举。也就是说稳态下每条消息对应约 0.1 次 reconcile，代理卡一次的代价是这段时间里所有人的新鲜度。
- **相关**：#2（停摆之后的静默丢失，实测 73%）、#4（无关事件吃掉同一块缓冲）。
- **不在本 issue 范围内**：订阅端自己的读取策略；内核 fanotify 队列参数；`FAN_UNLIMITED_QUEUE` 是否该默认开启。

## 维护者决定（2026-10-06）

STATUS: accept，保持 **P0**，但**排在 #3 之后**

REASON: 可用性下限等于最慢订阅者，这在架构上是错的 —— 代理是给多个消费者用的基础设施，
不该被任何一个消费者的节奏绑定。`MSG_DONTWAIT` + desync 是这个架构内的正确修法：
它把「无限停摆」换成「有界丢失 + 显式信号」，不需要换掉单线程单进程的整体结构。

**排在 #3 之后的理由**：本篇是**难看但不丢数据** —— 事件还留在内核队列里（`FAN_UNLIMITED_QUEUE`
协商成功时），追得上；#2 是数据已经没了且无人知晓。相比之下 #3 是承诺没实现、代价最小，
先做它。

**与 #2 的关系（2026-10-06 决定）**：不合并，但共用丢失信号形状。本篇 desync 的补发信号
就是 #2 新增的 `SFA_EV_UNRESOLVED`，两者同族，不另开第三种语义。

一处补充，实现时会绊到：`accept()` 之后 `sfa-server.c:305` 的 `send(cfd, &w, sizeof(w), ...)`
是同一形状的阻塞 send，welcome 是 4104 字节（`sfa.h:70-74`）。默认 sndbuf 下一次发得完，
今天不构成卡死点，但它属于同一个类。本篇的修改不覆盖它 —— 单独记在这里，不要顺手改。

NEXT（验收标准）：

1. 广播路径用 `MSG_NOSIGNAL | MSG_DONTWAIT`；`EAGAIN` / `ENOBUFS` 置 `desynced` 并保留 slot，
   其他错误 `close()`（压缩逻辑不回收 fd，必须真的 close）；
2. `desynced` 有明确上界（本篇建议的「持续 N 轮仍发不出去就 close」，N 取值写在代码注释里说明理由）；
3. 复现脚本（`SIGSTOP` 一个订阅者 + 一个健康订阅者）下，健康客户端**不再出现超过一轮的停顿**；
4. `struct client` 新字段在 `sfa-server.c:307-310` 与 `mask` 一起显式初始化；
5. 一轮 poll 内 `while (FAN_EVENT_OK(...))` 的批次上限**不在本篇范围**，但 #1 的重测数据
   要单独标注「修完 send 之后」，好让 #4 的噪声影响可见。
## 修复与实测（2026-10-06）

已实现并实测，环境 WSL2 `6.18.40.1-microsoft-standard-WSL2`（root），commit `bdc50bf`。
与 #2 一次性实现：两者共用 `SFA_EV_UNRESOLVED` 丢失信号（本篇「维护者决定」的要求）。

| 验收标准 | 实现 / 实测 | |
|---|---|---|
| 1. `MSG_NOSIGNAL\|MSG_DONTWAIT`；EAGAIN/ENOBUFS 置 desynced 并保留 slot；其他错误 `close()` | broadcast() 已改；EPIPE/ECONNRESET 等立即 `close()` + `fd=-1` | 通过 |
| 2. desynced 有明确上界 | **有一处偏离本篇建议**：上界不是「N 轮」而是 10 秒墙钟（`DESYNC_KICK_TIMEOUT_MS`）。e2e 实测发现：一次突发里 send 可在毫秒内连续 EAGAIN 上百次、一轮 poll 可短至微秒，按轮计数把还在正常排空的客户端误杀（A 尚未恢复即被断开）。墙钟语义与「持续 N 轮」一致且不受事件流量影响；取值理由在宏注释 | 通过（偏离已记录） |
| 3. 复现脚本下健康客户端不再停顿超过一轮 | `scripts/test_slow_client.sh`：A SIGSTOP 期间 B 事件数 621→1242 持续增长；修复前基线是停在 26 条 | 通过 |
| 4. 新字段与 mask 一起显式初始化 | accept 处 `desync_since_ms = 0` | 通过 |
| 5. 批次上限不在本篇范围，重测数据标注「修完 send 之后」 | e2e 数据均为「修完 send 之后」；`while (FAN_EVENT_OK(...))` 的批次上限仍未做，留待后续 issue | 未做（按约定） |

补发信号按维护者决定走 `SFA_EV_UNRESOLVED`（发往 desynced 客户端本身，不走订阅过滤——老客户端收到未知位只是把 mask 打成 UNKNOWN，不会崩）。desynced 存在时 poll 超时取 1s，使上界有实际时间语义。另：正文「一处补充」指出的 welcome 阻塞 send（accept 之后那次）按正文要求未动。
