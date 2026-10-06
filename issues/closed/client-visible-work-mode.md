---
id: 9
status: closed
type: feature
priority: P2
created: 2026-10-06
closed: 2026-10-06
commit: 20eafeb
verdict: 复用 sfa_welcome.reserved（改名 flags）携带 SFA_WF_*，sizeof 与版本号不变；客户端实测可见 FILESYSTEM|RENAME_PAIR|ONDIR|PATH_LOOKUP，六项验收全过
---

# 客户端看不到服务端的工作模式：连接后不知道监控范围有多大

> 提出者：维护者本人。读代码 + `--probe` 输出得出；`sfa_welcome` 只带 version 与 mount。

## 现象

`struct sfa_welcome`（`sfa.h:70-74`）只有三项：

```c
struct sfa_welcome {
    uint32_t version;
    uint32_t reserved;              /* 从未使用，服务端不填 */
    char     mount[SFA_MAX_PATH];   /* 被监控的挂载点 */
};
```

而服务端实际协商出的工作模式只打在**服务端自己的 stderr** 上（`sfa_probe_print()`），
客户端拿不到：

| 客户端想知道 | 现在从 welcome 能得到吗 |
|---|---|
| 监控范围是挂载点还是整个文件系统（`FAN_MARK_MOUNT` / `FAN_MARK_FILESYSTEM`） | 不能 |
| rename 是单条 `SFA_EV_MOVED` 还是需要自己配对 `MOVED_FROM`/`MOVED_TO` | 不能 |
| 目录自身的事件（`FAN_ONDIR`）是否可见 | 不能 |
| 服务端是否启用了 `--prefix` 裁剪 | 不能 |
| 路径反解能力（`open_by_handle_at`）是否可用 | 不能 |

### 为什么这是个问题

`FAN_MARK_FILESYSTEM` 是 `FAN_MARK_MOUNT` 被内核拒绝或覆盖不全时的**降级路径**，
覆盖范围是整个文件系统，而不只是命令行给的那个目录（README 能力表已注明）。
于是在本机（WSL2 `6.18`，`MOUNT 1/7, FILESYSTEM 7/7`）实际跑的是 FILESYSTEM——
`./sfa-server /tmp/x` 会推送整个文件系统的事件，客户端却以为自己在看 `/tmp/x`。

对索引类客户端这不是「知道了更好」的细节，而是**判断事件是否可信的依据**：

- FILESYSTEM 模式下，落在监控目录之外的路径**不是异常**，是预期；
- `SFA_EV_MOVED` 缺失意味着 rename 要靠客户端自己配对两条事件（协议允许）；
- `FAN_ONDIR` 缺失意味着**永远不会**收到目录自身的事件，客户端不能把「没收到 rmdir」当成「目录还在」。

这些都能从「收不到某类事件」反推，但反推是猜 —— 客户端宁可在连接那一刻就知道。

## 建议

**复用 `sfa_welcome.reserved`，不新增字段、不升版本。**

- `sizeof(struct sfa_welcome)` 不变（4104 字节：8 字节头 + 4096 字节路径，实测确认），
  老客户端读到的字节数与解析方式不变；
- 老客户端把非零值当垃圾忽略即可，不崩、不影响已有行为——这与 #2 用位掩码扩展的
  代价同级（#7 的判据表：不改结构 = 0 代价），而不是 `sfa_event` 那种「改结构 = 全量重连」；
- #7 的约束「加了 reserved 就要有明确的填充规则」在这里是直接回答：**服务端按实测协商结果
  置位，未识别的位由客户端忽略**，且 `flags == 0` 明确表示「旧服务端，未报告」。

新增 `SFA_WF_*` 位（`sfa.h`，紧接 `sfa_welcome`）：

```c
#define SFA_WF_MARK_MOUNT      0x0001u  /* 监控范围 = 挂载点 */
#define SFA_WF_MARK_FILESYSTEM 0x0002u  /* 监控范围 = 整个文件系统（降级路径） */
#define SFA_WF_RENAME_PAIR     0x0004u  /* SFA_EV_MOVED 单条带旧+新路径 */
#define SFA_WF_ONDIR           0x0008u  /* 目录自身的事件可见 */
#define SFA_WF_PREFIX_FILTER   0x0010u  /* 服务端已按 --prefix 裁剪 */
#define SFA_WF_PATH_LOOKUP     0x0020u  /* open_by_handle_at 可用 */
```

SDK 侧需要一个不破坏现有 API 的入口：`sfa_connect()` 的签名不能动（客户端已编译产物
与源码调用点都在用），新增 `sfa_connect2(path, struct sfa_welcome *out)`，`out` 可为
`NULL`，`sfa_connect()` 变成它的一行包装。附一个 `sfa_work_flags_str()`（与
`sfa_event_names()` 同风格）便于日志打印。

`mount` 字段一并暴露给客户端——它一直就在 welcome 里，只是 SDK 丢掉了。

### 不做什么

- **不把事件掩码塞进 welcome**：`mask` 是 8 位事件集合，4 字节的 flags 装不下
  （已用 6 位给模式），而它属于 #7 讨论的「结构化能力协商」，一旦要做就该按 #7 的
  结论走，不在本篇夹带。
- **不升 `SFA_PROTO_VERSION`**：结构未变，升版本只会让全部现有客户端硬失败
  （`libsfa.c:31-35` 严格相等）。
- **不引入能力查询往返**：#7 候选 3 的形态，成本比复用 reserved 高一个数量级。

## 验收标准

1. `SFA_WF_*` 定义在 `sfa.h`，`sfa_welcome.reserved` 改名为 `flags`，
   `sizeof(struct sfa_welcome)` 与 `SFA_PROTO_VERSION` 均不变；
2. 服务端按协商结果填充：本机应得到 `MARK_FILESYSTEM | RENAME_PAIR | ONDIR | PATH_LOOKUP`，
   带 `--prefix` 时额外有 `PREFIX_FILTER`；
3. `sfa_connect2()` 可取到 welcome（含 `mount` 与 `flags`），`sfa_connect()` 行为不变；
4. `sfa_client` 连接时打印 watch 路径与模式（人可读）；
5. `SFA_WF_MARK_MOUNT` 与 `SFA_WF_MARK_FILESYSTEM` 互斥且必有其一（新服务端）；
6. README 的协议表与 SDK 说明同步。

## 修复与实测（2026-10-06）

已实现，环境 WSL2 `6.18.40.1-microsoft-standard-WSL2`（root）。

| 验收标准 | 实现 / 实测 | |
|---|---|---|
| 1. `SFA_WF_*` 在 `sfa.h`；`reserved` 改名 `flags`；`sizeof` 与 `SFA_PROTO_VERSION` 不变 | 实测 `sizeof(struct sfa_welcome) = 4104`（与改动前一致）；`SFA_PROTO_VERSION` 未动 | 通过 |
| 2. 服务端按协商结果填充，`--prefix` 时多一位 | 客户端实测输出 `mode=FILESYSTEM\|RENAME_PAIR\|ONDIR\|PATH_LOOKUP`；加 `--prefix` 后 `...\|ONDIR\|PREFIX_FILTER\|PATH_LOOKUP` | 通过 |
| 3. `sfa_connect2()` 可取 welcome（含 mount 与 flags），`sfa_connect()` 行为不变 | `sfa_connect()` 现为其一行包装；示例客户端改为 `sfa_connect2()` 并打印 `watch=` | 通过 |
| 4. `sfa_client` 连接时打印 watch 与模式 | `[client] watch=/tmp/.../mnt mode=FILESYSTEM\|RENAME_PAIR\|ONDIR\|PATH_LOOKUP` | 通过 |
| 5. MOUNT 与 FILESYSTEM 互斥且必居其一 | 新增 `selftest_work_flags()`（`--selftest` 一并跑）：互斥、`--prefix` 置位、能力缺失不置位，全过 | 通过 |
| 6. README 协议表与 SDK 说明同步 | 已改 | 通过 |

回归：`make check` 通过；三条 e2e（`test_delete_paths.sh` / `test_slow_client.sh` /
`test_unresolved.sh`）全过，无退化。未在 ext4 裸机复测。

一处副产品：本次 e2e 在 FILESYSTEM 模式下跑，`/tmp` 上其他活动的噪声事件也被计入
（慢客户端脚本里 B 收到 14503→31028 条），正好是本篇动机的现场演示——客户端以为在看
`$TMP/mnt`，实际收到的是整个文件系统的动静。
