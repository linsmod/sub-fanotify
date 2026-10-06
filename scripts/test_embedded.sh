#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
# e2e（issue #10）：把 server 库嵌进调用方进程，与普通客户端互操作。
#
# 验证四件事：
#   1. libsfa-server.a 可直接链接（调用方自己的 main 与库不冲突）；
#   2. 内嵌实例能起服务，标准 sfa_client 能连上并收到事件；
#   3. sfa_srv_stop() 在信号处理器里调用可让 sfa_srv_run() 正常返回（退出码 0）；
#   4. 退出时 socket 被清理（sfa_srv_close 回收）。
#
# 需要 root（fanotify）。
set -u
cd "$(dirname "$0")/.."

TMP=$(mktemp -d /tmp/sfa-embed.XXXXXX) || exit 1

# 只 kill/wait 自己起过的 PID：裸 `wait` 会等所有子进程，而客户端是长驻进程，
# 会让脚本永远不返回（踩过）。
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

# 内嵌 demo 只用 libsfa.a + 头文件（不经 CLI、不链接 sfa-server.c）
cc -O2 -Wall -Wextra -I. -o "$TMP/demo" scripts/embed_server_demo.c libsfa.a || {
    echo "FAIL: 无法链接 libsfa.a（内嵌调用方有自己的 main）"; exit 1; }

"$TMP/demo" "$MNT" "$SOCK" >"$TMP/demo.out" 2>"$TMP/demo.err" & D=$!
PIDS="$PIDS $D"
for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL: 内嵌实例未能起服务"; cat "$TMP/demo.err"; exit 1; }

# 标准客户端照常订阅（握手里的 welcome.flags 由内嵌实例填）
./sfa_client "$SOCK" >"$TMP/C.out" 2>"$TMP/c.err" & C=$!
PIDS="$PIDS $C"
sleep 0.5

for i in $(seq 1 20); do echo x > "$MNT/f$i"; done
sleep 1

EVENTS=$(grep -c "path=" "$TMP/C.out" 2>/dev/null || true)
MODE=$(grep -o 'mode=[^ ]*' "$TMP/c.err" 2>/dev/null | head -1)

# 信号停：处理器里调用 sfa_srv_stop()，run() 应正常返回且退出码 0
kill -TERM $D
wait $D; DRC=$?

SOCK_LEFT=$([ -e "$SOCK" ] && echo yes || echo no)
CLOSED=$(grep -c "已优雅退出" "$TMP/demo.err" 2>/dev/null || true)

echo "客户端收到事件：$EVENTS 条，握手 $MODE"
echo "内嵌实例退出码：$DRC，退出后 socket 残留：$SOCK_LEFT，优雅退出标记：$CLOSED"

FAIL=0
[ "$EVENTS" -ge 10 ] || { echo "FAIL: 客户端没收到事件（内嵌实例未正确投递）"; FAIL=1; }
[ "$DRC" -eq 0 ]     || { echo "FAIL: sfa_srv_stop() 未能让 run() 正常返回"; FAIL=1; }
[ "$SOCK_LEFT" = no ]|| { echo "FAIL: 退出后 socket 未清理"; FAIL=1; }
[ "$CLOSED" -ge 1 ]  || { echo "FAIL: 未走到 sfa_srv_close()"; FAIL=1; }

# ---- 验收 1：同进程两个实例，各自的 socket / 事件互不串味 ----
MNT2=$TMP/mnt2; SOCK2=$TMP/sfa2.sock
mkdir -p "$MNT2"
"$TMP/demo" "$MNT" "$SOCK" "$MNT2" "$SOCK2" >"$TMP/demo2.out" 2>"$TMP/demo2.err" & D2=$!
PIDS="$PIDS $D2"
for i in $(seq 50); do [ -S "$SOCK2" ] && [ -S "$SOCK" ] && break; sleep 0.1; done
if [ ! -S "$SOCK" ] || [ ! -S "$SOCK2" ]; then
    echo "FAIL: 同进程两个实例未能各自起服务"; cat "$TMP/demo2.err"; FAIL=1
fi

./sfa_client "$SOCK"  >"$TMP/C1.out" 2>/dev/null & C1=$!
./sfa_client "$SOCK2" >"$TMP/C2.out" 2>/dev/null & C2=$!
PIDS="$PIDS $C1 $C2"
sleep 0.5
for i in $(seq 1 10); do echo x > "$MNT/a$i"; done      # 只写第一个实例的目录
for i in $(seq 1 10); do echo x > "$MNT2/b$i"; done     # 只写第二个实例的目录
sleep 1

E1=$(grep -c "mnt/a" "$TMP/C1.out" 2>/dev/null || true)
E2=$(grep -c "mnt2/b" "$TMP/C2.out" 2>/dev/null || true)
# 注意：FILESYSTEM 模式（降级路径）覆盖整个文件系统，所以实例1 也可能看到
# mnt2 下的路径 —— 那是预期行为，不是隔离缺陷（见 welcome.flags 的 MARK_FILESYSTEM）。
# 隔离性看的是各自 socket/实例状态互不影响，所以这里只报告，不断言。
CROSS=$(grep -c "mnt2/" "$TMP/C1.out" 2>/dev/null || true)

echo "同进程双实例：实例1 自身事件 $E1 条，实例2 $E2 条"
echo "（实例1 另见同文件系统上 mnt2 的路径 $CROSS 条；FILESYSTEM 模式下属预期）"
[ "$E1" -ge 8 ] || { echo "FAIL: 实例1 未收到自己的事件"; FAIL=1; }
[ "$E2" -ge 8 ] || { echo "FAIL: 实例2 未收到自己的事件"; FAIL=1; }

kill -TERM $D2 2>/dev/null; wait $D2 2>/dev/null; DRC2=$?
[ "$DRC2" -eq 0 ] || { echo "FAIL: 双实例退出码 $DRC2（stop 未覆盖两个实例？）"; FAIL=1; }

# ---- #10 后续：进程内订阅（事件不经 socket，单进程不必连自己）----
MNT3=$TMP/mnt3; SOCK3=$TMP/sfa3.sock
mkdir -p "$MNT3"
"$TMP/demo" "$MNT3" "$SOCK3" --events >"$TMP/demo3.out" 2>"$TMP/demo3.err" & D3=$!
PIDS="$PIDS $D3"
for i in $(seq 50); do [ -S "$SOCK3" ] && break; sleep 0.1; done
sleep 0.3
for i in $(seq 1 10); do echo x > "$MNT3/c$i"; done
sleep 1

IN=$(grep -c "^EVENT " "$TMP/demo3.out" 2>/dev/null || true)
echo "进程内订阅（全程没有 socket 客户端）：收到事件 $IN 条"
[ "$IN" -ge 8 ] || { echo "FAIL: 进程内回调未收到事件"; cat "$TMP/demo3.err"; FAIL=1; }
kill -TERM $D3 2>/dev/null; wait $D3 2>/dev/null

[ $FAIL -eq 0 ] && echo "PASS: server 库可内嵌（socket 互操作 / 双实例隔离 / 进程内订阅），停止语义正常"
exit $FAIL
