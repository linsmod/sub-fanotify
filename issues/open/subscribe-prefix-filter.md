---
id: 4
status: accepted
type: feature
priority: P2
created: 2026-10-06
closed:
commit:
verdict:
---

# 订阅没有前缀过滤：整块文件系统的事件都被推到客户端

> 提出者：esidx，sfa 的一个订阅方（只订阅、不 fork、不改 sfa）。本篇**不阻塞**我们：客户端自己
> 已经在过滤（见「esidx 侧现在怎么做」），所以这是一条带宽与噪声的诉求，不是正确性诉求。

## 现象

`FAN_MARK_FILESYSTEM` 覆盖的是**整个文件系统**，服务端把它广播给所有订阅者，代码里没有任何
按路径的裁剪。

标记模式的协商在 `sfa_probe.c:189-193`（先试 `FAN_MARK_MOUNT`）与 `sfa_probe.c:206-219`
（选覆盖事件位更多的模式，同分优先 MOUNT）。最终选中的那一种在 `sfa-server.c:181-185` 下发，
此后事件范围由内核决定。

**实测（本仓库的 `--probe` 自己的输出）**：

```
事件位覆盖率     : MOUNT 1/7, FILESYSTEM 7/7
mark 模式        : FAN_MARK_FILESYSTEM
```

即在这台内核上 `FAN_MARK_MOUNT` 只支持 7 个事件位里的 1 个，**不能靠换标记形式缩小监视范围** ——
这不是配置问题，是内核能力问题。

**实测（无关事件确实到了客户端）**：代理只被要求关注 `/tmp`，两个客户端都收到了另一个进程在
`/home/linsmod` 下写文件的事件：

```
CREATE       pid=296  path=/home/linsmod/.hermes/weixin/accounts/.e73484816cc7@im.bot.sync_....tmp
CLOSE_WRITE  pid=296  path=/home/linsmod/.hermes/weixin/accounts/...
MOVED        pid=296  path=/home/linsmod/.hermes/weixin/accounts/e73484816cc7@im.bot.sync.json  old=...
```

## 影响

**量级：全机任何一个进程写文件都会推给客户端，每条 4128 字节**（`sfa.h:52-61`）。构建
（`make -j`）、包管理器、日志轮转、`/tmp` 里的临时文件都在内。

**实测比例（我们自己的计数器）**：客户端在索引根之外收到的事件占多少，是可以量的 —— 我们统计
`outside the root`。一次 100 内 / 100 外的对照：

```
watch: a batch of events applied 100 mark(s) over 1 directory -> 100 added, 0 removed,
       0 refreshed in 0.4 ms (196 events so far, 96 outside the root, 0 not in the index)
```

49% 被丢掉，丢掉的那部分是 96 × 4128 ≈ 387 KiB 的消息流量，换来零个结果。**这是我们构造的
fixture，比例是 fixture 的形状，不是繁忙机器的形状** —— 真实数字要在真实机器上量，办法见下面
「决策记录」。

**不只是浪费。** #1 里客户端 socket 名义上装得下 103 条消息，噪声直接吃掉这块缓冲，于是
**无关事件把相关事件挤掉**。#1 建议的补发 `OVERFLOW` 能防止卡死，但被挤掉的事件照样丢 —— 也就是说
#1 的修复质量直接受这份噪声影响：噪声越大，一个正常客户端被判 desynced 的概率越高。

**不修的后果。** 单实例部署下每个特权实例都要为整块文件系统的流量付出 socket 带宽和内核队列
内存，多实例时按倍数放大。

## 建议

拆成两条，优先级不同。

### 服务端 `--prefix`（建议做）

`./sfa-server / --prefix /usr --prefix /home`，可重复；在 `broadcast` 之前做前缀比较。

**为什么先做这条。** 成本接近零（一次 `strncmp`），对所有订阅者都有效，而且直接缓解 #1 的缓冲
压力 —— 先做它，#1 的重测结果才干净。

**代价与不覆盖的情况：**

1. 不同订阅者想要不同前缀时只能起多个代理，而每个代理都是一个特权进程。这是本方案的真实代价。
2. `--prefix` 是**代理级**的，写死在命令行里；客户端换了订阅范围要重启代理。
3. 前缀比较必须按**路径分量**做，不能裸 `strncmp`：`/usrlocal` 不该被 `/usr` 收进来。这件事我们
   自己在客户端里做错了就会错，所以这一条值得在实现时连带写个断言。

### 协议过滤（建议暂不做）

在 `sfa_subscribe_req` 里带前缀。三个理由，其中两个是上一版就有的、这次更强：

1. **会让所有现有客户端硬失败。** `sfa_connect()` 在 `libsfa.c:31-35` 严格校验
   `SFA_PROTO_VERSION`，不匹配直接返回 `EPROTO`，没有协商也没有兼容路径。升版本等于全量重连。
2. **形状不对。** `struct sfa_subscribe_req` 目前只有 8 字节（`sfa.h:64-67`），塞进
   `char prefix[SFA_MAX_PATH]` 会让每次订阅请求涨到 4104 字节，而实际前缀用不到几百字节。真要做，
   应该是变长的第三种消息类型，而不是把 prefix 塞进定长结构。
3. **收益仍未测量。** 见下面的决策记录。

**决策记录。** 若将来重开协议过滤这条，前置条件是：先在真实机器上量出前缀过滤能滤掉多少比例的
事件。**这个测量现在是可做的**，因为客户端已经有一个逐会话累加的 `outside the root` 计数，每个
apply 批次都会打出来：

```sh
grep -o '([0-9]* events so far, [0-9]* outside the root' serve.log | tail -1
```

量出来是 5%，就把服务端 `--prefix` 做足、协议这条路就此关掉；量出来是 80%，那才值得讨论升版本。

## 附注

- **esidx 侧现在怎么做**：客户端自己按路径分量做前缀比较，根之外的路径计入 `outside` 计数而不标脏
  （`watch.c` 的 `under_root()` / `mark_parent()`）。这是已发布的行为，`./test_watch.sh` 有一条断言
  专门用 fixture 里的一个诱饵文件验证「根外事件被计数为 outside 而不是被标脏」。所以：
  **正确性不受这份噪声影响**，本篇的诉求是带宽、延迟与 #1 的重测质量。
- **一条 README 该补的话**：`FAN_MARK_FILESYSTEM` 是 `FAN_MARK_MOUNT` 被内核拒绝时的**降级路径**，
  不是等价替代 —— 它覆盖范围更宽（就是本篇的现象），且可能触发不支持 exportfs 的后端。当前 README
  只在能力表里列了 mark 能力，没有说明这个差别；上面 1/7 对 7/7 的对比就是差别的具体量级。
- **相关**：#1（噪声吃掉缓冲，放大 desync 的概率）。
- **不在本 issue 范围内**：按 inode 或按挂载点订阅（会改变协议形状）；`FAN_MARK_MOUNT` 在支持它的
  内核上作为更窄范围的默认（那会让本篇在部分机器上自动消失，值得在改 mark 选择逻辑时一并考虑）。

## 维护者决定（2026-10-06）

STATUS: **split**。本篇保留服务端 `--prefix`（`accepted`），协议级过滤拆出为 #6（`deferred`）。

REASON: `ISSUE_GUIDELINES.md` 第 1 条要求「能独立讨论的两件事拆成两份」，本篇正文自己
也写着「拆成两条，优先级不同」。两半的结论不同 —— `--prefix` 的代价接近零且对所有订阅者有效；
协议过滤要动 `sfa_subscribe_req` 的形状，而 `sfa.h:64-67` 是定长 8 字节结构，塞进
`char prefix[SFA_MAX_PATH]` 会让每次订阅请求涨到 4104 字节。绑在一起的结果是：
便宜的方案被贵的那半拖住，而它们本来可以各自推进。

NEXT（验收标准，本篇保留的这一半）：

1. `./sfa-server / --prefix /usr --prefix /home` 可重复指定，在 `broadcast` 之前裁剪；
2. 前缀比较**按路径分量**，不能裸 `strncmp` —— `/usrlocal` 不该被 `/usr` 收进来，
   实现时附一条断言；
3. 不给 `--prefix` 时行为与现在完全一致；
4. `README.md` 补一句：`FAN_MARK_FILESYSTEM` 是 `FAN_MARK_MOUNT` 被内核拒绝时的**降级路径**，
   覆盖范围更宽，且可能触发不支持 exportfs 的后端 —— 能力表里现在只有 mark 能力，
   没说明这个差别；
5. 重跑 #1 的复现脚本，分开标注「修完 `send` 且装了 `--prefix`」与「只修完 `send`」，
   好让噪声对缓冲的影响可见。

**排序**：在 #3 之后、#1 之前。理由是它成本近零，且直接减轻 #1 的缓冲压力 ——
先做它，#1 的重测数据才干净。

拆出去的那一半见 #6，**不在本篇范围内，不要在这里实现协议过滤**。

## 修复与实测（2026-10-06）

已实现并验证，环境 WSL2 `6.18.40.1-microsoft-standard-WSL2`。因为 #3 要重写 CLI 解析，
两处改动共用同一个解析器，**一次性做完比改两遍省一轮回归** —— 这是本篇排到 #1 之前的另一个理由。

| 验收标准 | 实测 | |
|---|---|---|
| 1. `--prefix` 可重复、在 `broadcast` 前裁剪 | `--prefix "$D/mnt"` 下客户端收到 3 条前缀内事件 | 通过 |
| 2. **按路径分量比较**，附断言 | 内置 `--selftest` 10/10 通过，含 `/usr` vs `/usrlocal`、`/usr2`、`/a/b` vs `/a/bc`；另加诱饵实测：`$D/mnt2/decoy.txt` 未被 `$D/mnt` 收进来 | 通过 |
| 3. 不给 `--prefix` 行为不变 | 无选项时照常收到 `CREATE\|CLOSE_WRITE\|ATTRIB` | 通过 |
| 4. README 补`FAN_MARK_FILESYSTEM` 是降级路径 | 已补，并附 `MOUNT n/7, FILESYSTEM m/7` 的对照 | 通过 |
| 5. 重跑 #1 复现脚本、分开标注 | **未做** —— #1 尚未修，先修 #1 才有对照 | 未完成 |

实现补充两条正文没提的：

1. **相对前缀直接拒绝**（退出码 1）。事件路径全部来自 `/proc/self/fd` 的 readlink，
   一定是绝对路径；给个相对前缀会一条都不匹配，而这种失败是静默的。
2. **无路径事件不参与前缀过滤**。`SFA_EV_OVERFLOW`（以及 #2 将要加的
   `SFA_EV_UNRESOLVED`）是「有东西丢了」的唯一通知，按前缀挡掉等于把静默丢失重新引进来。
   宁可多投一条信号，不可少投。

**改动未提交** —— `commit` 字段留空，等提交后回填。