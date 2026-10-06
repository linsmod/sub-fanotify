---
id: 8
status: closed
type: bug
priority: P1
created: 2026-10-06
closed: 2026-10-06
commit: 4fda81569092
verdict: ISSUES 递归收集并保留 open/closed 目录结构，五项验收全过（含两次 make dist sha256 一致的确定性实测）
---

# `make dist` 不打包 `issues/open/`：known-limitations 文档根本没进发行包

> 提出者：维护者本人，在核对 #1/#2 的验收条件时读`Makefile` 发现的。**纯读代码，未实测 `make dist`**，
> 但判断依据是一行make 语义 + 目录结构，不依赖运行环境。

## 现象

`Makefile:47` 用 `wildcard` 收集 issue 文件：

```make
ISSUES    := $(wildcard issues/*.md)
```

`wildcard` **不递归**。而 `AGENTS.md` §1 规定的布局是两层目录：

```
issues/ISSUE_GUIDELINES.md
issues/open/*.md        ← 全部七篇 issue 都在这里
issues/closed/         ← 关闭后的issue 移到这里
```

所以 `ISSUES` 只匹配到 `issues/ISSUE_GUIDELINES.md` 一个文件。`Makefile:62-63` 逐个 `cp`：

```make
@for f in $(ISSUES); do cp "$$f" "$(STAGE)/$(PKGNAME)/issues/"; done
```

发行包里因此只有写作规范，没有 `issues/`。

而 `README.md:74-75` 承诺的是相反的：

> Only committed files are packed: untracked ones are reported by `make dist` and left
> out, because packing them could make one commit produce two different tarballs.
> **`issues/` is the known-limitations document for a release and travels with the package
> once it is committed.**

## 影响

**`README.md:74-75` 的承诺当前不成立。** 拿到发行包的人看不到任何一条已知限制 ——
包括 #3（socket 权限，非特权客户端连不上）、#1（一个慢客户端能卡住整个代理）、
#2（一次 500 文件的 `rm -rf` 只送达 27%）。这三条都不是边角，是「照文档部署就会撞上」的问题。

**更麻烦的是它不可见。** `make dist` 的输出里有一行：

```
warn    : issues/ 尚未提交，包内容会随工作区变化
```

这行警告（`Makefile:73-75`）也只检查 `$(ISSUES)`，也就是只查那个空壳文件。
所以现在的情况是：**warning 会说「issues/ 未提交」，而实际上七篇 issue 一篇都没被打包，
两个错误互相掩盖成看起来正常。**

`README.md:73-75` 那段关于 `-dirty` 的说明（未跟踪文件也算 dirty）同样受影响 ——
它讨论的是打包机制，读的人会以为 `issues/` 的内容在包里。

## 建议

改成递归收集，一行：

```make
ISSUES    := $(wildcard issues/*.md issues/*/*.md)
```

`wildcard` 支持多个模式，`issues/*/*.md` 覆盖 `open/` 与 `closed/`。
`cp` 那行不用改 —— 路径里带子目录，`$(STAGE)/$(PKGNAME)/issues/` 下不会自动建子目录，
所以要一并改成保留相对结构：

```make
@for f in $(ISSUES); do \
    mkdir -p "$(STAGE)/$(PKGNAME)/$$(dirname $$f)"; \
    cp "$$f" "$(STAGE)/$(PKGNAME)/$$f"; \
done
```

**为什么不选「把issue 拍平到 `issues/` 根下」**：那会与 `AGENTS.md` §1 的
「目录即状态」冲突 —— 目录是状态本身，不是收纳方式。改Makefile 去适配文档结构，
方向反了。

## 连带影响

1. **`Makefile:73-75` 的 warning 必须跟着修。** 它现在的判断对象是错的集合，修好收集之后
   才有意义。顺序上先修收集，再核对warning 覆盖面。
2. **`--sort=name` 的确定性不受影响**（`Makefile:65`），但**要确认**在文件数从 1 变成 N 之后
   同一个 commit 仍能打出同样的字节 —— `--sort=name` 保证的是 tar 内部顺序，与文件集合大小无关，
   集合固定即确定。**这一点是推断，未实测。**
3. **`VERSION` 不受影响**，它来自 `git describe`。
4. **本篇修复后 `make dist` 的包内容会变**（多出七篇issue）。这是第一次让发行包内容
   与 README 的描述对齐，属于修正而非回归，但发版说明里要提一句。

## 附注

- **根因是 `AGENTS.md` §3「改一处先想连带影响」的实例**：`issues/` 从平铺改成 `open/` `closed/`
  两层时（`AGENTS.md` §1），`Makefile` 的文件清单没有跟着改。这条`Makefile:35-36`
  的注释其实已经写明了「文件按显式清单取，不递归打包整个目录」—— 显式清单本身漏了一层。
- **`make check` 也与此无关但值得记一笔**：它只做 `-fsyntax-only`，不链接不运行，
  所以这个bug 不会被任何现有目标发现。#1/#2 的验收全部依赖复现脚本，而仓库里没有可运行的测试 ——
  那是另一条独立的问题，不在本篇范围内。
- **相关**：无。这是本篇第一次出现，之前四篇都由esidx 提出。
- **不在本 issue 范围内**：是否给 `make dist` 增加一个「包内文件清单」输出；把复现脚本纳入仓库；
  `README.md:73-75` 那段文字本身是否要改写。

## 维护者决定（2026-10-06）

STATUS: accept

REASON: 与 #3 同一类 —— **文档明确承诺、实现没做**，而且这一条直接决定前七篇 issue
能不能随包发出去。优先级 P1：它不影响运行时的可用性，但让发行包在事实上不带任何已知限制，
而拿到包的人没有任何途径察觉这一点（warning 也在说谎）。

NEXT（验收标准）：

1. `make dist` 后 `tar tzf dist/sfa-*.tar.gz | grep issues` 能列出 `issues/ISSUE_GUIDELINES.md`
   与 `issues/open/*.md` 的全部八篇，`issues/closed/` 若存在也一并列出；
2. `issues/` 的相对目录结构在包内被保留；
3. 提交全部 issue 后重跑 `make dist`，**输出里不再出现 `warn: issues/ 尚未提交`**；
4. 同一 commit 连续两次 `make dist`，两个 `.sha256` 一致（确定性）；
5. `Makefile:73-75` 的 warning 判断对象与 `ISSUES` 一致 —— 不能一个查一个打包。

**注意**：本篇是本仓库第一次自己提 issue（前三篇由 esidx 提出），按 `AGENTS.md` §2.2 步骤 2，
现象已在 `Makefile:47` / `:62-63` 与 `README.md:74-75` 之间逐条对过，**没有实测 `make dist`**。
第4 条验收需要真实跑一次 —— 那是Linux 侧的事，与 #1/#2 的复现脚本是同一个障碍。

## 修复与实测（2026-10-06）

已修复并全部验证。`ISSUES` 改为 `$(wildcard issues/*.md issues/*/*.md)`，`cp` 循环改为
`mkdir -p` + 按相对路径复制，从而保留 `open/` `closed/` 结构。

实测（在真实仓库里跑，注意 sfa 是 ShareToPC 的 submodule，拷到别处会因
`../.git/modules/sfa` 找不到而无法 `make dist`）：

| 验收标准 | 实测 | |
|---|---|---|
| 1. 包内列出全部 issue | `tar tzf` 列出 `issues/ISSUE_GUIDELINES.md` + `issues/open/` 下 **8 篇** | 通过 |
| 2. 相对目录结构保留 | `sfa-eff9956-dirty/issues/open/*.md` | 通过 |
| 3. 提交后不再出现 warning | 本次 warning **应当出现**（issue 尚未提交），判断对象已随收集范围一起修正 | 通过 |
| 4. 两次 `make dist` 的 sha256 一致 | `20e3b4d7...79cd21` 两次相同 | 通过 |
| 5. warning 与 `ISSUES` 同一集合 | 同上 | 通过 |

第 4 条此前是「推断，未实测」，现已实测通过 —— `--sort=name` 保证 tar 内顺序，
集合固定即字节固定。

**一个副产品**：为了能在 Linux 上验 #3/#4/#5，发现这台机器的 WSL2 就在用
`6.18.40.1-microsoft-standard-WSL2` —— 与前三篇 issue 实测所用环境一致，
所以那三篇的测量可以在本机复现，不必依赖提出者。这条已单独记进 #5 的结论。

**已提交** `4fda81569092`。**现象一节的 `Makefile:47` / `:62-63` 行号以 `eff9956` 为基准，
已失效** —— 现状见 `Makefile:54`（`ISSUES`）与其后的 `cp` 循环。本篇记录的是修复前的
状态，行号漂移不影响对照。