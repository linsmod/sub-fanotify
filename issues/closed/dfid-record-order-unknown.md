---
id: 5
status: closed
type: bug
priority: P3
created: 2026-10-06
closed: 2026-10-06
commit: d2083a2
verdict: find_fid_info 收紧为显式类型匹配（优先 DFID_NAME，退 DFID/FID），隐式记录顺序前提变为显式契约；新增 test_delete_paths.sh 一次性检查，e2e 实测 DELETE 40 条无 (deleted)
---

# `find_fid_info` 匹配到哪条 info 记录未定：DELETE 的路径可能带 ` (deleted)`

> **结论（2026-10-06 实测，假设 A 被否）**：40 个文件删除，40 条 `FAN_DELETE` 全部送达，
> 路径全部正确，` (deleted)` 出现 **0 次**。即 `find_fid_info(meta, 0)` 拿到的是
> `DFID_NAME`（父目录句柄 + 名字）而不是 `FID`，`is_dirent` 为 1，拼接分支是活代码。
> 本篇按预先写下的规则降级为 P3，只保留「收紧 `find_fid_info`」这一项。
> 实测数据见文末「维护者决定」。

> 提出者：维护者本人，在整理 #2 时从代码里读出来的。**正文所有 `文件:行号` 以 `eff9956`
> 为基准**；代码改动后行号会漂移，摘录才是判据。**下文正文保留提出时的假设原样，
> 不回改** —— 它记录的是「当时为什么值得查」，而结论另记在文末。判别方法见
> 「需要补的一个测量」，一次 `grep DELETE` 就能定论。

## 现象

非 rename 分支用 `find_fid_info(meta, 0)` 取 fid 记录，而这个 helper 把三种记录类型放在
同一个判断里，返回**第一条**匹配的（`sfa-server.c:86-90`）：

```c
if (type == 0) {
    if (h->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
        h->info_type == FAN_EVENT_INFO_TYPE_DFID ||
        h->info_type == FAN_EVENT_INFO_TYPE_FID)
        return (struct fanotify_event_info_fid *)p;
}
```

调用方据此决定「句柄是父目录 + 后面跟着名字」还是「句柄就是对象本身」（`sfa-server.c:275-276`）：

```c
int is_dirent = fid &&
    fid->hdr.info_type == FAN_EVENT_INFO_TYPE_DFID_NAME;
```

问题在于 `FAN_REPORT_FID` 是**协商出来的**（`sfa_probe.c:224`，仅在 `FAN_RENAME` 可用时并入
`init_extra`），而它一旦打开，内核会在同一条事件里多放一条 `FAN_EVENT_INFO_TYPE_FID`（对象自身的
句柄）。**这条记录与 `DFID_NAME` 谁排在前面，决定了上面那个 `is_dirent` 的取值。**

**假设 A（FID 在前）**：5.17+ 的默认路径上 `is_dirent` 恒为 0，`DFID_NAME` 的「父目录句柄 + 名字」
分支成为死代码。此时 `FAN_DELETE` 走的是**对象自身的句柄**，而 `/proc/self/fd/N` 对已 unlink 的
inode 返回的是 `/path/to/file (deleted)` —— 客户端收到一个带后缀的假路径。

**假设 B（`DFID_NAME` 在前）**：`is_dirent` 恒为 1，反解走父目录 + 名字，DELETE 的路径是对的；
此时 #2 里那个 27% 就只能由「父目录句柄过期」解释。

**两种假设都能解释 #2 实测一的 27%，所以现有测量判别不出来。** 这是本篇要解决的问题。

## 影响

若假设 A 成立：

- **客户端收到的 DELETE 路径不存在**，索引里那一行永远删不掉。这不是丢事件，是**收到一条
  指向错误对象的、看起来完全正常的事件** —— 比 #2 的静默丢失更难发现，因为日志里每一条都齐整。
- 影响面取决于 #2：#2 测的是「送达率」，本篇是「送达的对不对」。#2 的对照实验只数了行数，
  没有核对路径，所以即便这个假设成立，#2 的数据也不会暴露它。
- 量级未知。若成立，`FAN_MARK_FILESYSTEM` + `FAN_REPORT_FID` 的组合下是**每一次删除**都错。

若假设 B 成立：本篇降级为「`find_fid_info` 的宽松匹配在某条路径上从未被验证过」，
`is_dirent` 与 `DFID` / `FID` 两个分支实际上是未测试代码 —— 仍值得收紧，但不再是正确性问题。

**两种情况都不影响 #1 与 #2 的结论**，它们各自独立成立。

## 影响之外的判断

无论哪种假设成立，`find_fid_info(meta, 0)` 现在的形态都是隐患：把三种语义不同的记录用
「任意一种」匹配，等于把记录顺序这件内核细节变成了本实现的隐式前提。#2 的连带影响第 4 条
要求四处失败收敛到一个 helper，本篇应该**并进那个 helper 一起改**，而不是单独改。

## 建议

先测，再改。判别方法（Linux 上，一次就够）：

```sh
./sfa-server /tmp /tmp/sfa.sock &
./sfa_client /tmp/sfa.sock | grep -E 'DELETE|CLOSE_WRITE'
# 判据：DELETE 行的 path 末尾有没有 " (deleted)"
```

- 出现 ` (deleted)` → 假设 A 成立，按下面第 1 条修；
- 没有 → 假设 B 成立，只做第 2 条。

### 假设 A 成立时的修法

**不要靠「判断路径是否以 ` (deleted)` 结尾再截掉」来修。** 截掉能修好客户端拿到的路径，
但 `is_dirent` 仍然是错的：对 `FAN_CREATE` / `FAN_ATTRIB` / `FAN_CLOSE_WRITE`，走对象自身句柄
恰好是对的，对 `FAN_DELETE` 是错的 —— 一套逻辑在同一个分支里对两类事件给出不同语义，
靠字符串后缀去纠正是把这层不一致继续糊下去。

正确的做法是**显式区分两条路**：非 rename 分支只取 `DFID_NAME`（有名字的 dirent）与
`DFID` / `FID`（无名字的对象本身），并按内核给出的记录类型决定走哪条，
不再依赖「哪条先出现」。`FAN_DELETE` 若确实只给 `FID`，则**该事件没有可用路径**，
应当走 #2 的 `SFA_EV_UNRESOLVED` 信号，而不是伪造一个带后缀的路径。

### 两条假设下都要做的

**收紧 `find_fid_info`。** 要么拆成两个 helper（`find_fid_info_exact` 供 rename 分支用，
`find_dfid_name` 供非 rename 分支用），要么给它加一个明确的优先级并在注释里写明依赖记录顺序。
现状是两者都没有。

**补一句断言。** 按 `sfa-server.c:273-274` 已有注释的意图（非 rename 分支期望的是
「父目录句柄 + 名字」），取到 `FID` / `DFID` 时应该是一个可被观测到的异常，而现在是静默换语义。

### 连带影响

1. **`sfa_client.c:46` 把任何空 path 都打成 `(overflow)`。** 一旦 #2 的 `SFA_EV_UNRESOLVED`
   落地，它和 OVERFLOW 在示例客户端里**输出完全一样**，无法区分。#1 的连带影响第 4 条
   写「示例客户端不需要改」，在显示层面不准确 —— 要么改打印串，要么让 `sfa_event_name`
   的名字出现在输出里。
2. **#2 的实测需要在补测之后重跑。** 若假设 A 成立，#2 里「DELETE 丢失」那一栏的结论
   要改写成「DELETE 路径不可信」，两篇的优先级会重排。
3. **不影响 `SFA_PROTO_VERSION`。** 本篇是实现缺陷，不是协议变更。

## 附注

- **与 #2 的关系**：#2 量的是送达率，本篇量的是送达的对不对，两者在现有测量里无法互相印证。
  #2 建议的 `handle → path` 缓存（连带影响第 6 条）如果先落地，本篇的假设 A 会被绕开一部分 ——
  但 DELETE 事件的父目录恰好是被删掉的那个，缓存对它无效，所以不能靠那条路消掉本篇。
- **相关**：#2（静默丢失，本篇的前置背景）、#1（可用性，本篇不影响）。
- **不在本 issue 范围内**：路径反解失败本身（#2）；`FAN_EVENT_INFO_TYPE_FID` 事件的路径语义
  在内核侧的完整规则。

## 维护者决定（2026-10-06）

STATUS: ~~needs-info~~ → **accepted，优先级 P1 → P3**。假设 A 被实测否证。

实测（2026-10-06，WSL2 `6.18.40.1-microsoft-standard-WSL2`，即本篇提出者所用的同一环境）：

```
先建 40 个空文件，等代理与客户端就绪，然后一次性 rm 掉全部：
  DELETE 事件条数            40
  路径末尾含 " (deleted)"     0
  路径全部正确                是（/tmp/sfa-t5-*/mnt/f1 … f40）
  代理 stderr                 无
```

判据命中的是「没有 ` (deleted)`」这一侧，所以按本篇正文自己写下的规则走另一半：
**本篇降级为「未验证的宽松匹配」，只做 `find_fid_info` 的收紧，假设 A 那套改法不适用。**

结论的实际含义：`find_fid_info(meta, 0)` 在开启 `FAN_REPORT_FID` 时**仍然优先返回
`DFID_NAME`**，因此 `sfa-server.c:275-276` 的 `is_dirent` 在这个内核上恒为 1，
`DFID_NAME` 拼接分支不是死代码，DELETE 走父目录 + 名字因而路径正确。

**留下的风险是什么**（这才是本篇还要做的部分）：这个正确性依赖「`DFID_NAME` 排在
`FID` 之前」这个内核记录顺序，而 `find_fid_info` 的实现（三种类型混在同一次匹配里、
返回第一条）**没有把这个依赖写下来，也没有测**。换个内核、换个 fanotify 版本，
它可以静默翻转 —— 而表现是每一条 DELETE 的路径都带 ` (deleted)`，不报错、不崩、
日志齐整，只是内容错的。所以下一步只有一件事：把隐式前提变成显式契约。

NEXT（验收标准）：

1. 非 rename 分支不再用「任意 fid 记录」，改为按记录类型显式取：dirent 走 `DFID_NAME`，
   无名字的对象走 `DFID` / `FID`；
2. 在 `sfa-server.c` 的注释里写明依赖的记录顺序，并说明翻转后的症状；
3. 把本篇这段实测变成可重复执行的一次性检查（放进 `make selftest` 或独立脚本），
   使「顺序翻转」在改动后能被立刻发现；
4. 与 #2 的 helper 收敛一起做（#2 连带影响第 4 条），两者共用一个收敛点。

**给后来人的话**：这个否证本身是有价值的结论 —— 它把一条「每次删除都可能是假路径」
的疑似 P1 降成了「一个有文档的隐式前提」，但前提仍然要写下来。**不要因为假设被否就
删掉本篇**：否证的过程和判别方法比结论更耐用。
## 修复与实测（2026-10-06）

四项验收逐条落实，commit `d2083a2`：

| 验收标准 | 实现 / 实测 | |
|---|---|---|
| 1. 非 rename 分支按记录类型显式取 | `find_fid_info(meta, FAN_EVENT_INFO_TYPE_DFID_NAME)`，无则退 DFID、再退 FID；`is_dirent = (拿到的是 DFID_NAME)` | 通过 |
| 2. 注释写明依赖的记录顺序与翻转症状 | `find_fid_info` 重写为必须显式给类型，type=0 宽松分支已删除；注释写明旧实现隐式依赖 DFID_NAME 在前、翻转后症状是 DELETE 路径带 " (deleted)"、不报错不崩只是内容错，并指向本篇存档 | 通过 |
| 3. 实测判据变成可重复检查 | 新增 `scripts/test_delete_paths.sh`（40 文件先建后删，断言 0 条 "(deleted)"），随 `make e2e` 运行 | 通过 |
| 4. 与 #2 的 helper 收敛一起做 | #2 的 loss_stats 已在 bdc50bf 落地；本篇改动只动 fid 选取，loss 路径不受影响，回归通过 | 通过 |

实测（WSL2 `6.18.40.1-microsoft-standard-WSL2`，root）：新脚本 DELETE 40 条、"(deleted)" 0 条，
路径样例正确；`make check`、`selftest` 10/10、另两条 e2e 无退化。未在 ext4 裸机复测。

按「给后来人的话」保留本篇全部正文：否证的判别方法（看 DELETE 行末尾）比结论耐用。
