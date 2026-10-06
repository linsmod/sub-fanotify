#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
# e2e（issue #2）：rm -rf 一棵子树后，路径反解失败的事件不再静默丢弃 ——
# 客户端必须收到 UNRESOLVED 信号事件，服务端 stderr 必须有按批次聚合的
# 丢失计数（含 errno 分布）。
#
# 修复前的实测基线：500 文件的嵌套删除只送达 27%（139/511），OVERFLOW 为 0、
# 代理 stderr 为 0，客户端完全无从得知丢了事件。
#
# 需要 root（fanotify）。在支持 fanotify 的 Linux 上运行（WSL2 验证过）。
set -u
cd "$(dirname "$0")/.."

TMP=$(mktemp -d /tmp/sfa-e2e.XXXXXX) || exit 1

# 只 wait 自己起过的 PID：裸 `wait` 会等所有子进程，客户端是长驻进程时会挂住
PIDS=""
cleanup() {
    kill $PIDS 2>/dev/null
    wait $PIDS 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

MNT=$TMP/mnt
SOCK=$TMP/sfa.sock
mkdir -p "$MNT"

./sfa-server "$MNT" "$SOCK" 2>"$TMP/server.log" & SRV=$!
PIDS="$PIDS $SRV"
for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL: server 未能启动"; exit 1; }

./sfa_client "$SOCK" >"$TMP/C.out" 2>/dev/null & C=$!
PIDS="$PIDS $C"
sleep 0.5

# 500 个文件分布在 10 个目录里（嵌套形状，复现实测一的对照）
mkdir -p "$MNT/tree"
for d in $(seq 1 10); do
    mkdir -p "$MNT/tree/d$d"
    for i in $(seq 1 50); do echo x > "$MNT/tree/d$d/f$i"; done
done
sleep 1

rm -rf "$MNT/tree"       # 删除比「读事件 + 反解路径」快，父目录成批消失
sleep 2

UNRES=$(grep -c UNRESOLVED "$TMP/C.out" 2>/dev/null || true)
LOSS=$(grep -c "丢失" "$TMP/server.log" 2>/dev/null || true)
TOTAL=$(grep -c "path=" "$TMP/C.out" 2>/dev/null || true)

echo "客户端收到事件行：$TOTAL，其中 UNRESOLVED 信号：$UNRES"
echo "服务端 stderr 丢失计数行：$LOSS"

FAIL=0
[ "$UNRES" -ge 1 ] || { echo "FAIL: 客户端没有收到 UNRESOLVED 信号"; FAIL=1; }
[ "$LOSS"  -ge 1 ] || { echo "FAIL: 服务端 stderr 没有丢失计数"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "PASS: 路径反解失败对客户端可见"
exit $FAIL
