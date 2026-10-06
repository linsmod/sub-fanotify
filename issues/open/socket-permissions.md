---
id: 3
status: accepted
type: bug
priority: P0
created: 2026-10-06
closed:
commit: 4fda81569092
verdict:
---

# socket 权限 0600 让「无特权索引器」连不上

> 提出者：esidx，sfa 的一个订阅方（只订阅、不 fork、不改 sfa）。`README.md:23-24` 把
> 「特权代理 + 无特权索引器」写成这个项目存在的理由，本篇说的是那句话在当前实现下不成立。

## 现象

`sfa-server.c:197`：

```c
chmod(sock_path, 0600);   /* 收紧权限；需要多用户访问时调整 */
```

`README.md` 的原话是 "The split is deliberate. The watcher needs `CAP_SYS_ADMIN`;
the indexer must not have it."（`README.md:23-24`），而 root 创建的 socket 是
`root:root 0600`，于是只有 root 能 `connect()`。

### 实测（对照组只差 socket 自身的 mode 与 group）

```sh
D=/tmp/sfa-perm-$$; mkdir -p "$D"; chmod 0755 "$D"      # 目录必须人人可进入，见下
./sfa-server /tmp "$D/sfa.sock" &
su -s /bin/sh sfauser -c "./sfa/sfa_client '$D/sfa.sock'"
```

```
1. 服务器当前留下的样子：
   srw------- 1 root root      /tmp/sfa-perm-1094847/sfa.sock
   sfauser: sfa_connect: Permission denied

2. 同一个 socket，chgrp 到客户端所属组 + chmod 0660：
   srw-rw---- 1 root sfa-test  /tmp/sfa-perm-1094847/sfa.sock
   [client] 已订阅 /tmp/sfa-perm-1094847/sfa.sock
   CREATE   pid=1094847  path=/tmp/sfa-perm-probe.txt
```

第二组是本篇诉求的正面验证：**同样的 socket、同样的客户端，只改 mode 与属组就能连上并收到事件**，
不需要改协议、不需要改客户端、不需要放宽到 world-readable。

> **一处更正（2026-10-06）**：本篇上一版给出的 `EACCES` 实测是错的 —— 它把 socket 放在
> `mktemp -d` 建出来的目录里，而那个目录是 `0700 root`，非特权用户**连目录都进不去**，
> 与 socket 权限无关。上面的对照组刻意用 `0755` 的目录，就是为了把这个变量排除掉。

## 影响

**这不是性能问题，是文档承诺的实现缺口。** 当前形态下「特权代理 + 无特权索引器」这个组合
**只在索引器也用 root 跑时成立**，而那正是这个项目想避免的。

**必然发生，不是概率事件。** 按 README 跑起来就会撞上，属于「照做就不对」。而且 socket 路径是可
覆盖的（`README.md:143-144`，`./sfa-server /data /tmp/sfa.sock`），所以这不是「默认路径碰巧在
`/run` 下」—— 运维为了把 socket 放到别处而覆盖路径时，撞上的一定是 `0600`。

**它挡住了下游的验证，而不只是部署。** 我们这一侧的 watcher 套件目前**整体以 root 运行**
（代理要 root，客户端也就顺带用了 root），而「客户端可以在非特权下工作」这件事因此从未被端到端
验证过。用 root 跑客户端还会让 #1 描述的那种慢客户端场景更难构造 —— 一个 root 客户端挂了，
代理是以「特权客户端也读不动」的方式挂掉，和普通用户挂掉不是同一个故障。

## 建议

给一个 `--group` 选项，启动时把 socket 的属组设成它：

```sh
./sfa-server / --group esidx            # 0660 root:esidx
```

**只做 `--group`，不做 `--mode 0666`。** 事件流会泄露全机文件名，任何用户可读不是能接受的默认值；
既然它不该做，就不必作为一个选项存在 —— 多一个选项就多一份文档和一次误用。

**代价。** socket 的属主永远是 root（代理本来就要特权），所以客户端用户必须属于该组；组的创建与
成员管理落在部署上，不在代理里。这是这个方案的真实代价，也是它比 `--mode` 好的地方。

### 连带影响（示例命令按现状跑不起来，这四条都会绊到实现）

1. **CLI 解析必须重写。** 现在是纯位置参数（`sfa-server.c:155-156`，`argv[1]`=mount、
   `argv[2]`=socket）。直接追加选项会让 `./sfa-server / --group esidx` 把 `--group` 当成 socket
   路径。需要在 `usage()`（`sfa-server.c:133-140`）与 `main` 开头一起改成真正的选项解析，
   **并保证 `--probe` 分支（`sfa-server.c:147-153`）不受影响** —— 它现在靠 `argv[1]` 认自己。
2. **`getgrnam()` 失败必须报错退出**，不能静默退化成 0600 —— 那正是本篇描述的形态复发。
3. **`chown` 不能省。** 只有 `chmod 0660` 而不改属组，客户端仍然连不上（上面的对照组里两步是一起做的）。
4. **`umask` 当前是安全的，但改成可配置后不能只依赖它。** bind 时权限是 `0777 & ~umask`，
   随后 `chmod` 覆盖，所以现状没问题；一旦权限可配置，必须保证最终那次 `chmod` 仍然生效。
5. **启动那行日志（`sfa-server.c:200`）应带上实际 mode 与属组**，例如
   `socket=/run/sfa.sock mode=0660 owner=root:esidx` —— 一行就够，但运维不必再去 `ls -l`。
6. **`README.md:23-24` 与 `README.md:91-92` 的用法示例需要同步**：现在示例里客户端与代理用同一个
   用户跑，读者照抄就得不到承诺的形态。

**默认行为不变。** 不给 `--group` 时保持 0600，现有单用户场景完全不受影响。

## 附注

- **esidx 侧现在怎么做**：`--watch` 的 socket 路径是显式参数（`--watch=PATH`），所以部署上可以把
  socket 放到一个客户端用户可读的目录，但 mode 仍由代理决定，我们改不了。
- **`SFA_SOCKET_PATH` 默认是 `/run/sfa.sock`**（`sfa.h:10`），该目录本身就需要 root 才能写入，
  所以默认路径与「代理必须特权」是一致的；`--group` 的价值在于配合自定义 socket 路径，让客户端
  不必是 root。
- **相关**：#1（用 root 跑客户端会掩盖慢客户端故障）。
- **不在本 issue 范围内**：systemd socket unit 与 socket 激活（那会把 0600 变成 unit 的 `SocketMode=`，
  是另一条更彻底的路线）；`SFA_PROTO_VERSION`；客户端侧的凭据或认证。

## 维护者决定（2026-10-06）

STATUS: accept，**排第一**

REASON: 这是四篇里唯一一处**文档明确承诺、实现明确没做**的缺口 —— `README.md:23-24`
把「indexer must not have it」写成项目存在的理由，而 `sfa-server.c:197` 把它取消掉了。
其余三篇要么是架构的必然结果（#1、#2），要么本来就没有承诺（#4）。代价也最小：
一处 CLI 解析重写 + `chgrp` + `chmod`，不动协议、不动事件路径、不动主循环。
按投入产出比，它是唯一一个「今天就能兑现 README 里已经写下的功能」的改动。

NEXT（验收标准）：

1. `./sfa-server / --group esidx` 后 `ls -l` 显示 `srw-rw---- root esidx`；
2. **非 root 用户**的 `sfa_client` 能连上并收到事件（不接受用 root 客户端验证 —— 那正是
   本篇指出的、掩盖了问题的跑法）；
3. 不给 `--group` 时仍是 `0600 root:root`，现有单用户场景行为不变；
4. `getgrnam()` 失败时**非零退出**，不静默退回 0600；
5. 启动日志一行带上实际 mode 与属组；
6. `README.md:23-24` 与 `:91-92` 的用法示例改为「代理 root、客户端普通用户」的形态。

连带影响清单（`--probe` 分支不受影响、chown 不可省、umask 仍以最终 chmod 为准）以本篇正文为准，
逐条在实现时对照。

## 修复与实测（2026-10-06）

已实现并逐条验证，环境 WSL2 `6.18.40.1-microsoft-standard-WSL2`、root、gcc。

实现范围：`sfa-server.c` 的 CLI 解析重写（位置参数与选项可混排、未知选项报错、
`--probe` 不再依赖 `argv[1]`）、`--group`、`--selftest`、`chown`+`chmod` 的顺序处理、
启动日志带 mode 与属组；`Makefile` 加 `selftest` 目标；`README.md` 的 split 段落、
用法示例、能力表降级说明同步。

| 验收标准 | 实测 | |
|---|---|---|
| 1. `--group` 后 `srw-rw---- root GROUP` | `srw-rw---- 1 root sfa-test 0 ... /tmp/.../s.sock` | 通过 |
| 2. **非 root** 客户端收到事件 | `sfauser`（属 `sfa-test`）连上，收到 5 行，含 `CREATE\|CLOSE_WRITE\|ATTRIB` 合并事件 | 通过 |
| 3. 不给 `--group` 仍是 `0600 root:root` | `srw------- 1 root root`，非特权 `sfa_connect: Permission denied` | 通过 |
| 4. `getgrnam` 失败非零退出 | `sfa-server: 未知组 nosuchgroup_zzz`，`exit=1` | 通过 |
| 5. 启动日志带 mode 与属组 | `socket=... mode=0660 owner=root:sfa-test` / 默认时 `mode=0600 owner=root:root` | 通过 |
| 6. README 示例改为代理 root、客户端普通用户 | 已改split 段落与 Usage | 通过 |

**一次真实的失败记录**：第一次跑第2 条时用了 `--group sfatest` 但客户端用户属于 `sfa-test`，
结果 `Permission denied`。这不是实现的问题 —— 它正好验证了本篇写下的代价
「客户端用户必须属于该组」。改用 `--group sfa-test` 后通过。**组员关系由部署负责，
代理不会也不应该替客户端加组**，这一点在实测里比在文字里清楚。

**尚未做的**：`SFA_PROTO_VERSION` 未变（正确，本篇不涉及协议）；systemd socket 激活
仍在本篇范围外。

**已提交** `4fda81569092`。**现象一节的行号以 `eff9956` 为基准，已失效** ——
`chmod` 那一行现在在 `sfa-server.c:353-355`（`chown` 在 353，`chmod 0660` 在 354，
默认 `chmod 0600` 在 355），启动日志在 `:362`，CLI 解析是 `parse_args()`。摘录仍然有效。